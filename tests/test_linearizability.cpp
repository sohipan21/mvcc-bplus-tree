#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "btree.h"
#include "mvcc.h"

// ===========================================================================
// Strict linearizability / consistency stress tests.
//
// These differ from tests/test_concurrent.cpp in ONE deliberate way: they
// assert the STRICT invariant. The concurrent suite checks
//     if (val != nullptr && U(val) != expected) fail;
// which TOLERATES a spurious nullptr. Here, a key that is present for the
// entire run must ALWAYS read back its exact value and NEVER nullptr. Any
// violation is a real linearizability bug, not a tolerated race.
//
// ---------------------------------------------------------------------------
// HISTORY: these tests caught a real bug that ThreadSanitizer could not.
// ---------------------------------------------------------------------------
// The original tree mutated internal nodes IN PLACE during merges/borrows:
// keys[]/children[] were shifted with relaxed stores and num_keys published
// with a *trailing* release. A reader that had loaded the OLD num_keys had no
// happens-before edge to those shifts, read a torn separator array, and
// descended into the wrong subtree -- returning nullptr for a key that was
// structurally present the whole time, e.g.
//     Read(61, snap) = 0, expected frozen value 610
// TSan reported ZERO races on the same runs (every field was atomic), which
// is exactly the point: data-race freedom != linearizability. The two
// structural-churn tests below reproduced this reliably and were DISABLED_.
//
// The fix: structural writes are now copy-on-write (see btree.cpp). A writer
// builds fresh nodes for the whole modified path and publishes with a single
// release store of root_; readers acquire-load root_ once and traverse an
// immutable snapshot. Torn intermediate states are unrepresentable, so these
// tests are re-enabled and act as permanent regression guards.
//
// MvccScanExactSetUnderValueChurn guards MVCC scan snapshot isolation; since
// the COW rewrite, Scan is also structurally snapshot-consistent (one root
// load per scan).
// ===========================================================================

namespace {

void* V(uint64_t v) { return reinterpret_cast<void*>(v); }
uint64_t U(void* p) { return reinterpret_cast<uint64_t>(p); }

// Records the first observed violation so a failing run prints a single,
// concrete, reproducible detail line instead of a flood.
struct Violation {
  std::atomic<bool> hit{false};
  std::mutex mu;
  std::string detail;

  void Record(const std::string& d) {
    bool expected = false;
    if (hit.compare_exchange_strong(expected, true)) {
      std::lock_guard<std::mutex> lock(mu);
      detail = d;
    }
  }
};

// Partition [1, key_max] into a stable band (k % stride == 1, inserted once and
// never removed) and a churn band (everything else, repeatedly inserted and
// deleted). Interleaving the two bands in key order guarantees that churn-band
// splits/merges restructure internal nodes that lie on stable-key search paths.
struct KeyBands {
  std::vector<uint64_t> stable;
  std::vector<uint64_t> churn;
};

KeyBands MakeBands(uint64_t key_max, uint64_t stride) {
  KeyBands b;
  for (uint64_t k = 1; k <= key_max; ++k) {
    if (k % stride == 1)
      b.stable.push_back(k);
    else
      b.churn.push_back(k);
  }
  return b;
}

}  // namespace

// ---------------------------------------------------------------------------
// Test A -- BPlusTree::Search strict presence under structural churn.
//
// Regression guard for the internal-node merge/borrow misroute (see HISTORY
// above). Stable keys are structurally present for the whole run, so a
// correct lock-free reader must find every one of them on every lookup.
// ---------------------------------------------------------------------------
TEST(Linearizability, StableKeysAlwaysVisibleUnderStructuralChurn) {
  constexpr uint64_t kKeyMax = 1024;
  constexpr uint64_t kStride = 4;  // stable keys: 1, 5, 9, ... (256 of them)
  constexpr int kReaders = 8;
  constexpr int kWriterRounds = 15;

  const KeyBands bands = MakeBands(kKeyMax, kStride);

  BPlusTree tree;
  for (uint64_t k : bands.stable) tree.Insert(k, V(k * 10));

  Violation v;
  std::atomic<bool> writer_done{false};

  // One writer churns the non-stable keys in a tight insert-then-delete loop.
  // Writes are serialized behind write_mutex_, so a single writer already
  // produces continuous structural mutation; more writers would only contend.
  std::thread writer([&]() {
    for (int round = 0; round < kWriterRounds; ++round) {
      for (uint64_t k : bands.churn) tree.Insert(k, V(k * 10));
      for (uint64_t k : bands.churn) tree.Delete(k);
    }
    writer_done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int t = 0; t < kReaders; ++t) {
    readers.emplace_back([&, t]() {
      std::mt19937_64 rng(0xABCDEF0ULL + static_cast<uint64_t>(t));
      std::uniform_int_distribution<size_t> pick(0, bands.stable.size() - 1);
      uint64_t iters = 0;
      while (!writer_done.load(std::memory_order_acquire) || iters < 1000) {
        uint64_t key = bands.stable[pick(rng)];
        void* r = tree.Search(key);
        if (U(r) != key * 10) {
          v.Record("thread " + std::to_string(t) + " iter " + std::to_string(iters) + ": Search(" +
                   std::to_string(key) + ") = " + std::to_string(U(r)) + ", expected " +
                   std::to_string(key * 10));
        }
        ++iters;
      }
    });
  }

  writer.join();
  for (auto& th : readers) th.join();

  EXPECT_FALSE(v.hit.load())
      << "Strict linearizability violation on the lock-free read path: " << v.detail
      << "\nA structurally-present key read back wrong/absent while churn-band keys "
         "were merging -- this reproduces the internal-node mid-restructure misroute.";
}

// ---------------------------------------------------------------------------
// Test B -- MVCC Read(key, snap) immutability under structural + value churn.
//
// Combines two guarantees: (1) snapshot isolation -- a read at a fixed snapshot
// must be frozen even though stable keys are updated to new values at newer
// timestamps; and (2) the same lock-free structural descent as Test A, since
// MVCCTree::Read -> SearchRaw -> SearchNode runs while churn keys split/merge.
// ---------------------------------------------------------------------------
TEST(Linearizability, MvccSnapshotReadIsStableUnderChurn) {
  constexpr uint64_t kKeyMax = 1024;
  constexpr uint64_t kStride = 4;
  constexpr int kReaders = 6;
  constexpr int kWriterRounds = 12;

  const KeyBands bands = MakeBands(kKeyMax, kStride);

  MVCCTree tree;
  for (uint64_t k : bands.stable) tree.Insert(k, V(k * 10));

  // Snapshot now: sees exactly the stable keys at value k*10. Every write below
  // happens at a strictly newer timestamp, so a read at this snapshot is frozen.
  const uint64_t snap = tree.BeginRead();

  Violation v;
  std::atomic<bool> writer_done{false};

  std::thread writer([&]() {
    for (int round = 0; round < kWriterRounds; ++round) {
      for (uint64_t k : bands.churn) tree.Insert(k, V(k * 99));     // structural churn
      for (uint64_t k : bands.stable) tree.Update(k, V(k * 1000));  // newer stable versions
      for (uint64_t k : bands.churn) tree.Delete(k);
    }
    writer_done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int t = 0; t < kReaders; ++t) {
    readers.emplace_back([&, t]() {
      std::mt19937_64 rng(777ULL + static_cast<uint64_t>(t));
      std::uniform_int_distribution<size_t> pick(0, bands.stable.size() - 1);
      uint64_t iters = 0;
      while (!writer_done.load(std::memory_order_acquire) || iters < 500) {
        uint64_t key = bands.stable[pick(rng)];
        void* r = tree.Read(key, snap);
        if (U(r) != key * 10) {
          v.Record("Read(" + std::to_string(key) + ", snap) = " + std::to_string(U(r)) +
                   ", expected frozen value " + std::to_string(key * 10));
        }
        ++iters;
      }
    });
  }

  writer.join();
  for (auto& th : readers) th.join();

  EXPECT_FALSE(v.hit.load()) << "MVCC snapshot read was not frozen / misrouted under churn: "
                             << v.detail;
}

// ---------------------------------------------------------------------------
// Test C -- MVCC Scan exact-set under value-only concurrency.
//
// Writers append new versions to the stable keys while a scanner walks the
// range at a frozen snapshot; the scan must return exactly the stable set at
// the original values. (Since the copy-on-write rewrite, BPlusTree::Scan is
// also structurally snapshot-consistent -- it loads root_ once per scan --
// so this guards the full MVCC visibility contract, not a weakened one.)
// ---------------------------------------------------------------------------
TEST(Linearizability, MvccScanExactSetUnderValueChurn) {
  constexpr uint64_t kKeyMax = 1024;
  constexpr uint64_t kStride = 4;
  constexpr int kWriterRounds = 200;

  const KeyBands bands = MakeBands(kKeyMax, kStride);
  const std::unordered_set<uint64_t> stable_set(bands.stable.begin(), bands.stable.end());

  MVCCTree tree;
  for (uint64_t k : bands.stable) tree.Insert(k, V(k * 10));
  const uint64_t snap = tree.BeginRead();

  Violation v;
  std::atomic<bool> writer_done{false};

  // Value-only churn: Update existing stable keys (appends versions, no split/merge).
  std::thread writer([&]() {
    for (int round = 0; round < kWriterRounds; ++round) {
      for (uint64_t k : bands.stable) tree.Update(k, V(k * 1000 + static_cast<uint64_t>(round)));
    }
    writer_done.store(true, std::memory_order_release);
  });

  std::thread scanner([&]() {
    uint64_t iters = 0;
    while (!writer_done.load(std::memory_order_acquire) || iters < 50) {
      size_t seen = 0;
      tree.Scan(1, kKeyMax, snap, [&](uint64_t key, void* val) {
        ++seen;
        if (stable_set.find(key) == stable_set.end())
          v.Record("Scan returned non-stable key " + std::to_string(key));
        if (U(val) != key * 10)
          v.Record("Scan key " + std::to_string(key) + " val " + std::to_string(U(val)) +
                   ", expected frozen " + std::to_string(key * 10));
      });
      if (seen != bands.stable.size())
        v.Record("Scan returned " + std::to_string(seen) + " keys, expected " +
                 std::to_string(bands.stable.size()));
      ++iters;
    }
  });

  writer.join();
  scanner.join();

  EXPECT_FALSE(v.hit.load()) << "MVCC snapshot scan was not consistent: " << v.detail;
}
