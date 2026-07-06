#include "epoch_reclamation.h"

#include <cassert>
#include <mutex>
#include <vector>

#include "btree.h"
#include "mvcc.h"
#include "version_chain.h"

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

// Structural clock starts at 1 so a freshly retired node (stamped >= 1) can
// never be confused with a zero-initialized announcement.
std::atomic<uint64_t> g_structural_epoch{1};

EpochSlot g_epoch_slots[kMaxThreads];

thread_local int tl_slot_index = -1;
thread_local EpochSlotGuard tl_epoch_slot_guard;

namespace {

// Ensures g_epoch_slots are filled with kEpochInactive exactly once.
// std::atomic<uint64_t> default-constructs to 0, not kEpochInactive,
// so we must explicitly initialize before any slot is claimed.
std::once_flag g_slots_init_flag;

void InitSlots() {
  for (int i = 0; i < kMaxThreads; ++i) {
    g_epoch_slots[i].epoch.store(kEpochInactive, std::memory_order_relaxed);
    g_epoch_slots[i].structural_epoch.store(kEpochInactive, std::memory_order_relaxed);
    g_epoch_slots[i].commit_ts.store(kEpochInactive, std::memory_order_relaxed);
  }
}

// Retire queues — protected by g_retire_mutex. Two queues because the two
// object kinds are stamped with DIFFERENT clocks (structural vs snapshot);
// comparing a structural stamp against a snapshot minimum would be meaningless.
struct RetiredItem {
  void* ptr;
  void (*deleter)(void*);
  uint64_t retired_epoch;
};

std::mutex g_retire_mutex;
std::vector<RetiredItem> g_retire_nodes;     // stamped with g_structural_epoch
std::vector<RetiredItem> g_retire_versions;  // stamped with global_version

// Frees every item whose stamp is older than `safe`; keeps the rest.
// safe == kEpochInactive means no active readers on that clock: free all.
void DrainQueue(std::vector<RetiredItem>& queue, uint64_t safe) {
  if (queue.empty()) return;
  std::vector<RetiredItem> survivors;
  survivors.reserve(queue.size());
  for (auto& item : queue) {
    if (safe == kEpochInactive || item.retired_epoch < safe) {
      item.deleter(item.ptr);
    } else {
      survivors.push_back(item);
    }
  }
  queue = std::move(survivors);
}

}  // namespace

// ---------------------------------------------------------------------------
// AcquireEpochSlot
// ---------------------------------------------------------------------------

int AcquireEpochSlot() {
  // Ensure slots are filled with kEpochInactive before first use.
  std::call_once(g_slots_init_flag, InitSlots);

  if (tl_slot_index >= 0) return tl_slot_index;  // already claimed

  for (int i = 0; i < kMaxThreads; ++i) {
    bool expected = false;
    // Claim via the dedicated ownership flag so a slot can be retained while
    // its epochs are kEpochInactive (idle between reads) without being stolen.
    if (g_epoch_slots[i].claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed)) {
      tl_slot_index = i;
      (void)tl_epoch_slot_guard;  // ensure destructor is registered for this thread
      return i;
    }
  }

  assert(false && "EBR: all kMaxThreads slots occupied; increase kMaxThreads");
  std::abort();
}

// ---------------------------------------------------------------------------
// ThreadEnterEpoch / ThreadExitEpoch
// ---------------------------------------------------------------------------

void ThreadEnterEpoch(uint64_t snapshot_ts) {
  AcquireEpochSlot();
  // acquire on the structural clock: pairs with the writer's release
  // fetch_add after a root swing, so announcing epoch E guarantees we see
  // every root published up through E — we can never reach a node retired
  // before E became current.
  uint64_t structural = g_structural_epoch.load(std::memory_order_acquire);
  // seq_cst stores: full fences so no traversal load can be reordered before
  // the announcements are globally visible. Without them a reclaimer could
  // observe no active readers, free a node, and we'd dereference freed
  // memory. (Reclaim side pairs with a seq_cst fence in ComputeSafe*.)
  g_epoch_slots[tl_slot_index].structural_epoch.store(structural, std::memory_order_seq_cst);
  g_epoch_slots[tl_slot_index].epoch.store(snapshot_ts, std::memory_order_seq_cst);
}

void ThreadExitEpoch() {
  if (tl_slot_index < 0) return;  // already inactive — idempotent exit is safe
  // Stop announcing but keep the slot claimed so the next read on this thread
  // skips the O(kMaxThreads) acquire scan in AcquireEpochSlot. Release stores:
  // a reclaimer that acquire-loads kEpochInactive gets a happens-before edge
  // over every traversal load we made, so freeing after that is race-free.
  // Full release (claimed=false, tl_slot_index=-1) happens at thread exit via
  // EpochSlotGuard.
  g_epoch_slots[tl_slot_index].epoch.store(kEpochInactive, std::memory_order_release);
  g_epoch_slots[tl_slot_index].structural_epoch.store(kEpochInactive, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// ComputeSafeStructuralEpoch / ComputeSafeSnapshot
// ---------------------------------------------------------------------------

// Both scans start with a seq_cst fence pairing with the seq_cst announcement
// stores in ThreadEnterEpoch (Dekker-style): either the scan observes a
// reader's announcement, or that reader's traversal loads are ordered after
// the scan — meaning the reader observed the post-retirement state and cannot
// hold a pointer into anything the scan approves for freeing.

uint64_t ComputeSafeStructuralEpoch() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  uint64_t safe = kEpochInactive;
  for (int i = 0; i < kMaxThreads; ++i) {
    uint64_t e = g_epoch_slots[i].structural_epoch.load(std::memory_order_acquire);
    if (e != kEpochInactive && e < safe) safe = e;
  }
  return safe;
}

uint64_t ComputeSafeSnapshot() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  uint64_t safe = kEpochInactive;
  for (int i = 0; i < kMaxThreads; ++i) {
    uint64_t e = g_epoch_slots[i].epoch.load(std::memory_order_acquire);
    if (e != kEpochInactive && e < safe) safe = e;
  }
  return safe;
}

// ---------------------------------------------------------------------------
// DeferredFree / DeferFreeNode / DeferFreeVersionNode
// ---------------------------------------------------------------------------

// Spec-required signature. Since we don't know the type here, this path is only
// safe for plain-delete objects. Prefer the typed helpers below.
void DeferredFree(void* ptr) {
  uint64_t epoch = global_version.load(std::memory_order_acquire);
  std::lock_guard<std::mutex> lock(g_retire_mutex);
  g_retire_versions.push_back({ptr, [](void* p) { ::operator delete(p); }, epoch});
}

void DeferFreeNode(BPlusNode* node) {
  // Stamp with the CURRENT structural epoch: the node was still reachable
  // during this epoch, so it may be freed only once every active reader has
  // announced a strictly newer one.
  uint64_t epoch = g_structural_epoch.load(std::memory_order_acquire);
  std::lock_guard<std::mutex> lock(g_retire_mutex);
  g_retire_nodes.push_back({node, [](void* p) { delete static_cast<BPlusNode*>(p); }, epoch});
}

void DeferFreeVersionNode(VersionNode* node) {
  uint64_t epoch = global_version.load(std::memory_order_acquire);
  std::lock_guard<std::mutex> lock(g_retire_mutex);
  g_retire_versions.push_back({node, [](void* p) { delete static_cast<VersionNode*>(p); }, epoch});
}

// ---------------------------------------------------------------------------
// AdvanceEpoch / Reclaim
// ---------------------------------------------------------------------------

// Batch counter: only trigger Reclaim() every kReclaimBatch writes.
// Profiling showed the retire queue was growing too large at 32; halving to 16
// keeps memory usage bounded without meaningful throughput impact.
static constexpr int kReclaimBatch = 16;
static std::atomic<int> g_write_count{0};

void AdvanceEpoch() {
  if (g_write_count.fetch_add(1, std::memory_order_relaxed) % kReclaimBatch == 0) Reclaim();
}

void Reclaim() {
  uint64_t safe_structural = ComputeSafeStructuralEpoch();
  uint64_t safe_snapshot = ComputeSafeSnapshot();

  std::lock_guard<std::mutex> lock(g_retire_mutex);
  DrainQueue(g_retire_nodes, safe_structural);
  DrainQueue(g_retire_versions, safe_snapshot);
}

size_t RetireQueueSize() {
  std::lock_guard<std::mutex> lock(g_retire_mutex);
  return g_retire_nodes.size() + g_retire_versions.size();
}
