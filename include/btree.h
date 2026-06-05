#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

/// Fanout: each node holds up to K keys and K+1 child pointers.
/// K=8 is intentionally small for a demo — production trees use K in the
/// hundreds so each node fills roughly one memory page. The concurrency
/// design here is independent of K and scales to any fanout.
static constexpr size_t K = 8;

/// alignas(64) puts each node on a cache-line boundary so adjacent nodes never
/// share a line, preventing false sharing when two threads access different nodes.
/// The node itself spans several cache lines; the alignment is about inter-node
/// isolation, not fitting the whole node into one line.
/// All fields a lock-free reader touches are atomic, so concurrent reads and
/// writes are TSan-clean without the reader ever holding a mutex.
struct alignas(64) BPlusNode {
  /// Leaf chain pointer — links leaf nodes in sorted key order.
  std::atomic<BPlusNode*> next{nullptr};

  /// Node-level epoch tracking used by the EBR layer.
  uint64_t created_at{0};
  uint64_t deleted_at{0};

  /// Sorted keys — atomic so readers can load while a writer shifts entries.
  /// No {} initializer (std::atomic is not copy-constructible); see constructor.
  std::array<std::atomic<uint64_t>, K> keys;

  /// Child pointers (internal nodes) or value pointers (leaf nodes).
  std::array<std::atomic<void*>, K + 1> children;

  bool is_leaf{false};

  /// Atomic commit signal: the LAST release-store after all key/children writes.
  /// Readers acquire-load this first to see all prior stores.
  std::atomic<uint32_t> num_keys{0};

  explicit BPlusNode(bool leaf) : is_leaf(leaf) {
    for (auto& k : keys) k.store(0, std::memory_order_relaxed);
    for (auto& c : children) c.store(nullptr, std::memory_order_relaxed);
  }

  bool IsFull() const { return num_keys.load(std::memory_order_relaxed) == K; }
};

static_assert(sizeof(BPlusNode) <= 512, "BPlusNode exceeds maximum allowed size of 512 bytes");

/// B+Tree with lock-free reads and serialized structural writes.
///
/// Structural modifications (Insert, Delete) are serialized behind write_mutex_.
/// Search() is fully lock-free and safe to call concurrently with any writer.
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

 private:
  std::atomic<BPlusNode*> root_;
  std::atomic<size_t> size_{0};

  mutable std::mutex write_mutex_;
  uint64_t current_write_epoch_{0};

  // Node lifecycle
  static BPlusNode* AllocateNode(bool is_leaf);
  static void FreeNode(BPlusNode* node);
  void DestroySubtree(BPlusNode* node);

  // Search helpers
  void* SearchNode(const BPlusNode* node, uint64_t key) const;

  /// @brief Descend to the leaf node that would contain key.
  /// Lock-free; caller must hold an active EBR epoch.
  BPlusNode* FindLeaf(uint64_t key) const;

  /// Like Search() but does NOT call ThreadEnterEpoch/ThreadExitEpoch.
  /// The caller must hold an active EBR epoch for the duration of the call
  /// and for as long as the returned pointer is dereferenced.
  void* SearchRaw(uint64_t key) const;

  friend class MVCCTree;

  // Insert helpers
  void InsertNonFull(BPlusNode* node, uint64_t key, void* value);
  void SplitChild(BPlusNode* parent, int child_index, BPlusNode* child);

  // Delete helpers — key passed as parameter to avoid the shared delete_key_ TOCTOU race
  bool DeleteFromNode(BPlusNode* node, BPlusNode* parent, int parent_index, uint64_t key);

  // Underflow repair
  void FixUnderflow(BPlusNode* parent, int child_index);
  void BorrowFromLeft(BPlusNode* parent, int child_index);
  void BorrowFromRight(BPlusNode* parent, int child_index);
  void MergeChildren(BPlusNode* parent, int left_index);

  static constexpr size_t kMinKeys = (K - 1) / 2;
};
