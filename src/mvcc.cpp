#include "mvcc.h"

#include <cassert>

#include "epoch_reclamation.h"

// ---------------------------------------------------------------------------
// Global version counter
// Incremented with memory_order_seq_cst inside CommitGuard so the claim
// announcement (seq_cst store) is totally ordered before the timestamp draw —
// the property BeginRead's watermark scan relies on.
// ---------------------------------------------------------------------------
std::atomic<uint64_t> global_version{0};

namespace {

// ---------------------------------------------------------------------------
// CommitGuard — the writer half of the commit watermark.
//
// Drawing a timestamp and publishing the version are two separate steps, so
// without coordination a reader could obtain snapshot >= ts while the write
// is still mid-publication, observe "no value", and later observe the value
// at the SAME snapshot — a non-repeatable read that tests/test_model_check.cpp
// flags as a snapshot-isolation violation.
//
// Protocol (writer):
//   1. commit_ts.store(kCommitClaiming, seq_cst)   "in flight, ts unknown"
//   2. ts = global_version.fetch_add(1, seq_cst)+1
//   3. commit_ts.store(ts, seq_cst)                "in flight at ts"
//   4. ... build + publish the version ...
//   5. commit_ts.store(kEpochInactive, release)    "fully published"
//
// BeginRead scans the slots after loading global_version: any writer whose ts
// could be <= the loaded value has already executed (2), hence (1) is seq_cst-
// ordered before the scan and MUST be observed — as kCommitClaiming (reader
// briefly spins; the claiming window is two instructions), as ts (reader caps
// its snapshot to ts-1), or as kEpochInactive from (5) (write fully published,
// safe to include). A snapshot therefore never covers a half-published commit.
// ---------------------------------------------------------------------------
class CommitGuard {
 public:
  CommitGuard() {
    slot_ = AcquireEpochSlot();
    g_epoch_slots[slot_].commit_ts.store(kCommitClaiming, std::memory_order_seq_cst);
    ts_ = global_version.fetch_add(1, std::memory_order_seq_cst) + 1;
    g_epoch_slots[slot_].commit_ts.store(ts_, std::memory_order_seq_cst);
  }

  ~CommitGuard() {
    g_epoch_slots[slot_].commit_ts.store(kEpochInactive, std::memory_order_release);
  }

  uint64_t ts() const { return ts_; }

 private:
  int slot_;
  uint64_t ts_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

MVCCTree::~MVCCTree() {
  std::lock_guard<std::mutex> lock(chains_mutex_);
  for (VersionChain* chain : chains_) delete chain;
}

// ---------------------------------------------------------------------------
// BeginRead
// ---------------------------------------------------------------------------

uint64_t MVCCTree::BeginRead() const {
  // Reader half of the commit watermark (see CommitGuard above): cap the
  // snapshot below the oldest in-flight commit so every timestamp <= snapshot
  // belongs to a fully published write.
  while (true) {
    uint64_t snap = global_version.load(std::memory_order_seq_cst);

    bool retry = false;
    uint64_t min_inflight = kEpochInactive;
    for (int i = 0; i < kMaxThreads; ++i) {
      uint64_t c = g_epoch_slots[i].commit_ts.load(std::memory_order_seq_cst);
      if (c == kCommitClaiming) {
        retry = true;  // ts being drawn right now; window is two instructions
        break;
      }
      if (c != kEpochInactive && c < min_inflight) min_inflight = c;
    }
    if (retry) continue;

    if (min_inflight != kEpochInactive && min_inflight - 1 < snap) snap = min_inflight - 1;
    return snap;
  }
}

// ---------------------------------------------------------------------------
// Insert — uses InsertOrGet to prevent duplicate VersionChains under concurrency.
// ---------------------------------------------------------------------------

void MVCCTree::Insert(uint64_t key, void* value) {
  {
    CommitGuard commit;  // holds the watermark until the version is published

    // Speculatively build a chain, then publish atomically. InsertOrGet installs
    // ours and returns nullptr when the key was absent; if another thread already
    // inserted this key (or it pre-existed) it returns the existing chain instead,
    // closing the Search-then-Insert race that could orphan a chain.
    auto* fresh = new VersionChain();
    fresh->Append(value, commit.ts());

    void* existing = tree_.InsertOrGet(key, fresh);
    if (existing == nullptr) {
      std::lock_guard<std::mutex> lock(chains_mutex_);
      chains_.push_back(fresh);
    } else {
      delete fresh;  // never published — no reader can reach it
      auto* chain = static_cast<VersionChain*>(existing);
      if (!chain->UpdateVersion(value, commit.ts())) chain->Append(value, commit.ts());
    }
  }
  AdvanceEpoch();
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void MVCCTree::Update(uint64_t key, void* value) {
  void* existing = tree_.Search(key);
  if (existing == nullptr) return;  // key not present — no-op, no ts consumed

  auto* chain = static_cast<VersionChain*>(existing);
  {
    CommitGuard commit;
    chain->UpdateVersion(value, commit.ts());
  }
  AdvanceEpoch();
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

bool MVCCTree::Delete(uint64_t key) {
  void* existing = tree_.Search(key);
  if (existing == nullptr) return false;  // no ts consumed

  auto* chain = static_cast<VersionChain*>(existing);
  bool deleted;
  {
    CommitGuard commit;
    deleted = chain->DeleteVersion(commit.ts());
  }
  AdvanceEpoch();
  return deleted;
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

void* MVCCTree::Read(uint64_t key, uint64_t snapshot_ts) const {
  // Hold the EBR epoch across both the tree traversal and the version chain
  // traversal so that PruneVersionNodes cannot free VersionNodes that are
  // still visible at snapshot_ts while we are reading them.
  ThreadEnterEpoch(snapshot_ts);
  void* existing = tree_.SearchRaw(key);
  void* result = nullptr;
  if (existing != nullptr) result = static_cast<VersionChain*>(existing)->Read(snapshot_ts);
  ThreadExitEpoch();
  return result;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

void MVCCTree::Scan(uint64_t lo, uint64_t hi, uint64_t snapshot_ts,
                    const std::function<void(uint64_t, void*)>& fn) const {
  // Hold the EBR epoch across the entire leaf-chain walk so PruneVersionNodes
  // cannot free version nodes that are visible at snapshot_ts mid-scan.
  ThreadEnterEpoch(snapshot_ts);
  tree_.Scan(lo, hi, [&](uint64_t key, void* chain_ptr) {
    void* v = static_cast<VersionChain*>(chain_ptr)->Read(snapshot_ts);
    if (v != nullptr) fn(key, v);
  });
  ThreadExitEpoch();
}

// ---------------------------------------------------------------------------
// Exists
// ---------------------------------------------------------------------------

bool MVCCTree::Exists(uint64_t key, uint64_t snapshot_ts) const {
  return Read(key, snapshot_ts) != nullptr;
}

// ---------------------------------------------------------------------------
// NumKeys
// ---------------------------------------------------------------------------

size_t MVCCTree::NumKeys() const { return tree_.Size(); }

// ---------------------------------------------------------------------------
// PruneVersionNodes
// ---------------------------------------------------------------------------

void MVCCTree::PruneVersionNodes() {
  // Oldest snapshot any active reader announced. If no readers are active,
  // prune up to the current global_version: every future reader's snapshot
  // will be >= it, so all versions tombstoned so far are unreachable.
  uint64_t safe = ComputeSafeSnapshot();
  if (safe == kEpochInactive) safe = global_version.load(std::memory_order_acquire);
  std::lock_guard<std::mutex> lock(chains_mutex_);
  for (VersionChain* chain : chains_) chain->Prune(safe);
}
