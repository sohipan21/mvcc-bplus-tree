#include "btree.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>

#include "epoch_reclamation.h"
#include "mvcc.h"

// ---------------------------------------------------------------------------
// Memory ordering invariant
// ---------------------------------------------------------------------------
//
// Writer path (inside write_mutex_):
//   Intermediate key/children stores use memory_order_relaxed — write_mutex_
//   ensures no concurrent structural writer. Readers cannot observe partial
//   state because num_keys.store(release) is the LAST write after all key
//   and children stores; it acts as the publication fence.
//
// Reader path (SearchNode, lock-free):
//   num_keys.load(acquire) is the FIRST load on a node. This acquire pairs
//   with the writer's release on num_keys, establishing happens-before for
//   all preceding key/children stores. Subsequent keys[i].load(relaxed)
//   reads are safe because the acquire already guarantees visibility.
//
// Stores that MUST remain release:
//   - num_keys.fetch_add(1, release) / num_keys.store(n, release) — commit
//   - child->next.store(sibling, release)                        — leaf chain
//   - parent->children[i].store(ptr, release)                    — tree link
//   - head_.store(new_node, release)          (in VersionChain)  — version pub
//   - deleted_at.store(ts, release)           (in VersionChain)  — tombstone
//
// Everything else in the writer path is relaxed (inside write_mutex_).
// ---------------------------------------------------------------------------

static constexpr auto relaxed = std::memory_order_relaxed;
static constexpr auto acquire = std::memory_order_acquire;
static constexpr auto release = std::memory_order_release;

// ---------------------------------------------------------------------------
// Node lifecycle
// ---------------------------------------------------------------------------

BPlusNode* BPlusTree::AllocateNode(bool is_leaf) { return new BPlusNode(is_leaf); }

void BPlusTree::FreeNode(BPlusNode* node) { delete node; }

void BPlusTree::DestroySubtree(BPlusNode* node) {
  if (node == nullptr) return;
  if (!node->is_leaf) {
    for (uint32_t i = 0; i <= node->num_keys.load(relaxed); ++i) {
      DestroySubtree(static_cast<BPlusNode*>(node->children[i].load(relaxed)));
    }
  }
  FreeNode(node);
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

BPlusTree::BPlusTree() : root_(AllocateNode(true)) {}

BPlusTree::~BPlusTree() { DestroySubtree(root_.load(relaxed)); }

// ---------------------------------------------------------------------------
// Search (lock-free read path)
// ---------------------------------------------------------------------------

void* BPlusTree::SearchNode(const BPlusNode* node, uint64_t key) const {
  // --- Internal node descent ---
  while (!node->is_leaf) {
    uint32_t nkeys = node->num_keys.load(acquire);
    uint32_t i = 0;
    while (i < nkeys && key >= node->keys[i].load(relaxed)) ++i;
    node = static_cast<const BPlusNode*>(node->children[i].load(acquire));
  }

  // --- Leaf search with sibling fallback ---
  // If a concurrent split trimmed this leaf's num_keys and moved keys to the
  // right sibling, the reader follows the leaf chain to find them.
  while (true) {
    uint32_t nkeys = node->num_keys.load(acquire);
    uint32_t lo = 0, hi = nkeys;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      uint64_t k = node->keys[mid].load(relaxed);
      if (k == key) return node->children[mid].load(acquire);
      if (k < key)
        lo = mid + 1;
      else
        hi = mid;
    }

    // Key not found in this leaf — check right sibling for a concurrent split.
    const BPlusNode* sib = node->next.load(acquire);
    if (sib != nullptr) {
      uint32_t sib_nkeys = sib->num_keys.load(acquire);
      if (sib_nkeys > 0 && key >= sib->keys[0].load(relaxed)) {
        node = sib;
        continue;
      }
    }
    return nullptr;
  }
}

void* BPlusTree::Search(uint64_t key) const {
  uint64_t snap = global_version.load(acquire);
  ThreadEnterEpoch(snap);

  BPlusNode* r = root_.load(acquire);
  void* result = SearchNode(r, key);

  ThreadExitEpoch();
  return result;
}

void* BPlusTree::SearchRaw(uint64_t key) const {
  BPlusNode* r = root_.load(acquire);
  return SearchNode(r, key);
}

// ---------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------

void BPlusTree::SplitChild(BPlusNode* parent, int child_index, BPlusNode* child) {
  BPlusNode* sibling = AllocateNode(child->is_leaf);

  if (child->is_leaf) {
    uint32_t split = K - K / 2;
    uint32_t sib_count = K / 2;

    // 1. Fill sibling (relaxed — not yet visible to readers).
    for (uint32_t i = 0; i < sib_count; ++i) {
      sibling->keys[i].store(child->keys[split + i].load(relaxed), relaxed);
      sibling->children[i].store(child->children[split + i].load(relaxed), relaxed);
    }
    sibling->num_keys.store(sib_count, relaxed);

    uint64_t promoted = sibling->keys[0].load(relaxed);

    // 2. Publish sibling into leaf chain (release — readers may follow next).
    sibling->next.store(child->next.load(relaxed), relaxed);
    child->next.store(sibling, release);

    // 3. Shift parent's children/keys and publish sibling ptr.
    uint32_t pnkeys = parent->num_keys.load(relaxed);
    for (int i = static_cast<int>(pnkeys); i > child_index; --i) {
      parent->keys[i].store(parent->keys[i - 1].load(relaxed), relaxed);
      parent->children[i + 1].store(parent->children[i].load(relaxed), release);
    }
    parent->keys[child_index].store(promoted, relaxed);
    parent->children[child_index + 1].store(sibling, release);
    parent->num_keys.fetch_add(1, release);

    // 4. Trim child LAST — after sibling is reachable from parent and leaf chain.
    child->num_keys.store(split, release);

  } else {
    uint32_t mid = K / 2;
    uint64_t promoted = child->keys[mid].load(relaxed);
    uint32_t sib_count = K - mid - 1;

    // 1. Fill sibling (relaxed — not yet visible).
    for (uint32_t i = 0; i < sib_count; ++i) {
      sibling->keys[i].store(child->keys[mid + 1 + i].load(relaxed), relaxed);
      sibling->children[i].store(child->children[mid + 1 + i].load(relaxed), relaxed);
    }
    sibling->children[sib_count].store(child->children[K].load(relaxed), relaxed);
    sibling->num_keys.store(sib_count, relaxed);

    // 2. Shift parent and publish sibling ptr.
    uint32_t pnkeys = parent->num_keys.load(relaxed);
    for (int i = static_cast<int>(pnkeys); i > child_index; --i) {
      parent->keys[i].store(parent->keys[i - 1].load(relaxed), relaxed);
      parent->children[i + 1].store(parent->children[i].load(relaxed), release);
    }
    parent->keys[child_index].store(promoted, relaxed);
    parent->children[child_index + 1].store(sibling, release);
    parent->num_keys.fetch_add(1, release);

    // 3. Trim child LAST.
    child->num_keys.store(mid, release);
  }
}

void BPlusTree::InsertNonFull(BPlusNode* node, uint64_t key, void* value) {
  if (node->is_leaf) {
    uint32_t nkeys = node->num_keys.load(relaxed);
    int i = static_cast<int>(nkeys) - 1;

    // Overwrite if key already exists.
    for (uint32_t j = 0; j < nkeys; ++j) {
      if (node->keys[j].load(relaxed) == key) {
        node->children[j].store(value, release);
        return;
      }
    }

    // Shift entries right to make room.
    while (i >= 0 && node->keys[i].load(relaxed) > key) {
      node->keys[i + 1].store(node->keys[i].load(relaxed), relaxed);
      node->children[i + 1].store(node->children[i].load(relaxed), relaxed);
      --i;
    }
    node->keys[i + 1].store(key, relaxed);
    node->children[i + 1].store(value, relaxed);
    // release: readers acquire-loading num_keys see all prior key/children stores.
    node->num_keys.fetch_add(1, release);
    size_.fetch_add(1, relaxed);

  } else {
    uint32_t nkeys = node->num_keys.load(relaxed);
    int i = static_cast<int>(nkeys) - 1;
    while (i >= 0 && node->keys[i].load(relaxed) > key) --i;
    ++i;

    auto* child = static_cast<BPlusNode*>(node->children[i].load(acquire));
    if (child->IsFull()) {
      SplitChild(node, i, child);
      if (node->keys[i].load(relaxed) <= key) ++i;
    }
    InsertNonFull(static_cast<BPlusNode*>(node->children[i].load(acquire)), key, value);
  }
}

void BPlusTree::Insert(uint64_t key, void* value) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  current_write_epoch_ = global_version.load(acquire);

  BPlusNode* r = root_.load(relaxed);
  if (r->IsFull()) {
    BPlusNode* new_root = AllocateNode(false);
    new_root->children[0].store(r, relaxed);
    SplitChild(new_root, 0, r);
    root_.store(new_root, release);
    r = new_root;
  }
  InsertNonFull(r, key, value);
  Reclaim();
}

void* BPlusTree::InsertOrGet(uint64_t key, void* value) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  current_write_epoch_ = global_version.load(acquire);

  // SearchNode is safe to call under write_mutex_: no concurrent structural
  // writer exists, and concurrent lock-free readers do not conflict.
  BPlusNode* r = root_.load(relaxed);
  void* existing = SearchNode(r, key);
  if (existing != nullptr) return existing;

  if (r->IsFull()) {
    BPlusNode* new_root = AllocateNode(false);
    new_root->children[0].store(r, relaxed);
    SplitChild(new_root, 0, r);
    root_.store(new_root, release);
    r = new_root;
  }
  InsertNonFull(r, key, value);
  Reclaim();
  return nullptr;
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

void BPlusTree::BorrowFromLeft(BPlusNode* parent, int child_index) {
  auto* child = static_cast<BPlusNode*>(parent->children[child_index].load(acquire));
  auto* left = static_cast<BPlusNode*>(parent->children[child_index - 1].load(acquire));

  uint32_t cnkeys = child->num_keys.load(relaxed);
  uint32_t lnkeys = left->num_keys.load(relaxed);

  // Shift child's entries right by one.
  for (int i = static_cast<int>(cnkeys); i > 0; --i) {
    child->keys[i].store(child->keys[i - 1].load(relaxed), relaxed);
    int src = i - 1 + (child->is_leaf ? 0 : 1);
    int dst = i + (child->is_leaf ? 0 : 1);
    child->children[dst].store(child->children[src].load(relaxed), relaxed);
  }
  if (!child->is_leaf) child->children[1].store(child->children[0].load(relaxed), relaxed);

  if (child->is_leaf) {
    child->keys[0].store(left->keys[lnkeys - 1].load(relaxed), relaxed);
    child->children[0].store(left->children[lnkeys - 1].load(relaxed), relaxed);
    left->num_keys.fetch_sub(1, release);
    parent->keys[child_index - 1].store(child->keys[0].load(relaxed), relaxed);
  } else {
    child->keys[0].store(parent->keys[child_index - 1].load(relaxed), relaxed);
    child->children[0].store(left->children[lnkeys].load(relaxed), relaxed);
    parent->keys[child_index - 1].store(left->keys[lnkeys - 1].load(relaxed), relaxed);
    left->num_keys.fetch_sub(1, release);
  }
  child->num_keys.fetch_add(1, release);
}

void BPlusTree::BorrowFromRight(BPlusNode* parent, int child_index) {
  auto* child = static_cast<BPlusNode*>(parent->children[child_index].load(acquire));
  auto* right = static_cast<BPlusNode*>(parent->children[child_index + 1].load(acquire));

  uint32_t cnkeys = child->num_keys.load(relaxed);
  uint32_t rnkeys = right->num_keys.load(relaxed);

  if (child->is_leaf) {
    child->keys[cnkeys].store(right->keys[0].load(relaxed), relaxed);
    child->children[cnkeys].store(right->children[0].load(relaxed), relaxed);
    child->num_keys.fetch_add(1, release);
    for (uint32_t i = 0; i < rnkeys - 1; ++i) {
      right->keys[i].store(right->keys[i + 1].load(relaxed), relaxed);
      right->children[i].store(right->children[i + 1].load(relaxed), relaxed);
    }
    right->num_keys.fetch_sub(1, release);
    parent->keys[child_index].store(right->keys[0].load(relaxed), relaxed);
  } else {
    child->keys[cnkeys].store(parent->keys[child_index].load(relaxed), relaxed);
    child->children[cnkeys + 1].store(right->children[0].load(relaxed), relaxed);
    child->num_keys.fetch_add(1, release);
    parent->keys[child_index].store(right->keys[0].load(relaxed), relaxed);
    for (uint32_t i = 0; i < rnkeys - 1; ++i) {
      right->keys[i].store(right->keys[i + 1].load(relaxed), relaxed);
      right->children[i].store(right->children[i + 1].load(relaxed), relaxed);
    }
    right->children[rnkeys - 1].store(right->children[rnkeys].load(relaxed), relaxed);
    right->num_keys.fetch_sub(1, release);
  }
}

void BPlusTree::MergeChildren(BPlusNode* parent, int left_index) {
  auto* left = static_cast<BPlusNode*>(parent->children[left_index].load(acquire));
  auto* right = static_cast<BPlusNode*>(parent->children[left_index + 1].load(acquire));

  uint32_t lnkeys = left->num_keys.load(relaxed);
  uint32_t rnkeys = right->num_keys.load(relaxed);

  if (left->is_leaf) {
    for (uint32_t i = 0; i < rnkeys; ++i) {
      left->keys[lnkeys + i].store(right->keys[i].load(relaxed), relaxed);
      left->children[lnkeys + i].store(right->children[i].load(relaxed), relaxed);
    }
    left->num_keys.store(lnkeys + rnkeys, release);
    left->next.store(right->next.load(relaxed), release);
  } else {
    left->keys[lnkeys].store(parent->keys[left_index].load(relaxed), relaxed);
    for (uint32_t i = 0; i < rnkeys; ++i) {
      left->keys[lnkeys + 1 + i].store(right->keys[i].load(relaxed), relaxed);
      left->children[lnkeys + 1 + i].store(right->children[i].load(relaxed), relaxed);
    }
    left->children[lnkeys + 1 + rnkeys].store(right->children[rnkeys].load(relaxed), relaxed);
    left->num_keys.store(lnkeys + rnkeys + 1, release);
  }

  uint32_t pnkeys = parent->num_keys.load(relaxed);
  for (uint32_t i = static_cast<uint32_t>(left_index); i < pnkeys - 1; ++i) {
    parent->keys[i].store(parent->keys[i + 1].load(relaxed), relaxed);
    parent->children[i + 1].store(parent->children[i + 2].load(relaxed), relaxed);
  }
  parent->num_keys.fetch_sub(1, release);

  DeferFreeNode(right);
}

void BPlusTree::FixUnderflow(BPlusNode* parent, int child_index) {
  auto* child = static_cast<BPlusNode*>(parent->children[child_index].load(acquire));
  if (child->num_keys.load(relaxed) >= kMinKeys) return;

  uint32_t pnkeys = parent->num_keys.load(relaxed);
  bool has_left = child_index > 0;
  bool has_right = child_index < static_cast<int>(pnkeys);

  if (has_left) {
    auto* left = static_cast<BPlusNode*>(parent->children[child_index - 1].load(acquire));
    if (left->num_keys.load(relaxed) > kMinKeys) {
      BorrowFromLeft(parent, child_index);
      return;
    }
  }
  if (has_right) {
    auto* right = static_cast<BPlusNode*>(parent->children[child_index + 1].load(acquire));
    if (right->num_keys.load(relaxed) > kMinKeys) {
      BorrowFromRight(parent, child_index);
      return;
    }
  }

  if (has_left)
    MergeChildren(parent, child_index - 1);
  else
    MergeChildren(parent, child_index);
}

bool BPlusTree::DeleteFromNode(BPlusNode* node, BPlusNode* parent, int parent_index, uint64_t key) {
  if (node->is_leaf) {
    uint32_t nkeys = node->num_keys.load(relaxed);
    for (uint32_t i = 0; i < nkeys; ++i) {
      if (node->keys[i].load(relaxed) == key) {
        for (uint32_t j = i; j < nkeys - 1; ++j) {
          node->keys[j].store(node->keys[j + 1].load(relaxed), relaxed);
          node->children[j].store(node->children[j + 1].load(relaxed), relaxed);
        }
        node->num_keys.fetch_sub(1, release);
        size_.fetch_sub(1, relaxed);
        if (parent != nullptr) FixUnderflow(parent, parent_index);
        return true;
      }
      if (node->keys[i].load(relaxed) > key) break;
    }
    return false;
  }

  uint32_t nkeys = node->num_keys.load(relaxed);
  uint32_t i = 0;
  while (i < nkeys && key >= node->keys[i].load(relaxed)) ++i;

  auto* child = static_cast<BPlusNode*>(node->children[i].load(acquire));
  bool found = DeleteFromNode(child, node, static_cast<int>(i), key);

  if (!found) return false;

  if (parent != nullptr) FixUnderflow(parent, parent_index);
  return true;
}

bool BPlusTree::Delete(uint64_t key) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  current_write_epoch_ = global_version.load(acquire);

  if (size_.load(relaxed) == 0) return false;

  bool found = DeleteFromNode(root_.load(relaxed), nullptr, 0, key);

  BPlusNode* r = root_.load(relaxed);
  if (!r->is_leaf && r->num_keys.load(relaxed) == 0) {
    BPlusNode* new_root = static_cast<BPlusNode*>(r->children[0].load(relaxed));
    root_.store(new_root, release);
    r->children[0].store(nullptr, relaxed);
    DeferFreeNode(r);
  }

  Reclaim();
  return found;
}
