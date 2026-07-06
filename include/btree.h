#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

/// Fanout: each node holds up to K keys and K+1 child pointers.
/// K=8 is intentionally small for a demo — production trees use K in the
/// hundreds so each node fills roughly one memory page. The concurrency
/// design here is independent of K and scales to any fanout.
static constexpr size_t K = 8;

/// A B+Tree node that is IMMUTABLE after publication.
///
/// Structural writes are copy-on-write: a writer builds fresh nodes for the
/// entire root-to-leaf path it modifies, then publishes the new version of
/// the tree with a single release store of root_. A node's fields are only
/// ever written before that publication, so readers — who acquire-load
/// root_ exactly once per traversal — observe every reachable node fully
/// constructed. No per-field atomics are needed; the root_ acquire/release
/// pair is the ONLY synchronization the read path relies on.
///
/// alignas(64) keeps distinct nodes off shared cache lines (inter-node
/// isolation for readers pulling different paths).
struct alignas(64) BPlusNode {
  /// Sorted keys; only the first num_keys entries are valid.
  uint64_t keys[K] = {};

  /// Child pointers (internal nodes) or value pointers (leaf nodes).
  void* children[K + 1] = {};

  uint32_t num_keys = 0;
  bool is_leaf = false;

  explicit BPlusNode(bool leaf) : is_leaf(leaf) {}
};

static_assert(sizeof(BPlusNode) <= 512, "BPlusNode exceeds maximum allowed size of 512 bytes");

/// B+Tree with lock-free reads and copy-on-write structural writes.
///
/// Writers (Insert, InsertOrGet, Delete) serialize behind write_mutex_ and
/// never mutate a published node: they copy the affected path, publish the
/// new root with one release store, and retire the replaced nodes through
/// epoch-based reclamation. Readers therefore always traverse a consistent,
/// immutable snapshot of the tree — reads are linearizable by construction
/// (the linearization point is the root_ load).
class BPlusTree {
 public:
  BPlusTree();
  ~BPlusTree();

  BPlusTree(const BPlusTree&) = delete;
  BPlusTree& operator=(const BPlusTree&) = delete;

  /// @brief Insert key→value into the tree. If the key exists, overwrites the stored pointer.
  /// @note Serialized behind write_mutex_; safe to call from multiple threads.
  void Insert(uint64_t key, void* value);

  /// @brief Insert key→value only if key is absent.
  /// @return nullptr if the key was absent and value was inserted.
  ///         The existing stored pointer if key was already present (no modification made).
  /// @note Serialized behind write_mutex_; safe to call from multiple threads.
  void* InsertOrGet(uint64_t key, void* value);

  /// @brief Lock-free point lookup.
  /// @return The stored value pointer, or nullptr if the key is not present.
  /// @note Safe to call concurrently with Insert/Delete and other Search calls.
  void* Search(uint64_t key) const;

  /// @brief Remove key from the tree.
  /// @return true if the key was found and removed; false if not present.
  /// @note Serialized behind write_mutex_; safe to call from multiple threads.
  bool Delete(uint64_t key);

  /// @brief Number of live keys (relaxed — approximate under concurrent modification).
  size_t Size() const { return size_.load(std::memory_order_relaxed); }

  /// @brief Call fn(key, value) for every key in [lo, hi], ascending order.
  /// Loads root_ once and walks that immutable snapshot, advancing between
  /// leaves by re-descending on the next separator (O(log n) per leaf; the
  /// tree keeps no leaf chain, so a snapshot never has cross-version links).
  /// The whole scan therefore observes one structural version of the tree.
  /// Caller must hold an active EBR epoch for the duration of the scan.
  void Scan(uint64_t lo, uint64_t hi, const std::function<void(uint64_t, void*)>& fn) const;

 private:
  std::atomic<BPlusNode*> root_;
  std::atomic<size_t> size_{0};

  mutable std::mutex write_mutex_;

  // Node lifecycle
  static BPlusNode* AllocateNode(bool is_leaf);
  static void FreeNode(BPlusNode* node);
  void DestroySubtree(BPlusNode* node);

  /// @brief Lock-free lookup within the immutable snapshot rooted at node.
  void* SearchNode(const BPlusNode* node, uint64_t key) const;

  /// Like Search() but does NOT call ThreadEnterEpoch/ThreadExitEpoch.
  /// The caller must hold an active EBR epoch for the duration of the call
  /// and for as long as the returned pointer is dereferenced.
  void* SearchRaw(uint64_t key) const;

  /// @brief Copy-on-write insert (overwrites an existing key's value).
  /// Builds the fresh path, publishes root_, retires replaced nodes, and
  /// advances the structural epoch. Assumes write_mutex_ is held.
  void InsertLocked(uint64_t key, void* value);

  friend class MVCCTree;
};
