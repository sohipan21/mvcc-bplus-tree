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
// Insert
// ---------------------------------------------------------------------------

void MVCCTree::Insert(uint64_t key, void* value) {
  uint64_t ts = global_version.fetch_add(1, std::memory_order_release) + 1;

  void* existing = tree_.Search(key);
  if (existing == nullptr) {
    VersionChain* chain = new VersionChain();
    chain->Append(value, ts);
    tree_.Insert(key, chain);
    std::lock_guard<std::mutex> lock(chains_mutex_);
    chains_.push_back(chain);
  } else {
    auto* chain = static_cast<VersionChain*>(existing);
    // UpdateVersion atomically checks for a live head, marks it deleted,
    // and prepends the new version — all under version_lock_.
    // If no live version exists, just append (insert-after-delete case).
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
  // Single timestamp for both the deletion of the old version and the
  // creation of the new one. This ensures no visibility gap:
  //   readers at ts   → see new version (created_at == ts <= ts)
  //   readers at ts-1 → see old version (deleted_at == ts > ts-1)
  // UpdateVersion atomically checks+marks+appends under version_lock_.
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
  // DeleteVersion atomically checks for a live head and marks it deleted
  // under version_lock_ — no TOCTOU race with concurrent writers.
  uint64_t ts = global_version.fetch_add(1, std::memory_order_release) + 1;
  bool deleted = chain->DeleteVersion(ts);
  AdvanceEpoch();
  return deleted;
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

void* MVCCTree::Read(uint64_t key, uint64_t snapshot_ts) const {
  void* existing = tree_.Search(key);
  if (existing == nullptr) return nullptr;
  return static_cast<VersionChain*>(existing)->Read(snapshot_ts);
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
// PruneVersionNodes — intended for a background thread.
// Computes the oldest active reader epoch from EBR, then walks
// all chains and frees VersionNodes whose deleted_at falls below it.
// ---------------------------------------------------------------------------

void MVCCTree::PruneVersionNodes() {
  uint64_t safe = ComputeSafeEpoch();
  if (safe == 0 || safe == kEpochInactive) return;
  std::lock_guard<std::mutex> lock(chains_mutex_);
  for (VersionChain* chain : chains_) chain->Prune(safe);
}
