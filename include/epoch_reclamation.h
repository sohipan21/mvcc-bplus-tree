#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

/// ---------------------------------------------------------------------------
/// Epoch-Based Reclamation (EBR)
///
/// How it works:
///   - Each reader announces its snapshot timestamp before traversing the tree.
///   - Writers defer node deletions instead of freeing immediately.
///   - safe_epoch = oldest announced snapshot across all active readers.
///   - A retired node is freed only when its retired_epoch < safe_epoch,
///     guaranteeing no live reader holds a pointer into it.
///
/// Usage:
///   Readers wrap every Search() call with ThreadEnterEpoch / ThreadExitEpoch.
///   Writers call AdvanceEpoch() after each mutation to trigger batched reclamation.
/// ---------------------------------------------------------------------------

static constexpr int kMaxThreads = 64;
static constexpr uint64_t kEpochInactive = std::numeric_limits<uint64_t>::max();

/// Per-thread epoch slot. alignas(64) prevents false-sharing between slots.
struct alignas(64) EpochSlot {
  std::atomic<uint64_t> epoch{kEpochInactive};
};

extern EpochSlot g_epoch_slots[kMaxThreads];

/// Thread-local slot index; -1 means not yet assigned.
extern thread_local int tl_slot_index;

/// RAII guard that releases the EBR slot back to the pool when a thread exits
/// without explicitly calling ThreadExitEpoch. Declared here so its destructor
/// body can reference g_epoch_slots and tl_slot_index without a forward decl.
struct EpochSlotGuard {
  ~EpochSlotGuard() {
    if (tl_slot_index >= 0) {
      g_epoch_slots[tl_slot_index].epoch.store(kEpochInactive,
                                               std::memory_order_release);
      tl_slot_index = -1;
    }
  }
};

extern thread_local EpochSlotGuard tl_epoch_slot_guard;

// ---------------------------------------------------------------------------
// Thread slot management
// ---------------------------------------------------------------------------

/// @brief Claim a per-thread EBR slot (idempotent after the first call).
/// @return The slot index in [0, kMaxThreads). Aborts if all slots are occupied.
int AcquireEpochSlot();

// ---------------------------------------------------------------------------
// Reader lifecycle — wrap every BPlusTree::Search() call
// ---------------------------------------------------------------------------

/// @brief Announce that this thread is reading at snapshot_ts.
/// Uses memory_order_seq_cst as a full fence so no tree-traversal load can be
/// reordered before the announcement — which would allow a concurrent writer to
/// reclaim a node the reader is about to dereference.
void ThreadEnterEpoch(uint64_t snapshot_ts);

/// @brief Signal that this thread has finished its read traversal.
void ThreadExitEpoch();

// ---------------------------------------------------------------------------
// Writer interface
// ---------------------------------------------------------------------------

/// @brief Compute the minimum announced epoch across all active slots.
/// @return The safe epoch, or kEpochInactive if no readers are active
///         (meaning all retired nodes are immediately reclaimable).
uint64_t ComputeSafeEpoch();

// Forward declarations for typed helpers (avoids delete-void* UB).
struct BPlusNode;
struct VersionNode;

/// @brief Enqueue ptr for deferred deletion (plain operator-delete).
/// Prefer the typed helpers below for objects with non-trivial destructors.
void DeferredFree(void* ptr);

/// @brief Enqueue a BPlusNode for deferred deletion via EBR.
void DeferFreeNode(BPlusNode* node);

/// @brief Enqueue a VersionNode for deferred deletion via EBR.
void DeferFreeVersionNode(VersionNode* node);

/// @brief Called by writers after each mutation to trigger batched reclamation.
/// Reclamation runs every kReclaimBatch calls to amortize the retire-queue scan.
/// Safe to call from any writer context (does not require write_mutex_).
void AdvanceEpoch();

/// @brief Free all retired entries whose retire_epoch < ComputeSafeEpoch().
/// Acquires g_retire_mutex internally; called by AdvanceEpoch() on a schedule.
void Reclaim();
