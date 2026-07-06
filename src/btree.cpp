#include "btree.h"

#include <algorithm>
#include <cassert>
#include <vector>

#include "epoch_reclamation.h"

// ---------------------------------------------------------------------------
// Copy-on-write invariant
// ---------------------------------------------------------------------------
//
// Published nodes are immutable. A structural writer (inside write_mutex_):
//   1. builds fresh copies of every node on the root->leaf path it changes
//      (plus any sibling consumed by a merge/redistribution),
//   2. publishes the whole new version with ONE root_.store(release),
//   3. retires the replaced nodes via DeferFreeNode (EBR, structural clock),
//   4. advances g_structural_epoch (release).
//
// A reader loads root_ once with acquire. That single acquire pairs with the
// writer's release store and establishes happens-before for every field of
// every node reachable from that root — the reader traverses a frozen
// snapshot and can never observe a half-shifted key array or a torn
// separator. Reads are linearizable at the root_ load.
//
// Retired nodes stay reachable from OLD snapshots only; EBR frees them once
// every active reader has announced a structural epoch newer than the
// retirement (see epoch_reclamation.h).
// ---------------------------------------------------------------------------

static constexpr auto relaxed = std::memory_order_relaxed;
static constexpr auto acquire = std::memory_order_acquire;
static constexpr auto release = std::memory_order_release;

namespace {

constexpr uint32_t kMinKeys = (K - 1) / 2;

/// Index of the child to descend into for key: first i with key < keys[i].
/// (Descent goes right on key >= keys[i], so child i covers
/// [keys[i-1], keys[i]) with keys[-1] = -inf and keys[n] = +inf.)
uint32_t ChildIndex(const BPlusNode* node, uint64_t key) {
  uint32_t i = 0;
  while (i < node->num_keys && key >= node->keys[i]) ++i;
  return i;
}

/// Fresh copies produced by an insert descent. right != nullptr means the
/// subtree split and sep must be added to the parent.
struct SplitResult {
  BPlusNode* left;
  BPlusNode* right;
  uint64_t sep;
};

}  // namespace

// ---------------------------------------------------------------------------
// Node lifecycle
// ---------------------------------------------------------------------------

BPlusNode* BPlusTree::AllocateNode(bool is_leaf) { return new BPlusNode(is_leaf); }

void BPlusTree::FreeNode(BPlusNode* node) { delete node; }

void BPlusTree::DestroySubtree(BPlusNode* node) {
  if (node == nullptr) return;
  if (!node->is_leaf) {
    for (uint32_t i = 0; i <= node->num_keys; ++i) {
      DestroySubtree(static_cast<BPlusNode*>(node->children[i]));
    }
  }
  FreeNode(node);
}

namespace {

/// Clone a published node's contents into a fresh (unpublished) node.
BPlusNode* CopyNode(const BPlusNode* node) {
  auto* fresh = new BPlusNode(node->is_leaf);
  fresh->num_keys = node->num_keys;
  for (uint32_t i = 0; i < node->num_keys; ++i) fresh->keys[i] = node->keys[i];
  uint32_t nchildren = node->is_leaf ? node->num_keys : node->num_keys + 1;
  for (uint32_t i = 0; i < nchildren; ++i) fresh->children[i] = node->children[i];
  return fresh;
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

BPlusTree::BPlusTree() : root_(AllocateNode(true)) {}

BPlusTree::~BPlusTree() { DestroySubtree(root_.load(relaxed)); }

// ---------------------------------------------------------------------------
// Search (lock-free read path)
// ---------------------------------------------------------------------------

void* BPlusTree::SearchNode(const BPlusNode* node, uint64_t key) const {
  // Plain (non-atomic) loads throughout: the snapshot rooted at `node` is
  // immutable, and visibility of all its fields was established by the
  // acquire load of root_ that produced it.
  while (!node->is_leaf) {
    node = static_cast<const BPlusNode*>(node->children[ChildIndex(node, key)]);
  }

  uint32_t lo = 0, hi = node->num_keys;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uint64_t k = node->keys[mid];
    if (k == key) return node->children[mid];
    if (k < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return nullptr;
}

void* BPlusTree::Search(uint64_t key) const {
  // kEpochInactive snapshot: a raw tree read needs structural protection only
  // (it never dereferences version nodes), so it must not hold back version
  // pruning.
  ThreadEnterEpoch(kEpochInactive);

  const BPlusNode* r = root_.load(acquire);
  void* result = SearchNode(r, key);

  ThreadExitEpoch();
  return result;
}

void* BPlusTree::SearchRaw(uint64_t key) const {
  const BPlusNode* r = root_.load(acquire);
  return SearchNode(r, key);
}

void BPlusTree::Scan(uint64_t lo, uint64_t hi,
                     const std::function<void(uint64_t, void*)>& fn) const {
  // One root load = one structural snapshot for the whole scan. There is no
  // leaf chain (a chain would link across snapshot versions); instead we
  // re-descend for the next leaf using the tightest separator above the
  // current one — O(log n) per leaf boundary within the same snapshot.
  const BPlusNode* root = root_.load(acquire);
  uint64_t cursor = lo;

  while (true) {
    const BPlusNode* node = root;
    uint64_t bound = 0;
    bool has_bound = false;  // smallest separator > cursor on the path

    while (!node->is_leaf) {
      uint32_t i = ChildIndex(node, cursor);
      if (i < node->num_keys) {
        bound = node->keys[i];
        has_bound = true;
      }
      node = static_cast<const BPlusNode*>(node->children[i]);
    }

    for (uint32_t j = 0; j < node->num_keys; ++j) {
      uint64_t k = node->keys[j];
      if (k < cursor) continue;
      if (k > hi) return;
      void* v = node->children[j];
      if (v != nullptr) fn(k, v);
    }

    // All keys >= bound live in the next leaf over; keys in this leaf are
    // < bound by the separator invariant. Separators strictly increase per
    // re-descent, so the walk terminates.
    if (!has_bound || bound > hi) return;
    cursor = bound;
  }
}

// ---------------------------------------------------------------------------
// Insert (copy-on-write)
// ---------------------------------------------------------------------------

namespace {

/// Build the fresh subtree that results from inserting key->value into the
/// immutable subtree rooted at node. Every replaced original is appended to
/// `retired`. *added reports whether a new key was created (vs overwrite).
SplitResult InsertRec(const BPlusNode* node, uint64_t key, void* value, bool* added,
                      std::vector<BPlusNode*>& retired) {
  retired.push_back(const_cast<BPlusNode*>(node));

  if (node->is_leaf) {
    uint64_t tmp_k[K + 1];
    void* tmp_v[K + 1];
    uint32_t n = node->num_keys;

    uint32_t pos = 0;
    while (pos < n && node->keys[pos] < key) ++pos;
    bool exists = pos < n && node->keys[pos] == key;
    *added = !exists;

    uint32_t total = 0;
    for (uint32_t i = 0; i < pos; ++i, ++total) {
      tmp_k[total] = node->keys[i];
      tmp_v[total] = node->children[i];
    }
    tmp_k[total] = key;
    tmp_v[total] = value;
    ++total;
    for (uint32_t i = pos + (exists ? 1 : 0); i < n; ++i, ++total) {
      tmp_k[total] = node->keys[i];
      tmp_v[total] = node->children[i];
    }

    if (total <= K) {
      auto* leaf = new BPlusNode(true);
      leaf->num_keys = total;
      for (uint32_t i = 0; i < total; ++i) {
        leaf->keys[i] = tmp_k[i];
        leaf->children[i] = tmp_v[i];
      }
      return {leaf, nullptr, 0};
    }

    // Overflow (total == K+1): split into two leaves.
    uint32_t right_n = total / 2;
    uint32_t left_n = total - right_n;
    auto* left = new BPlusNode(true);
    auto* right = new BPlusNode(true);
    left->num_keys = left_n;
    right->num_keys = right_n;
    for (uint32_t i = 0; i < left_n; ++i) {
      left->keys[i] = tmp_k[i];
      left->children[i] = tmp_v[i];
    }
    for (uint32_t i = 0; i < right_n; ++i) {
      right->keys[i] = tmp_k[left_n + i];
      right->children[i] = tmp_v[left_n + i];
    }
    return {left, right, right->keys[0]};
  }

  // Internal node: recurse, then rebuild this node with the fresh child
  // (and the promoted separator if the child split).
  uint32_t ci = ChildIndex(node, key);
  SplitResult child =
      InsertRec(static_cast<const BPlusNode*>(node->children[ci]), key, value, added, retired);

  uint64_t tmp_k[K + 1];
  void* tmp_c[K + 2];
  uint32_t n = node->num_keys;
  for (uint32_t i = 0; i < n; ++i) tmp_k[i] = node->keys[i];
  for (uint32_t i = 0; i <= n; ++i) tmp_c[i] = node->children[i];
  tmp_c[ci] = child.left;

  uint32_t total = n;
  if (child.right != nullptr) {
    for (uint32_t i = n; i > ci; --i) tmp_k[i] = tmp_k[i - 1];
    for (uint32_t i = n + 1; i > ci + 1; --i) tmp_c[i] = tmp_c[i - 1];
    tmp_k[ci] = child.sep;
    tmp_c[ci + 1] = child.right;
    total = n + 1;
  }

  if (total <= K) {
    auto* fresh = new BPlusNode(false);
    fresh->num_keys = total;
    for (uint32_t i = 0; i < total; ++i) fresh->keys[i] = tmp_k[i];
    for (uint32_t i = 0; i <= total; ++i) fresh->children[i] = tmp_c[i];
    return {fresh, nullptr, 0};
  }

  // Overflow (total == K+1 keys): split, promoting the middle key.
  uint32_t mid = total / 2;
  auto* left = new BPlusNode(false);
  auto* right = new BPlusNode(false);
  left->num_keys = mid;
  for (uint32_t i = 0; i < mid; ++i) left->keys[i] = tmp_k[i];
  for (uint32_t i = 0; i <= mid; ++i) left->children[i] = tmp_c[i];
  uint32_t right_n = total - mid - 1;
  right->num_keys = right_n;
  for (uint32_t i = 0; i < right_n; ++i) right->keys[i] = tmp_k[mid + 1 + i];
  for (uint32_t i = 0; i <= right_n; ++i) right->children[i] = tmp_c[mid + 1 + i];
  return {left, right, tmp_k[mid]};
}

}  // namespace

void BPlusTree::InsertLocked(uint64_t key, void* value) {
  std::vector<BPlusNode*> retired;
  bool added = false;

  SplitResult res = InsertRec(root_.load(relaxed), key, value, &added, retired);
  BPlusNode* new_root = res.left;
  if (res.right != nullptr) {
    auto* nr = AllocateNode(false);
    nr->keys[0] = res.sep;
    nr->children[0] = res.left;
    nr->children[1] = res.right;
    nr->num_keys = 1;
    new_root = nr;
  }

  // Single publication point: the entire new tree version becomes visible
  // atomically. Retire replaced nodes only AFTER the swing, then advance the
  // structural clock so future readers announce a newer epoch.
  root_.store(new_root, release);
  if (added) size_.fetch_add(1, relaxed);
  for (BPlusNode* n : retired) DeferFreeNode(n);
  g_structural_epoch.fetch_add(1, release);
  Reclaim();
}

void BPlusTree::Insert(uint64_t key, void* value) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  InsertLocked(key, value);
}

void* BPlusTree::InsertOrGet(uint64_t key, void* value) {
  std::lock_guard<std::mutex> lock(write_mutex_);

  // SearchNode is safe here without an EBR announcement: while we hold
  // write_mutex_ nothing reachable from the current root can be retired
  // (retirement only happens inside structural writes, all serialized
  // behind this mutex), and already-retired nodes are unreachable.
  void* existing = SearchNode(root_.load(relaxed), key);
  if (existing != nullptr) return existing;

  InsertLocked(key, value);
  return nullptr;
}

// ---------------------------------------------------------------------------
// Delete (copy-on-write)
// ---------------------------------------------------------------------------

namespace {

/// True if key is present in the subtree rooted at node.
bool KeyExists(const BPlusNode* node, uint64_t key) {
  while (!node->is_leaf) {
    node = static_cast<const BPlusNode*>(node->children[ChildIndex(node, key)]);
  }
  for (uint32_t i = 0; i < node->num_keys; ++i) {
    if (node->keys[i] == key) return true;
    if (node->keys[i] > key) return false;
  }
  return false;
}

/// Repair an underfull fresh child at parent->children[child_index] by
/// merging or redistributing with an adjacent sibling. `parent` is fresh
/// (unpublished) and mutated in place; consumed ORIGINAL siblings go to
/// `retired`, consumed FRESH nodes are deleted directly (never published).
void RebalanceChild(BPlusNode* parent, uint32_t child_index, std::vector<BPlusNode*>& retired) {
  uint32_t sib_index = (child_index > 0) ? child_index - 1 : child_index + 1;
  uint32_t li = std::min(child_index, sib_index);
  uint32_t ri = std::max(child_index, sib_index);
  auto* lc = static_cast<BPlusNode*>(parent->children[li]);
  auto* rc = static_cast<BPlusNode*>(parent->children[ri]);
  bool leaf = lc->is_leaf;

  // Gather both children's entries (plus the separator, for internal nodes).
  uint64_t gk[2 * K + 1];
  void* gc[2 * K + 2];
  uint32_t nk = 0, nc = 0;

  if (leaf) {
    for (uint32_t i = 0; i < lc->num_keys; ++i) {
      gk[nk++] = lc->keys[i];
      gc[nc++] = lc->children[i];
    }
    for (uint32_t i = 0; i < rc->num_keys; ++i) {
      gk[nk++] = rc->keys[i];
      gc[nc++] = rc->children[i];
    }
  } else {
    for (uint32_t i = 0; i < lc->num_keys; ++i) gk[nk++] = lc->keys[i];
    gk[nk++] = parent->keys[li];
    for (uint32_t i = 0; i < rc->num_keys; ++i) gk[nk++] = rc->keys[i];
    for (uint32_t i = 0; i <= lc->num_keys; ++i) gc[nc++] = lc->children[i];
    for (uint32_t i = 0; i <= rc->num_keys; ++i) gc[nc++] = rc->children[i];
  }

  // The child at child_index is the fresh (unpublished) node; the sibling is
  // an original shared with live snapshots.
  BPlusNode* fresh_consumed = static_cast<BPlusNode*>(parent->children[child_index]);
  BPlusNode* original_consumed = static_cast<BPlusNode*>(parent->children[sib_index]);
  delete fresh_consumed;
  retired.push_back(original_consumed);

  if (nk <= K) {
    // Merge into a single node; parent loses the separator at li.
    auto* merged = new BPlusNode(leaf);
    merged->num_keys = nk;
    for (uint32_t i = 0; i < nk; ++i) merged->keys[i] = gk[i];
    for (uint32_t i = 0; i < nc; ++i) merged->children[i] = gc[i];

    parent->children[li] = merged;
    for (uint32_t i = li; i + 1 < parent->num_keys; ++i) parent->keys[i] = parent->keys[i + 1];
    for (uint32_t i = ri; i < parent->num_keys; ++i) parent->children[i] = parent->children[i + 1];
    parent->num_keys -= 1;
    return;
  }

  // Redistribute into two balanced nodes; parent's separator at li updates.
  auto* nl = new BPlusNode(leaf);
  auto* nr = new BPlusNode(leaf);
  uint64_t sep;

  if (leaf) {
    uint32_t rn = nk / 2;
    uint32_t ln = nk - rn;
    nl->num_keys = ln;
    nr->num_keys = rn;
    for (uint32_t i = 0; i < ln; ++i) {
      nl->keys[i] = gk[i];
      nl->children[i] = gc[i];
    }
    for (uint32_t i = 0; i < rn; ++i) {
      nr->keys[i] = gk[ln + i];
      nr->children[i] = gc[ln + i];
    }
    sep = nr->keys[0];
  } else {
    uint32_t h = nk / 2;  // promoted separator index
    nl->num_keys = h;
    for (uint32_t i = 0; i < h; ++i) nl->keys[i] = gk[i];
    for (uint32_t i = 0; i <= h; ++i) nl->children[i] = gc[i];
    uint32_t rn = nk - h - 1;
    nr->num_keys = rn;
    for (uint32_t i = 0; i < rn; ++i) nr->keys[i] = gk[h + 1 + i];
    for (uint32_t i = 0; i <= rn; ++i) nr->children[i] = gc[h + 1 + i];
    sep = gk[h];
  }

  parent->keys[li] = sep;
  parent->children[li] = nl;
  parent->children[ri] = nr;
}

/// Build the fresh subtree that results from removing key from the immutable
/// subtree rooted at node. Precondition: key exists in this subtree.
BPlusNode* DeleteRec(const BPlusNode* node, uint64_t key, std::vector<BPlusNode*>& retired) {
  retired.push_back(const_cast<BPlusNode*>(node));

  if (node->is_leaf) {
    auto* fresh = new BPlusNode(true);
    uint32_t out = 0;
    for (uint32_t i = 0; i < node->num_keys; ++i) {
      if (node->keys[i] == key) continue;
      fresh->keys[out] = node->keys[i];
      fresh->children[out] = node->children[i];
      ++out;
    }
    fresh->num_keys = out;
    return fresh;
  }

  uint32_t ci = ChildIndex(node, key);
  BPlusNode* fresh_child =
      DeleteRec(static_cast<const BPlusNode*>(node->children[ci]), key, retired);

  BPlusNode* fresh = CopyNode(node);
  fresh->children[ci] = fresh_child;

  if (fresh_child->num_keys < kMinKeys) RebalanceChild(fresh, ci, retired);
  return fresh;
}

}  // namespace

bool BPlusTree::Delete(uint64_t key) {
  std::lock_guard<std::mutex> lock(write_mutex_);

  BPlusNode* old_root = root_.load(relaxed);
  if (!KeyExists(old_root, key)) return false;

  std::vector<BPlusNode*> retired;
  BPlusNode* new_root = DeleteRec(old_root, key, retired);

  // Collapse an empty internal root: publish its single child as the root.
  if (!new_root->is_leaf && new_root->num_keys == 0) {
    auto* collapsed = static_cast<BPlusNode*>(new_root->children[0]);
    FreeNode(new_root);  // fresh and never published — no reader can hold it
    new_root = collapsed;
  }

  root_.store(new_root, release);
  size_.fetch_sub(1, relaxed);
  for (BPlusNode* n : retired) DeferFreeNode(n);
  g_structural_epoch.fetch_add(1, release);
  Reclaim();
  return true;
}
