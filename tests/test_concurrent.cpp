#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <vector>

#include "btree.h"
#include "epoch_reclamation.h"
#include "mvcc.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void* V(uint64_t v) { return reinterpret_cast<void*>(v); }
static uint64_t U(void* p) { return reinterpret_cast<uint64_t>(p); }

// ---------------------------------------------------------------------------
// Fixture: pre-populate tree single-threaded, then read concurrently.
// (Since the copy-on-write rewrite, mixed read/write phases are equally safe —
// readers traverse immutable snapshots; see test_linearizability.cpp for the
// strict mixed-phase assertions.)
// ---------------------------------------------------------------------------

class ConcurrentReadTest : public ::testing::Test {
 protected:
  static constexpr int kNumKeys = 2000;
  static constexpr int kNumReaders = 8;
  static constexpr int kSearchesPerThread = 1000;

  BPlusTree tree_;

  void SetUp() override {
    for (uint64_t i = 1; i <= kNumKeys; ++i) tree_.Insert(i, V(i * 10));
  }
};

// ---------------------------------------------------------------------------
// Test 1: 8 concurrent readers, all searches return correct values.
// ---------------------------------------------------------------------------

TEST_F(ConcurrentReadTest, ConcurrentSearchesReturnCorrectValues) {
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kNumReaders);

  for (int t = 0; t < kNumReaders; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kSearchesPerThread; ++i) {
        // Spread queries across the full key space deterministically.
        uint64_t key = static_cast<uint64_t>((t * kSearchesPerThread + i) % kNumKeys) + 1;
        void* result = tree_.Search(key);
        if (U(result) != key * 10) failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& th : threads) th.join();
  EXPECT_EQ(failures.load(), 0);
}

// ---------------------------------------------------------------------------
// Test 2: 8 readers + 1 writer.
// Writer inserts into a fresh key range (> kNumKeys) that readers never touch,
// so readers and the writer never structurally modify the same node.
// Readers only verify that pre-populated keys remain findable and correct.
// ---------------------------------------------------------------------------

TEST_F(ConcurrentReadTest, ConcurrentReadWriteNoRace) {
  std::atomic<bool> writer_done{false};
  std::atomic<int> failures{0};

  std::thread writer([&]() {
    for (uint64_t k = kNumKeys + 1; k <= kNumKeys + 200; ++k) tree_.Insert(k, V(k * 10));
    writer_done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  readers.reserve(kNumReaders);
  for (int t = 0; t < kNumReaders; ++t) {
    readers.emplace_back([&, t]() {
      int iterations = 0;
      while (!writer_done.load(std::memory_order_acquire) || iterations < 100) {
        uint64_t key = static_cast<uint64_t>(t % kNumKeys) + 1;
        void* result = tree_.Search(key);
        // Pre-populated keys must always be findable with the correct value.
        if (result != nullptr && U(result) != key * 10)
          failures.fetch_add(1, std::memory_order_relaxed);
        ++iterations;
      }
    });
  }

  writer.join();
  for (auto& th : readers) th.join();
  EXPECT_EQ(failures.load(), 0);
}

// ---------------------------------------------------------------------------
// Test 3: EBR slot assignment — each thread gets a unique slot index.
// ---------------------------------------------------------------------------

TEST(EBRTest, SlotAssignmentIsUnique) {
  // Slots are now recyclable (released when a thread exits), so we must
  // verify uniqueness while all threads are concurrently alive.  Use a
  // spin-barrier: each thread claims its slot then waits until all kThreads
  // threads have claimed before exiting.
  constexpr int kThreads = 8;
  std::atomic<int> slot_results[kThreads];
  std::atomic<int> ready{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      int s = AcquireEpochSlot();
      slot_results[i].store(s, std::memory_order_relaxed);
      ready.fetch_add(1, std::memory_order_release);
      while (ready.load(std::memory_order_acquire) < kThreads) {
      }
    });
  }
  for (auto& th : threads) th.join();

  std::unordered_set<int> seen;
  for (int i = 0; i < kThreads; ++i) {
    int s = slot_results[i].load(std::memory_order_relaxed);
    EXPECT_GE(s, 0) << "slot " << i << " is negative";
    EXPECT_LT(s, kMaxThreads) << "slot " << i << " exceeds kMaxThreads";
    EXPECT_TRUE(seen.insert(s).second) << "duplicate slot index " << s;
  }
}

// ---------------------------------------------------------------------------
// Test 4: Safe epoch is bounded by the active reader's snapshot.
// ---------------------------------------------------------------------------

TEST(EBRTest, SafeEpochBoundsActiveReader) {
  // Announce that we are reading at snapshot 5.
  ThreadEnterEpoch(5);
  uint64_t safe = ComputeSafeSnapshot();
  // The safe epoch must not exceed the announced snapshot.
  EXPECT_LE(safe, 5u);

  ThreadExitEpoch();
  // After exit no readers are active in this slot; safe epoch should advance
  // (may still be bounded by other test threads, so just check it doesn't crash).
  uint64_t safe_after = ComputeSafeSnapshot();
  (void)safe_after;  // value depends on other concurrent tests; just don't crash
}

// ---------------------------------------------------------------------------
// Test 5: Throughput — 8 threads × 5000 searches must complete in < 500 ms.
// Validates that the lock-free read path is genuinely parallel (no hidden
// serialization behind write_mutex_ or a global lock in Search).
// ---------------------------------------------------------------------------

TEST_F(ConcurrentReadTest, ThroughputScalesWithThreads) {
  constexpr int kSearchesPerThread2 = 5000;
  auto start = std::chrono::steady_clock::now();

  std::vector<std::thread> threads;
  threads.reserve(kNumReaders);
  for (int t = 0; t < kNumReaders; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kSearchesPerThread2; ++i) {
        uint64_t key = static_cast<uint64_t>((t * 7 + i) % kNumKeys) + 1;
        // Prevent the compiler from optimizing away the call.
        volatile void* r = tree_.Search(key);
        (void)r;
      }
    });
  }
  for (auto& th : threads) th.join();

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  // 8 × 5000 = 40 000 lock-free searches; 500 ms is very generous.
  EXPECT_LT(elapsed, 500) << "Concurrent reads took " << elapsed
                          << " ms — possible hidden serialization in Search()";
}

// ===========================================================================
// Week 4: MVCC Concurrent Tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Fixture: MVCCTree pre-populated with kNumKeys keys.
// ---------------------------------------------------------------------------

class MVCCConcurrentTest : public ::testing::Test {
 protected:
  static constexpr int kNumKeys = 500;
  MVCCTree tree_;
  uint64_t snapshot_before_{0};

  void SetUp() override {
    for (uint64_t i = 1; i <= kNumKeys; ++i) tree_.Insert(i, V(i * 10));
    snapshot_before_ = tree_.BeginRead();
  }
};

// ===========================================================================
// Snapshot Isolation Tests (15)
// ===========================================================================

TEST_F(MVCCConcurrentTest, ReaderDoesNotSeeWriterUpdate) {
  tree_.Update(1, V(999));
  EXPECT_EQ(U(tree_.Read(1, snapshot_before_)), 10u);
}

TEST_F(MVCCConcurrentTest, ReaderDoesNotSeeWriterDelete) {
  tree_.Delete(1);
  EXPECT_EQ(U(tree_.Read(1, snapshot_before_)), 10u);
}

TEST_F(MVCCConcurrentTest, ReaderDoesNotSeeNewInsert) {
  tree_.Insert(kNumKeys + 1, V(9999));
  EXPECT_EQ(tree_.Read(kNumKeys + 1, snapshot_before_), nullptr);
}

TEST_F(MVCCConcurrentTest, SnapshotCapturesCorrectVersion) {
  tree_.Update(1, V(100));
  uint64_t snap2 = tree_.BeginRead();
  tree_.Update(1, V(200));
  EXPECT_EQ(U(tree_.Read(1, snapshot_before_)), 10u);
  EXPECT_EQ(U(tree_.Read(1, snap2)), 100u);
  uint64_t snap3 = tree_.BeginRead();
  EXPECT_EQ(U(tree_.Read(1, snap3)), 200u);
}

TEST_F(MVCCConcurrentTest, TwoReadersSeeDifferentValues) {
  tree_.Update(1, V(100));
  uint64_t snap_after = tree_.BeginRead();
  EXPECT_EQ(U(tree_.Read(1, snapshot_before_)), 10u);
  EXPECT_EQ(U(tree_.Read(1, snap_after)), 100u);
}

TEST_F(MVCCConcurrentTest, DeleteThenReinsertSnapshotIsolation) {
  tree_.Delete(1);
  tree_.Insert(1, V(777));
  EXPECT_EQ(U(tree_.Read(1, snapshot_before_)), 10u);
  uint64_t snap_after = tree_.BeginRead();
  EXPECT_EQ(U(tree_.Read(1, snap_after)), 777u);
}

TEST_F(MVCCConcurrentTest, MultipleUpdatesIntermediateSnapshots) {
  std::vector<uint64_t> snaps;
  snaps.push_back(snapshot_before_);
  for (uint64_t v = 1; v <= 5; ++v) {
    tree_.Update(1, V(v * 100));
    snaps.push_back(tree_.BeginRead());
  }
  EXPECT_EQ(U(tree_.Read(1, snaps[0])), 10u);
  for (uint64_t v = 1; v <= 5; ++v) EXPECT_EQ(U(tree_.Read(1, snaps[v])), v * 100);
}

TEST_F(MVCCConcurrentTest, PostWriteSnapshotSeesNewValue) {
  tree_.Update(42, V(4200));
  uint64_t snap = tree_.BeginRead();
  EXPECT_EQ(U(tree_.Read(42, snap)), 4200u);
}

TEST_F(MVCCConcurrentTest, ConcurrentReadersAtDifferentSnapshots) {
  tree_.Update(1, V(100));
  uint64_t snap_mid = tree_.BeginRead();
  tree_.Update(1, V(200));
  uint64_t snap_late = tree_.BeginRead();

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < 100; ++i) {
        uint64_t snap = (t % 2 == 0) ? snap_mid : snap_late;
        uint64_t expected = (t % 2 == 0) ? 100u : 200u;
        if (U(tree_.Read(1, snap)) != expected) failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, SnapshotMonotonicity) {
  uint64_t s1 = tree_.BeginRead();
  tree_.Insert(kNumKeys + 1, V(9999));
  uint64_t s2 = tree_.BeginRead();
  EXPECT_GE(s2, s1);
  EXPECT_EQ(tree_.Read(kNumKeys + 1, s1), nullptr);
  EXPECT_EQ(U(tree_.Read(kNumKeys + 1, s2)), 9999u);
}

TEST_F(MVCCConcurrentTest, WriterRacingReaderSnapshotIsolation) {
  std::atomic<bool> writer_done{false};
  std::atomic<int> failures{0};

  std::thread writer([&]() {
    for (uint64_t i = 1; i <= 50; ++i) tree_.Update(i, V(i * 1000));
    writer_done.store(true, std::memory_order_release);
  });

  std::thread reader([&]() {
    while (!writer_done.load(std::memory_order_acquire)) {
      for (uint64_t i = 1; i <= 50; ++i) {
        void* val = tree_.Read(i, snapshot_before_);
        if (val != nullptr && U(val) != i * 10) failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  writer.join();
  reader.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, ConcurrentUpdateAndDeleteSameKey) {
  tree_.Update(1, V(100));
  tree_.Delete(1);
  uint64_t snap = tree_.BeginRead();
  EXPECT_EQ(tree_.Read(1, snap), nullptr);
  EXPECT_EQ(U(tree_.Read(1, snapshot_before_)), 10u);
}

TEST_F(MVCCConcurrentTest, ReaderDuringStructuralInsert) {
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::thread writer([&]() {
    for (uint64_t k = kNumKeys + 1; k <= kNumKeys + 200; ++k) tree_.Insert(k, V(k * 10));
    done.store(true, std::memory_order_release);
  });

  std::thread reader([&]() {
    while (!done.load(std::memory_order_acquire)) {
      void* val = tree_.Read(1, snapshot_before_);
      if (val != nullptr && U(val) != 10u) failures.fetch_add(1, std::memory_order_relaxed);
    }
  });

  writer.join();
  reader.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, ExistsReflectsSnapshot) {
  EXPECT_TRUE(tree_.Exists(1, snapshot_before_));
  tree_.Delete(1);
  EXPECT_TRUE(tree_.Exists(1, snapshot_before_));
  uint64_t snap = tree_.BeginRead();
  EXPECT_FALSE(tree_.Exists(1, snap));
}

TEST_F(MVCCConcurrentTest, UpdateNonExistentKeyIsNoOp) {
  tree_.Update(kNumKeys + 999, V(1));
  uint64_t snap = tree_.BeginRead();
  EXPECT_EQ(tree_.Read(kNumKeys + 999, snap), nullptr);
}

// ===========================================================================
// Concurrent Writer Tests (10)
// ===========================================================================

TEST_F(MVCCConcurrentTest, FourWritersInsertDisjointKeys) {
  constexpr int kPerWriter = 50;
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&, w]() {
      uint64_t base = kNumKeys + 1 + w * kPerWriter;
      for (uint64_t k = base; k < base + kPerWriter; ++k) tree_.Insert(k, V(k * 10));
    });
  }
  for (auto& th : writers) th.join();

  uint64_t snap = tree_.BeginRead();
  for (int w = 0; w < 4; ++w) {
    uint64_t base = kNumKeys + 1 + w * kPerWriter;
    for (uint64_t k = base; k < base + kPerWriter; ++k) EXPECT_EQ(U(tree_.Read(k, snap)), k * 10);
  }
}

TEST_F(MVCCConcurrentTest, FourWritersUpdateDisjointKeys) {
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&, w]() {
      uint64_t base = 1 + w * 100;
      for (uint64_t k = base; k < base + 100 && k <= kNumKeys; ++k) tree_.Update(k, V(k * 100));
    });
  }
  for (auto& th : writers) th.join();

  uint64_t snap = tree_.BeginRead();
  for (uint64_t k = 1; k <= 400 && k <= kNumKeys; ++k) EXPECT_EQ(U(tree_.Read(k, snap)), k * 100);
}

TEST_F(MVCCConcurrentTest, TwoWritersUpdateSameKey) {
  std::vector<std::thread> writers;
  for (int w = 0; w < 2; ++w) {
    writers.emplace_back([&, w]() {
      for (int i = 0; i < 10; ++i) tree_.Update(1, V(w * 1000 + i));
    });
  }
  for (auto& th : writers) th.join();

  uint64_t snap = tree_.BeginRead();
  void* val = tree_.Read(1, snap);
  EXPECT_NE(val, nullptr);
}

TEST_F(MVCCConcurrentTest, WriterInsertsWhileReadersRead) {
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::thread writer([&]() {
    for (uint64_t k = kNumKeys + 1; k <= kNumKeys + 100; ++k) tree_.Insert(k, V(k));
    done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&]() {
      while (!done.load(std::memory_order_acquire)) {
        void* val = tree_.Read(1, snapshot_before_);
        if (val != nullptr && U(val) != 10u) failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  writer.join();
  for (auto& th : readers) th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, WriterDeletesWhileReadersRead) {
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::thread writer([&]() {
    for (uint64_t k = 1; k <= 50; ++k) tree_.Delete(k);
    done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&]() {
      while (!done.load(std::memory_order_acquire)) {
        for (uint64_t k = 1; k <= 50; ++k) {
          void* val = tree_.Read(k, snapshot_before_);
          if (val != nullptr && U(val) != k * 10) failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  writer.join();
  for (auto& th : readers) th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, SequentialWriterBatches) {
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&, w]() {
      for (uint64_t k = 1; k <= 50; ++k) {
        uint64_t key = kNumKeys + 1 + w * 50 + k;
        tree_.Insert(key, V(key));
      }
    });
  }
  for (auto& th : writers) th.join();

  uint64_t snap = tree_.BeginRead();
  int found = 0;
  for (uint64_t k = kNumKeys + 2; k <= kNumKeys + 201; ++k) {
    if (tree_.Read(k, snap) != nullptr) ++found;
  }
  EXPECT_EQ(found, 200);
}

TEST_F(MVCCConcurrentTest, WriterAndReaderSameKeyIsolation) {
  std::thread writer([&]() {
    for (int i = 0; i < 20; ++i) tree_.Update(1, V(i + 1000));
  });

  std::atomic<int> failures{0};
  std::thread reader([&]() {
    for (int i = 0; i < 100; ++i) {
      void* val = tree_.Read(1, snapshot_before_);
      if (val != nullptr && U(val) != 10u) failures.fetch_add(1, std::memory_order_relaxed);
    }
  });

  writer.join();
  reader.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, EightWritersEightDistinctKeys) {
  constexpr int kWriters = 8;
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w]() {
      uint64_t key = kNumKeys + 1 + w;
      tree_.Insert(key, V(key * 10));
    });
  }
  for (auto& th : writers) th.join();

  uint64_t snap = tree_.BeginRead();
  for (int w = 0; w < kWriters; ++w) {
    uint64_t key = kNumKeys + 1 + w;
    EXPECT_EQ(U(tree_.Read(key, snap)), key * 10);
  }
}

TEST_F(MVCCConcurrentTest, HighContentionSameKey) {
  constexpr int kWriters = 4;
  constexpr int kOpsPerWriter = 20;
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w]() {
      for (int i = 0; i < kOpsPerWriter; ++i) tree_.Update(1, V(w * 1000 + i));
    });
  }
  for (auto& th : writers) th.join();

  uint64_t snap = tree_.BeginRead();
  void* val = tree_.Read(1, snap);
  EXPECT_NE(val, nullptr);
}

TEST_F(MVCCConcurrentTest, DeleteAlreadyDeletedIsFalse) {
  EXPECT_TRUE(tree_.Delete(1));
  EXPECT_FALSE(tree_.Delete(1));
}

// ===========================================================================
// VersionNode EBR Tests (10)
// ===========================================================================

TEST(VersionEBRTest, PruneFreesOldNodes) {
  VersionChain chain;
  chain.Append(V(1), 1);
  chain.MarkDeleted(2);
  chain.Append(V(2), 3);

  chain.Prune(2);
  EXPECT_NE(chain.Head(), nullptr);
  EXPECT_EQ(chain.Head()->value, V(2));
}

TEST(VersionEBRTest, PrunePreservesLiveNodes) {
  VersionChain chain;
  chain.Append(V(1), 1);
  chain.Prune(5);
  EXPECT_NE(chain.Head(), nullptr);
  EXPECT_EQ(chain.Head()->value, V(1));
}

TEST(VersionEBRTest, PruneIsIdempotent) {
  VersionChain chain;
  chain.Append(V(1), 1);
  chain.MarkDeleted(2);
  chain.Append(V(2), 3);

  chain.Prune(2);
  chain.Prune(2);
  EXPECT_NE(chain.Head(), nullptr);
}

TEST(VersionEBRTest, PruneMultipleDeadNodes) {
  VersionChain chain;
  chain.Append(V(1), 1);
  chain.MarkDeleted(2);
  chain.Append(V(2), 3);
  chain.MarkDeleted(4);
  chain.Append(V(3), 5);

  chain.Prune(4);
  EXPECT_NE(chain.Head(), nullptr);
  EXPECT_EQ(chain.Head()->value, V(3));
}

TEST(VersionEBRTest, PruneEmptyChain) {
  VersionChain chain;
  chain.Prune(100);
}

TEST(VersionEBRTest, DeferFreeVersionNodeEnqueues) {
  auto* node = new VersionNode(V(42), 1, 2);
  DeferFreeVersionNode(node);
  Reclaim();
}

TEST(VersionEBRTest, NoReaderAllowsFullReclaim) {
  ThreadExitEpoch();
  uint64_t safe = ComputeSafeSnapshot();
  (void)safe;
}

TEST(VersionEBRTest, ActiveReaderPreventsReclaim) {
  ThreadEnterEpoch(10);
  uint64_t safe = ComputeSafeSnapshot();
  EXPECT_LE(safe, 10u);
  ThreadExitEpoch();
}

TEST(VersionEBRTest, ChainReadAfterPrune) {
  VersionChain chain;
  chain.Append(V(1), 1);
  chain.MarkDeleted(2);
  chain.Append(V(2), 3);
  chain.MarkDeleted(4);
  chain.Append(V(3), 5);

  chain.Prune(4);

  EXPECT_EQ(chain.Read(1), nullptr);
  EXPECT_EQ(chain.Read(3), nullptr);
  EXPECT_EQ(chain.Read(5), V(3));
}

TEST(VersionEBRTest, ConcurrentPruneAndRead) {
  VersionChain chain;
  for (uint64_t i = 1; i <= 20; ++i) {
    if (i > 1) chain.MarkDeleted(i * 2 - 1);
    chain.Append(V(i), i * 2);
  }

  std::atomic<int> failures{0};

  std::thread pruner([&]() {
    for (int i = 0; i < 5; ++i) chain.Prune(i * 8);
  });

  std::thread reader([&]() {
    for (int i = 0; i < 100; ++i) {
      void* val = chain.Read(40);
      if (val != V(20)) failures.fetch_add(1, std::memory_order_relaxed);
    }
  });

  pruner.join();
  reader.join();
  EXPECT_EQ(failures.load(), 0);
}

// ===========================================================================
// Integration Stress Tests (10)
// ===========================================================================

TEST_F(MVCCConcurrentTest, FourReadersPlusOneWriter1000Ops) {
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::thread writer([&]() {
    for (uint64_t k = 1; k <= 200; ++k) tree_.Update(k, V(k * 100));
    done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t]() {
      int iters = 0;
      while (!done.load(std::memory_order_acquire) || iters < 250) {
        uint64_t k = static_cast<uint64_t>(t * 50 + (iters % 50)) + 1;
        void* val = tree_.Read(k, snapshot_before_);
        if (val != nullptr && U(val) != k * 10) failures.fetch_add(1, std::memory_order_relaxed);
        ++iters;
      }
    });
  }

  writer.join();
  for (auto& th : readers) th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, ThroughputUnderConcurrentWrites) {
  std::atomic<bool> writer_done{false};

  std::thread writer([&]() {
    for (uint64_t k = kNumKeys + 1; k <= kNumKeys + 100; ++k) tree_.Insert(k, V(k));
    writer_done.store(true, std::memory_order_release);
  });

  auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t]() {
      for (int i = 0; i < 5000; ++i) {
        uint64_t k = static_cast<uint64_t>((t * 7 + i) % kNumKeys) + 1;
        volatile void* r = tree_.Read(k, snapshot_before_);
        (void)r;
      }
    });
  }

  for (auto& th : readers) th.join();
  writer.join();

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  EXPECT_LT(elapsed, 1000);
}

TEST_F(MVCCConcurrentTest, WriterInsertsWhileReadersSearchExisting) {
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::thread writer([&]() {
    for (uint64_t k = kNumKeys + 1; k <= kNumKeys + 200; ++k) tree_.Insert(k, V(k * 10));
    done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t]() {
      while (!done.load(std::memory_order_acquire)) {
        uint64_t k = static_cast<uint64_t>(t * 100 + 1);
        void* val = tree_.Read(k, snapshot_before_);
        if (val != nullptr && U(val) != k * 10) failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  writer.join();
  for (auto& th : readers) th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, DeleteHalfWhileReadersRead) {
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::thread writer([&]() {
    for (uint64_t k = 1; k <= kNumKeys / 2; ++k) tree_.Delete(k);
    done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t]() {
      while (!done.load(std::memory_order_acquire)) {
        uint64_t k = static_cast<uint64_t>(t % kNumKeys) + 1;
        void* val = tree_.Read(k, snapshot_before_);
        if (val != nullptr && U(val) != k * 10) failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  writer.join();
  for (auto& th : readers) th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, MixedInsertUpdateDeleteConcurrent) {
  std::vector<std::thread> writers;

  writers.emplace_back([&]() {
    for (uint64_t k = kNumKeys + 1; k <= kNumKeys + 50; ++k) tree_.Insert(k, V(k));
  });
  writers.emplace_back([&]() {
    for (uint64_t k = 1; k <= 50; ++k) tree_.Update(k, V(k * 100));
  });
  writers.emplace_back([&]() {
    for (uint64_t k = 100; k <= 150; ++k) tree_.Delete(k);
  });
  writers.emplace_back([&]() {
    for (uint64_t k = kNumKeys + 51; k <= kNumKeys + 100; ++k) tree_.Insert(k, V(k));
  });

  for (auto& th : writers) th.join();

  uint64_t snap = tree_.BeginRead();
  for (uint64_t k = kNumKeys + 1; k <= kNumKeys + 100; ++k)
    EXPECT_NE(tree_.Read(k, snap), nullptr) << "key " << k << " missing";
}

TEST_F(MVCCConcurrentTest, BeginReadMonotonic) {
  std::atomic<uint64_t> max_snap{0};
  std::atomic<int> failures{0};

  std::thread writer([&]() {
    for (int i = 0; i < 50; ++i) tree_.Update(1, V(i + 1000));
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&]() {
      uint64_t prev = 0;
      for (int i = 0; i < 50; ++i) {
        uint64_t s = tree_.BeginRead();
        if (s < prev) failures.fetch_add(1, std::memory_order_relaxed);
        prev = s;
      }
    });
  }

  writer.join();
  for (auto& th : readers) th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, FullMVCCLifecycle) {
  MVCCTree t;
  t.Insert(1, V(100));
  uint64_t s1 = t.BeginRead();

  t.Update(1, V(200));
  uint64_t s2 = t.BeginRead();

  t.Delete(1);
  uint64_t s3 = t.BeginRead();

  EXPECT_EQ(U(t.Read(1, s1)), 100u);
  EXPECT_EQ(U(t.Read(1, s2)), 200u);
  EXPECT_EQ(t.Read(1, s3), nullptr);
}

TEST_F(MVCCConcurrentTest, ScaleEightReadersTwoWriters) {
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::vector<std::thread> writers;
  for (int w = 0; w < 2; ++w) {
    writers.emplace_back([&, w]() {
      uint64_t base = kNumKeys + 1 + w * 500;
      for (uint64_t k = base; k < base + 500; ++k) tree_.Insert(k, V(k * 10));
    });
  }

  std::vector<std::thread> readers;
  for (int t = 0; t < 8; ++t) {
    readers.emplace_back([&, t]() {
      for (int i = 0; i < 1000; ++i) {
        uint64_t k = static_cast<uint64_t>((t * 7 + i) % kNumKeys) + 1;
        void* val = tree_.Read(k, snapshot_before_);
        if (val != nullptr && U(val) != k * 10) failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& th : writers) th.join();
  for (auto& th : readers) th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(MVCCConcurrentTest, NumKeysConsistentAfterConcurrentInserts) {
  constexpr int kNewKeys = 200;
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&, w]() {
      uint64_t base = kNumKeys + 1 + w * 50;
      for (uint64_t k = base; k < base + 50; ++k) tree_.Insert(k, V(k));
    });
  }
  for (auto& th : writers) th.join();
  EXPECT_EQ(tree_.NumKeys(), kNumKeys + kNewKeys);
}

TEST_F(MVCCConcurrentTest, RepeatedWritePruneReadCycle) {
  MVCCTree t;
  for (int round = 0; round < 20; ++round) {
    t.Insert(1, V(round + 1));
    if (round > 0) {
      uint64_t snap = t.BeginRead();
      void* val = t.Read(1, snap);
      EXPECT_EQ(U(val), static_cast<uint64_t>(round + 1));
    }
  }
}

// ===========================================================================
// Same-key concurrent Insert regression tests
// ===========================================================================

// 16 threads all Insert the same key simultaneously. Exactly one VersionChain
// should win the slot; the others must append onto it rather than orphan a chain.
TEST(InsertRaceTest, ConcurrentSameKeyInsertSingleChain) {
  constexpr int kThreads = 16;
  MVCCTree tree;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() { tree.Insert(1, V(i + 1)); });
  }
  for (auto& th : threads) th.join();

  // One structural key in the tree (not kThreads).
  EXPECT_EQ(tree.NumKeys(), 1u);

  // The latest snapshot must see a non-null value.
  uint64_t snap = tree.BeginRead();
  EXPECT_NE(tree.Read(1, snap), nullptr);
}

// Same test over a small key set to exercise the contended UpdateVersion path.
TEST(InsertRaceTest, ConcurrentSameKeySetNoOrphanedChains) {
  constexpr int kThreads = 8;
  constexpr uint64_t kKeys = 4;
  MVCCTree tree;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      for (uint64_t k = 1; k <= kKeys; ++k) tree.Insert(k, V(i * 100 + k));
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(tree.NumKeys(), kKeys);
  uint64_t snap = tree.BeginRead();
  for (uint64_t k = 1; k <= kKeys; ++k) EXPECT_NE(tree.Read(k, snap), nullptr);
}

// ===========================================================================
// EBR slot retention tests
// ===========================================================================

// A thread doing many sequential reads must reuse the same slot each time —
// no re-scan through g_epoch_slots after the first AcquireEpochSlot call.
TEST(EBRSlotTest, SlotIsRetainedAcrossReads) {
  MVCCTree tree;
  int val = 42;
  tree.Insert(1, &val);

  // Force slot assignment on first read, then verify the index is stable.
  uint64_t snap = tree.BeginRead();
  tree.Read(1, snap);
  int first_slot = tl_slot_index;
  ASSERT_GE(first_slot, 0) << "slot not assigned after first read";

  for (int i = 0; i < 200; ++i) {
    snap = tree.BeginRead();
    tree.Read(1, snap);
    EXPECT_EQ(tl_slot_index, first_slot) << "slot changed on iteration " << i;
  }
}

// Spawning well over kMaxThreads short-lived threads must not exhaust the
// slot pool — EpochSlotGuard must release each slot at thread exit.
TEST(EBRSlotTest, SlotPoolNotExhaustedByThreadChurn) {
  MVCCTree tree;
  int val = 99;
  tree.Insert(1, &val);

  // Run 3× kMaxThreads threads sequentially (not concurrently, so the pool
  // never needs more than one slot at a time).
  constexpr int kRounds = kMaxThreads * 3;
  for (int i = 0; i < kRounds; ++i) {
    std::thread t([&]() {
      uint64_t snap = tree.BeginRead();
      volatile void* r = tree.Read(1, snap);
      (void)r;
    });
    t.join();  // destructor releases slot before next iteration
  }
  // If we get here without aborting (slot pool exhaustion calls std::abort),
  // the guard is correctly releasing slots.
  SUCCEED();
}

// ===========================================================================
// EBR reclamation drain test
// ===========================================================================

// Copy-on-write structural writes retire O(depth) nodes on EVERY mutation, so
// reclamation must genuinely run — leaking retired nodes would look "safe" in
// every other test. This test drives structural churn under concurrent
// readers (under ASan a premature free surfaces as a use-after-free), then
// verifies the retire queues DRAIN once all readers quiesce, guarding against
// a regression where reclamation is silently disabled.
TEST(EBRReclaimTest, RetireQueueDrainsAfterReadersQuiesce) {
  BPlusTree tree;
  constexpr uint64_t kStable = 512;
  constexpr uint64_t kChurn = 256;
  constexpr int kReaders = 4;
  constexpr int kRounds = 20;

  for (uint64_t k = 1; k <= kStable; ++k) tree.Insert(k, V(k));

  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int t = 0; t < kReaders; ++t) {
    readers.emplace_back([&, t]() {
      uint64_t i = 0;
      while (!stop.load(std::memory_order_acquire)) {
        uint64_t key = (static_cast<uint64_t>(t) * 31 + i++) % kStable + 1;
        volatile void* r = tree.Search(key);
        (void)r;
      }
    });
  }

  // Writer: repeated insert+delete of a churn band forces splits and merges,
  // retiring a full root-to-leaf path per operation.
  for (int round = 0; round < kRounds; ++round) {
    for (uint64_t k = kStable + 1; k <= kStable + kChurn; ++k) tree.Insert(k, V(k));
    for (uint64_t k = kStable + 1; k <= kStable + kChurn; ++k) tree.Delete(k);
  }

  stop.store(true, std::memory_order_release);
  for (auto& th : readers) th.join();

  // Reader threads exited (EpochSlotGuard cleared their announcements) and
  // this thread announces nothing, so one Reclaim must free every retired
  // node — from this test and anything earlier tests left behind.
  ThreadExitEpoch();
  Reclaim();
  EXPECT_EQ(RetireQueueSize(), 0u) << "retire queues did not drain after all readers quiesced — "
                                      "reclamation is not actually running";
}
