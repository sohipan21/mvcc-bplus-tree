#include "mvcc.h"

#include <cassert>

#include "epoch_reclamation.h"

// ---------------------------------------------------------------------------
// Global version counter
// Incremented with memory_order_release so that all prior memory writes are
// visible to any reader who subsequently acquires the counter.
// ---------------------------------------------------------------------------
std::atomic<uint64_t> global_version{0};

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
  // acquire: guarantees we see all writes that were released before this load
  return global_version.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Insert — uses InsertOrGet to prevent duplicate VersionChains under concurrency.
// ---------------------------------------------------------------------------

void MVCCTree::Insert(uint64_t key, void* value) {
  uint64_t ts = global_version.fetch_add(1, std::memory_order_release) + 1;

  // Speculatively build a chain, then publish atomically. InsertOrGet installs
  // ours and returns nullptr when the key was absent; if another thread already
  // inserted this key (or it pre-existed) it returns the existing chain instead,
  // closing the Search-then-Insert race that could orphan a chain.
  auto* fresh = new VersionChain();
  fresh->Append(value, ts);

  void* existing = tree_.InsertOrGet(key, fresh);
  if (existing == nullptr) {
    std::lock_guard<std::mutex> lock(chains_mutex_);
    chains_.push_back(fresh);
  } else {
    delete fresh;  // never published — no reader can reach it
    auto* chain = static_cast<VersionChain*>(existing);
    if (!chain->UpdateVersion(value, ts)) chain->Append(value, ts);
  }
  AdvanceEpoch();
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void MVCCTree::Update(uint64_t key, void* value) {
  void* existing = tree_.Search(key);
  if (existing == nullptr) return;  // key not present — no-op

  auto* chain = static_cast<VersionChain*>(existing);
  uint64_t ts = global_version.fetch_add(1, std::memory_order_release) + 1;
  chain->UpdateVersion(value, ts);
  AdvanceEpoch();
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

bool MVCCTree::Delete(uint64_t key) {
  void* existing = tree_.Search(key);
  if (existing == nullptr) return false;

  auto* chain = static_cast<VersionChain*>(existing);
  uint64_t ts = global_version.fetch_add(1, std::memory_order_release) + 1;
  bool deleted = chain->DeleteVersion(ts);
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
  if (existing != nullptr)
    result = static_cast<VersionChain*>(existing)->Read(snapshot_ts);
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
  uint64_t safe = ComputeSafeEpoch();
  if (safe == 0 || safe == kEpochInactive) return;
  std::lock_guard<std::mutex> lock(chains_mutex_);
  for (VersionChain* chain : chains_) chain->Prune(safe);
}
