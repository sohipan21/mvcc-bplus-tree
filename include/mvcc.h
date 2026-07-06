#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "btree.h"
#include "version_chain.h"

/// Global monotonic version counter.
/// Writers increment with memory_order_release before publishing versions.
/// Readers load with memory_order_acquire to capture a consistent snapshot.
extern std::atomic<uint64_t> global_version;

/// MVCC wrapper around BPlusTree.
///
/// The B+Tree stores VersionChain* as its leaf values; all MVCC logic lives here.
///
/// Thread safety:
///   - Read() and Exists() are lock-free (no mutexes held).
///   - Insert/Update/Delete serialize per-key via VersionChain::version_lock_ and
///     structurally via BPlusTree::write_mutex_.
class MVCCTree {
 public:
  MVCCTree() = default;
  ~MVCCTree();

  MVCCTree(const MVCCTree&) = delete;
  MVCCTree& operator=(const MVCCTree&) = delete;

  /// @brief Capture a read snapshot timestamp.
  /// All subsequent Read(key, snapshot) calls using this value will see a
  /// consistent point-in-time view regardless of concurrent writes.
  /// The returned snapshot is capped below the oldest in-flight commit (the
  /// commit watermark), so every timestamp <= snapshot belongs to a fully
  /// published write — snapshot reads are repeatable even while writers are
  /// mid-commit.
  uint64_t BeginRead() const;

  /// @brief Insert key→value. If the key already exists, behaves like Update
  ///        (appends a new version to the existing chain).
  /// @note Concurrent same-key inserts are safe: InsertOrGet prevents duplicate chains.
  void Insert(uint64_t key, void* value);

  /// @brief Overwrite an existing key with a new version.
  /// Atomically marks the current live version deleted and prepends a new one,
  /// both with the same timestamp so there is no snapshot visibility gap.
  /// No-op if the key does not exist or has no live version.
  void Update(uint64_t key, void* value);

  /// @brief Tombstone the current live version of key.
  /// @return false if the key is not found or has no live version.
  bool Delete(uint64_t key);

  /// @brief Return the value visible at snapshot_ts, or nullptr if none.
  void* Read(uint64_t key, uint64_t snapshot_ts) const;

  /// @brief True if key has a version visible at snapshot_ts.
  bool Exists(uint64_t key, uint64_t snapshot_ts) const;

  /// @brief Call fn(key, value) for every key in [lo, hi] visible at snapshot_ts, ascending.
  /// Holds the EBR epoch for the whole walk so version nodes can't be freed mid-scan.
  void Scan(uint64_t lo, uint64_t hi, uint64_t snapshot_ts,
            const std::function<void(uint64_t, void*)>& fn) const;

  /// @brief Number of distinct keys ever inserted (includes fully-deleted keys).
  size_t NumKeys() const;

 private:
  BPlusTree tree_;

  std::mutex chains_mutex_;            ///< Protects chains_ for concurrent Insert callers.
  std::vector<VersionChain*> chains_;  ///< All VersionChains; used by destructor and Prune.

  /// @brief Walk all VersionChains and free nodes whose deleted_at timestamp
  /// is older than the oldest active reader epoch. Call from a background thread.
  void PruneVersionNodes();
};
