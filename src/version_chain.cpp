#include "version_chain.h"

#include <cassert>

#include "epoch_reclamation.h"

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

VersionChain::~VersionChain() {
  // Destructor runs single-threaded; relaxed loads are fine.
  VersionNode* node = head_.load(std::memory_order_relaxed);
  while (node != nullptr) {
    VersionNode* next = node->next.load(std::memory_order_relaxed);
    delete node;
    node = next;
  }
}

// ---------------------------------------------------------------------------
// Append  (write path — acquires version_lock_ internally)
// ---------------------------------------------------------------------------

void VersionChain::Append(void* value, uint64_t ts) {
  VersionNode* new_node = new VersionNode(value, ts, 0);
  std::lock_guard<std::mutex> lock(version_lock_);
  new_node->next.store(head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  head_.store(new_node, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// MarkDeleted  (write path — acquires version_lock_ internally)
// ---------------------------------------------------------------------------

void VersionChain::MarkDeleted(uint64_t ts) {
  std::lock_guard<std::mutex> lock(version_lock_);
  VersionNode* head = head_.load(std::memory_order_relaxed);
  assert(head != nullptr && "MarkDeleted called on empty chain");
  assert(head->deleted_at.load(std::memory_order_relaxed) == 0 && "head version already deleted");
  head->deleted_at.store(ts, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Read  (read path — lock-free, called from Search())
// ---------------------------------------------------------------------------

void* VersionChain::Read(uint64_t snapshot_ts) const {
  // acquire: pairs with Append's release store, ensuring we see all fields of
  // the node whose pointer we just loaded.
  const VersionNode* node = head_.load(std::memory_order_acquire);
  while (node != nullptr) {
    uint64_t del = node->deleted_at.load(std::memory_order_acquire);
    if (node->created_at <= snapshot_ts && (del == 0 || del > snapshot_ts)) {
      return node->value;
    }
    // acquire: traverse the chain with the same happens-before guarantee.
    node = node->next.load(std::memory_order_acquire);
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// HasLiveVersion  (may be called from read path)
// ---------------------------------------------------------------------------

bool VersionChain::HasLiveVersion() const {
  const VersionNode* head = head_.load(std::memory_order_acquire);
  return head != nullptr && head->deleted_at.load(std::memory_order_acquire) == 0;
}

// ---------------------------------------------------------------------------
// Prune  (writer path — defers old VersionNodes for EBR reclamation)
// ---------------------------------------------------------------------------

void VersionChain::Prune(uint64_t safe_epoch) {
  std::lock_guard<std::mutex> lock(version_lock_);

  VersionNode* prev = nullptr;
  VersionNode* node = head_.load(std::memory_order_relaxed);

  while (node != nullptr) {
    VersionNode* next = node->next.load(std::memory_order_relaxed);
    uint64_t del = node->deleted_at.load(std::memory_order_relaxed);
    if (del != 0 && del <= safe_epoch) {
      if (prev == nullptr) {
        head_.store(next, std::memory_order_release);
      } else {
        prev->next.store(next, std::memory_order_release);
      }
      DeferFreeVersionNode(node);
    } else {
      prev = node;
    }
    node = next;
  }
}
