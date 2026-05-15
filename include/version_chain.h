#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

/// A single immutable version of a key's value.
/// Versions are linked newest-first: head of VersionChain is the most recent.
struct VersionNode {
  void* value;
  uint64_t created_at;                      ///< MVCC timestamp when this version was written.
  std::atomic<uint64_t> deleted_at;         ///< 0 = live; non-zero = tombstoned at this timestamp.
  std::atomic<VersionNode*> next{nullptr};  ///< Next older version in chain.

  VersionNode(void* v, uint64_t created, uint64_t deleted)
      : value(v), created_at(created), deleted_at(deleted) {}
};

/// Newest-first linked list of VersionNode objects for a single key.
/// The chain owns all its nodes and frees them on destruction.
///
/// Thread safety:
///   - Read() and HasLiveVersion() are lock-free and safe to call from any thread.
///   - Append and MarkDeleted acquire version_lock_ internally and are safe to
///     call concurrently.
class VersionChain {
 public:
  VersionChain() = default;
  ~VersionChain();

  VersionChain(const VersionChain&) = delete;
  VersionChain& operator=(const VersionChain&) = delete;

  /// @brief Prepend a new live version (created_at=ts, deleted_at=0) at head.
  void Append(void* value, uint64_t ts);

  /// @brief Mark the current head version as deleted at timestamp ts.
  /// @note Caller must ensure the head exists and is not already deleted.
  void MarkDeleted(uint64_t ts);

  /// @brief Lock-free point read at snapshot_ts.
  /// @return Value of the first version visible at snapshot_ts, or nullptr if none.
  ///         A version is visible when: created_at <= snapshot_ts && (deleted_at == 0 ||
  ///         deleted_at > snapshot_ts).
  void* Read(uint64_t snapshot_ts) const;

  /// @brief True if the head version has deleted_at == 0 (key is currently live).
  bool HasLiveVersion() const;

  /// @brief Prune VersionNodes with deleted_at <= safe_epoch via DeferFreeVersionNode.
  void Prune(uint64_t safe_epoch);

  /// @brief Expose head for tests that need to inspect the chain directly.
  const VersionNode* Head() const { return head_.load(std::memory_order_relaxed); }

 private:
  std::mutex version_lock_;  ///< Serializes concurrent writers on this key.
  std::atomic<VersionNode*> head_{nullptr};
};
