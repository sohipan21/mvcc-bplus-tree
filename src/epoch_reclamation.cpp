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

EpochSlot g_epoch_slots[kMaxThreads];

thread_local int tl_slot_index = -1;
thread_local EpochSlotGuard tl_epoch_slot_guard;

namespace {

// Ensures g_epoch_slots are filled with kEpochInactive exactly once.
// std::atomic<uint64_t> default-constructs to 0, not kEpochInactive,
// so we must explicitly initialize before any slot is claimed.
std::once_flag g_slots_init_flag;

void InitSlots() {
  for (int i = 0; i < kMaxThreads; ++i)
    g_epoch_slots[i].epoch.store(kEpochInactive, std::memory_order_relaxed);
}

// Retire queue — protected by g_retire_mutex.
// Reclamation bookkeeping uses a mutex (only the read traversal is lock-free).
struct RetiredItem {
  void* ptr;
  void (*deleter)(void*);
  uint64_t retired_epoch;
};

std::mutex g_retire_mutex;
std::vector<RetiredItem> g_retire_queue;

}  // namespace

// ---------------------------------------------------------------------------
// AcquireEpochSlot
// ---------------------------------------------------------------------------

int AcquireEpochSlot() {
  // Ensure slots are filled with kEpochInactive before first use.
  std::call_once(g_slots_init_flag, InitSlots);

  if (tl_slot_index >= 0) return tl_slot_index;  // already claimed

  for (int i = 0; i < kMaxThreads; ++i) {
    uint64_t expected = kEpochInactive;
    // Claim by swapping kEpochInactive → 0 (a valid but never-announced epoch).
    // We'll overwrite with the real snapshot in ThreadEnterEpoch.
    if (g_epoch_slots[i].epoch.compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
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
  // seq_cst: full fence so no tree-traversal load can be reordered before this
  // announcement. Without it a writer could observe no active readers, advance
  // the safe epoch, free a node, and then we'd dereference freed memory.
  g_epoch_slots[tl_slot_index].epoch.store(snapshot_ts, std::memory_order_seq_cst);
}

void ThreadExitEpoch() {
  if (tl_slot_index < 0) return;  // already inactive — idempotent exit is safe
  g_epoch_slots[tl_slot_index].epoch.store(kEpochInactive, std::memory_order_release);
  tl_slot_index = -1;  // slot is released; guard destructor at thread death will be a no-op
}

// ---------------------------------------------------------------------------
// ComputeSafeEpoch
// ---------------------------------------------------------------------------

uint64_t ComputeSafeEpoch() {
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

// Internal helper: enqueue with a typed deleter and the current global_version.
static void EnqueueRetired(void* ptr, void (*deleter)(void*)) {
  uint64_t epoch = global_version.load(std::memory_order_acquire);
  std::lock_guard<std::mutex> lock(g_retire_mutex);
  g_retire_queue.push_back({ptr, deleter, epoch});
}

// Spec-required signature. Since we don't know the type here, this path is only
// safe for plain-delete objects. Prefer DeferFreeNode for BPlusNode.
void DeferredFree(void* ptr) {
  EnqueueRetired(ptr, [](void* p) { ::operator delete(p); });
}

void DeferFreeNode(BPlusNode* node) {
  EnqueueRetired(node, [](void* p) { delete static_cast<BPlusNode*>(p); });
}

void DeferFreeVersionNode(VersionNode* node) {
  EnqueueRetired(node, [](void* p) { delete static_cast<VersionNode*>(p); });
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
  uint64_t safe = ComputeSafeEpoch();
  if (safe == 0) return;  // nothing can be safely freed yet

  std::lock_guard<std::mutex> lock(g_retire_mutex);
  if (g_retire_queue.empty()) return;

  std::vector<RetiredItem> survivors;
  survivors.reserve(g_retire_queue.size());

  for (auto& item : g_retire_queue) {
    // Free if retired before the oldest active reader's snapshot.
    // safe == kEpochInactive means no active readers — free everything.
    if (safe == kEpochInactive || item.retired_epoch < safe) {
      item.deleter(item.ptr);
    } else {
      survivors.push_back(item);
    }
  }

  g_retire_queue = std::move(survivors);
}
