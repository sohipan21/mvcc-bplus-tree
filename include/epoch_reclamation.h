#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

/// ---------------------------------------------------------------------------
/// Epoch-Based Reclamation (EBR)
///
/// Two independent clocks drive reclamation, because the tree retires two
/// kinds of objects with different lifetimes:
///
///   1. g_structural_epoch — advanced by BPlusTree writers after every
///      copy-on-write structural publish (root swing). Retired BPlusNodes are
///      stamped with this clock. A reader announces the structural epoch it
///      observed before traversing; a retired node is freed only once every
///      active reader has announced a NEWER structural epoch (and therefore
///      sees the new root, never the retired node).
///
///   2. global_version (declared in mvcc.h) — the MVCC snapshot clock.
///      Retired VersionNodes are stamped with it. A version node is freed only
///      once no active reader's announced snapshot could still reach it.
///
/// Usage:
///   Readers wrap every traversal with ThreadEnterEpoch / ThreadExitEpoch.
///   Pure structural readers (BPlusTree::Search) pass kEpochInactive as the
///   snapshot; MVCC readers pass their snapshot timestamp.
///   Writers call AdvanceEpoch() after each mutation to trigger batched
///   reclamation (BPlusTree structural writers call Reclaim() directly).
/// ---------------------------------------------------------------------------

static constexpr int kMaxThreads = 64;
static constexpr uint64_t kEpochInactive = std::numeric_limits<uint64_t>::max();

/// Sentinel for EpochSlot::commit_ts: the writer has announced an in-flight
/// commit but has not yet drawn its timestamp. BeginRead must wait it out
/// (the claiming window is two instructions — announce, then fetch_add).
static constexpr uint64_t kCommitClaiming = 0;

/// Global structural-publish counter. Starts at 1; incremented (release) by a
/// structural writer AFTER the root swing and AFTER stamping retired nodes.
extern std::atomic<uint64_t> g_structural_epoch;

/// Per-thread epoch slot. alignas(64) prevents false-sharing between slots.
/// A reader announces BOTH clocks on entry:
///   - structural_epoch: the g_structural_epoch value observed at entry
///   - epoch:            the MVCC snapshot timestamp (kEpochInactive if the
///                       reader does not touch version chains)
struct alignas(64) EpochSlot {
  std::atomic<uint64_t> epoch{kEpochInactive};             ///< MVCC snapshot ts
  std::atomic<uint64_t> structural_epoch{kEpochInactive};  ///< structural clock
  /// In-flight MVCC commit announcement (the "commit watermark"):
  /// kEpochInactive = no commit in flight, kCommitClaiming = timestamp being
  /// drawn, otherwise the drawn timestamp of a write that has NOT finished
  /// publishing. BeginRead caps its snapshot below the minimum in-flight ts,
  /// so a snapshot can never include a timestamp whose write is still
  /// mid-publication (see MVCCTree::BeginRead).
  std::atomic<uint64_t> commit_ts{kEpochInactive};
  std::atomic<bool> claimed{false};  ///< ownership flag, distinct from epoch markers
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
      g_epoch_slots[tl_slot_index].epoch.store(kEpochInactive, std::memory_order_release);
      g_epoch_slots[tl_slot_index].structural_epoch.store(kEpochInactive,
                                                          std::memory_order_release);
      g_epoch_slots[tl_slot_index].commit_ts.store(kEpochInactive, std::memory_order_release);
      g_epoch_slots[tl_slot_index].claimed.store(false, std::memory_order_release);
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
// Reader lifecycle — wrap every traversal
// ---------------------------------------------------------------------------

/// @brief Announce that this thread is reading. Announces the current
/// g_structural_epoch (always) and snapshot_ts (kEpochInactive for readers
/// that never touch version chains, e.g. raw BPlusTree::Search).
/// Both announcements are seq_cst stores: they must be globally visible
/// before any traversal load, or a concurrent reclaimer could observe no
/// active readers, free a node, and leave us dereferencing freed memory.
void ThreadEnterEpoch(uint64_t snapshot_ts);

/// @brief Signal that this thread has finished its read traversal.
void ThreadExitEpoch();

// ---------------------------------------------------------------------------
// Writer interface
// ---------------------------------------------------------------------------

/// @brief Minimum announced structural epoch across active slots.
/// @return The safe structural epoch, or kEpochInactive if no readers are
///         active (all retired tree nodes are immediately reclaimable).
uint64_t ComputeSafeStructuralEpoch();

/// @brief Minimum announced MVCC snapshot across active slots.
/// @return The safe snapshot, or kEpochInactive if no snapshot readers are
///         active (all retired version nodes are immediately reclaimable).
uint64_t ComputeSafeSnapshot();

// Forward declarations for typed helpers (avoids delete-void* UB).
struct BPlusNode;
struct VersionNode;

/// @brief Enqueue ptr for deferred deletion (plain operator-delete), on the
/// snapshot clock. Prefer the typed helpers below.
void DeferredFree(void* ptr);

/// @brief Enqueue a BPlusNode for deferred deletion, stamped with the current
/// g_structural_epoch. Freed once every active reader announced a newer one.
void DeferFreeNode(BPlusNode* node);

/// @brief Enqueue a VersionNode for deferred deletion, stamped with the
/// current global_version (snapshot clock).
void DeferFreeVersionNode(VersionNode* node);

/// @brief Called by MVCC writers after each mutation to trigger batched
/// reclamation. Reclamation runs every kReclaimBatch calls to amortize the
/// retire-queue scan. Safe to call from any writer context.
void AdvanceEpoch();

/// @brief Free retired entries no active reader can still reach:
/// tree nodes with retired_epoch < ComputeSafeStructuralEpoch(), version
/// nodes with retired_epoch < ComputeSafeSnapshot() (everything when the
/// respective clock reports kEpochInactive — no active readers).
/// Acquires g_retire_mutex internally.
void Reclaim();

/// @brief Number of entries currently waiting in the retire queues (both
/// clocks). Test-only observability hook: lets tests assert that reclamation
/// actually drains after readers quiesce.
size_t RetireQueueSize();
