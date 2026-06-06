#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>

#include "btree.h"

// Helper: cast integer to void* and back
static void* V(uint64_t v) { return reinterpret_cast<void*>(v); }
static uint64_t U(void* p) { return reinterpret_cast<uint64_t>(p); }

// ===========================================================================
// 1. Basic correctness (20 tests)
// ===========================================================================

TEST(Basic, SearchEmptyTreeReturnsNull) {
  BPlusTree t;
  EXPECT_EQ(t.Search(0), nullptr);
  EXPECT_EQ(t.Search(42), nullptr);
  EXPECT_EQ(t.Search(UINT64_MAX), nullptr);
}

TEST(Basic, InsertAndFindSingleKey) {
  BPlusTree t;
  t.Insert(7, V(77));
  EXPECT_EQ(U(t.Search(7)), 77u);
}

TEST(Basic, SearchMissingKeyReturnsNull) {
  BPlusTree t;
  t.Insert(5, V(50));
  EXPECT_EQ(t.Search(4), nullptr);
  EXPECT_EQ(t.Search(6), nullptr);
}

TEST(Basic, InsertDuplicateOverwritesValue) {
  BPlusTree t;
  t.Insert(1, V(10));
  t.Insert(1, V(20));
  EXPECT_EQ(U(t.Search(1)), 20u);
  EXPECT_EQ(t.Size(), 1u);  // size unchanged on overwrite
}

TEST(Basic, DeleteNonExistentKeyReturnsFalse) {
  BPlusTree t;
  EXPECT_FALSE(t.Delete(99));
}

TEST(Basic, DeleteFromEmptyTreeReturnsFalse) {
  BPlusTree t;
  EXPECT_FALSE(t.Delete(0));
}

TEST(Basic, DeleteOnlyKeyLeavesTreeEmpty) {
  BPlusTree t;
  t.Insert(42, V(1));
  EXPECT_TRUE(t.Delete(42));
  EXPECT_EQ(t.Search(42), nullptr);
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Basic, SizeTracksInserts) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 10; ++i) {
    t.Insert(i, V(i));
    EXPECT_EQ(t.Size(), i);
  }
}

TEST(Basic, SizeTracksDeletes) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 5; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 5; ++i) {
    EXPECT_TRUE(t.Delete(i));
    EXPECT_EQ(t.Size(), 5 - i);
  }
}

TEST(Basic, InsertMinKey) {
  BPlusTree t;
  t.Insert(0, V(1));
  EXPECT_EQ(U(t.Search(0)), 1u);
}

TEST(Basic, InsertMaxKey) {
  BPlusTree t;
  t.Insert(UINT64_MAX, V(99));
  EXPECT_EQ(U(t.Search(UINT64_MAX)), 99u);
}

TEST(Basic, TwoKeysInsertedAndFound) {
  BPlusTree t;
  t.Insert(10, V(100));
  t.Insert(20, V(200));
  EXPECT_EQ(U(t.Search(10)), 100u);
  EXPECT_EQ(U(t.Search(20)), 200u);
}

TEST(Basic, TwoKeysInsertReversedOrder) {
  BPlusTree t;
  t.Insert(20, V(200));
  t.Insert(10, V(100));
  EXPECT_EQ(U(t.Search(10)), 100u);
  EXPECT_EQ(U(t.Search(20)), 200u);
}

TEST(Basic, DeleteFirstKeyOfTwo) {
  BPlusTree t;
  t.Insert(1, V(1));
  t.Insert(2, V(2));
  EXPECT_TRUE(t.Delete(1));
  EXPECT_EQ(t.Search(1), nullptr);
  EXPECT_EQ(U(t.Search(2)), 2u);
}

TEST(Basic, DeleteSecondKeyOfTwo) {
  BPlusTree t;
  t.Insert(1, V(1));
  t.Insert(2, V(2));
  EXPECT_TRUE(t.Delete(2));
  EXPECT_EQ(U(t.Search(1)), 1u);
  EXPECT_EQ(t.Search(2), nullptr);
}

TEST(Basic, InsertSameKeyThreeTimes) {
  BPlusTree t;
  t.Insert(5, V(1));
  t.Insert(5, V(2));
  t.Insert(5, V(3));
  EXPECT_EQ(U(t.Search(5)), 3u);
  EXPECT_EQ(t.Size(), 1u);
}

TEST(Basic, DeleteReturnsFalseSecondTime) {
  BPlusTree t;
  t.Insert(7, V(7));
  EXPECT_TRUE(t.Delete(7));
  EXPECT_FALSE(t.Delete(7));
}

TEST(Basic, InsertAndSearchAdjacentKeys) {
  BPlusTree t;
  t.Insert(100, V(1));
  t.Insert(101, V(2));
  t.Insert(102, V(3));
  EXPECT_EQ(U(t.Search(100)), 1u);
  EXPECT_EQ(U(t.Search(101)), 2u);
  EXPECT_EQ(U(t.Search(102)), 3u);
  EXPECT_EQ(t.Search(99), nullptr);
  EXPECT_EQ(t.Search(103), nullptr);
}

TEST(Basic, ValuePointerPreserved) {
  int dummy = 42;
  BPlusTree t;
  t.Insert(1, &dummy);
  EXPECT_EQ(t.Search(1), &dummy);
}

TEST(Basic, NullValueCanBeStored) {
  BPlusTree t;
  t.Insert(3, nullptr);
  // nullptr stored explicitly — search should find the key (returns nullptr but
  // key exists) We verify via Size()
  EXPECT_EQ(t.Size(), 1u);
}

// ===========================================================================
// 2. Sequential insertion (15 tests)
// ===========================================================================

TEST(Sequential, Insert100Ascending) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 100; ++i) t.Insert(i, V(i * 10));
  for (uint64_t i = 1; i <= 100; ++i) EXPECT_EQ(U(t.Search(i)), i * 10);
  EXPECT_EQ(t.Size(), 100u);
}

TEST(Sequential, Insert100Descending) {
  BPlusTree t;
  for (uint64_t i = 100; i >= 1; --i) t.Insert(i, V(i * 10));
  for (uint64_t i = 1; i <= 100; ++i) EXPECT_EQ(U(t.Search(i)), i * 10);
  EXPECT_EQ(t.Size(), 100u);
}

TEST(Sequential, Insert1000Ascending) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 1000; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 1000; ++i) EXPECT_EQ(U(t.Search(i)), i);
  EXPECT_EQ(t.Size(), 1000u);
}

TEST(Sequential, Insert1000Descending) {
  BPlusTree t;
  for (uint64_t i = 1000; i >= 1; --i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 1000; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Sequential, OutOfRangeSearchLow) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
  EXPECT_EQ(t.Search(0), nullptr);
}

TEST(Sequential, OutOfRangeSearchHigh) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
  EXPECT_EQ(t.Search(51), nullptr);
}

TEST(Sequential, DeleteAllAscending) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 100; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 100; ++i) EXPECT_TRUE(t.Delete(i));
  EXPECT_EQ(t.Size(), 0u);
  for (uint64_t i = 1; i <= 100; ++i) EXPECT_EQ(t.Search(i), nullptr);
}

TEST(Sequential, DeleteAllDescending) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 100; ++i) t.Insert(i, V(i));
  for (uint64_t i = 100; i >= 1; --i) EXPECT_TRUE(t.Delete(i));
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Sequential, OverwriteAllValues) {
  BPlusTree t;
  for (uint64_t i = 0; i < 200; ++i) t.Insert(i, V(i));
  for (uint64_t i = 0; i < 200; ++i) t.Insert(i, V(i + 1000));
  for (uint64_t i = 0; i < 200; ++i) EXPECT_EQ(U(t.Search(i)), i + 1000);
  EXPECT_EQ(t.Size(), 200u);
}

TEST(Sequential, EvenKeys) {
  BPlusTree t;
  for (uint64_t i = 0; i < 200; i += 2) t.Insert(i, V(i));
  for (uint64_t i = 0; i < 200; i += 2) EXPECT_EQ(U(t.Search(i)), i);
  for (uint64_t i = 1; i < 200; i += 2) EXPECT_EQ(t.Search(i), nullptr);
}

TEST(Sequential, OddKeys) {
  BPlusTree t;
  for (uint64_t i = 1; i < 200; i += 2) t.Insert(i, V(i));
  for (uint64_t i = 1; i < 200; i += 2) EXPECT_EQ(U(t.Search(i)), i);
  for (uint64_t i = 0; i < 200; i += 2) EXPECT_EQ(t.Search(i), nullptr);
}

TEST(Sequential, LargeKeyRange) {
  BPlusTree t;
  const uint64_t step = 1'000'000'000ULL;
  for (uint64_t i = 0; i < 50; ++i) t.Insert(i * step, V(i));
  for (uint64_t i = 0; i < 50; ++i) EXPECT_EQ(U(t.Search(i * step)), i);
}

TEST(Sequential, Insert500Delete250) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 500; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 500; i += 2) EXPECT_TRUE(t.Delete(i));  // delete odds
  EXPECT_EQ(t.Size(), 250u);
  for (uint64_t i = 1; i <= 500; i += 2) EXPECT_EQ(t.Search(i), nullptr);
  for (uint64_t i = 2; i <= 500; i += 2) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Sequential, InsertThenDeleteMinMax) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 30; ++i) t.Insert(i, V(i));
  EXPECT_TRUE(t.Delete(1));
  EXPECT_TRUE(t.Delete(30));
  EXPECT_EQ(t.Search(1), nullptr);
  EXPECT_EQ(t.Search(30), nullptr);
  EXPECT_EQ(U(t.Search(2)), 2u);
  EXPECT_EQ(U(t.Search(29)), 29u);
}

TEST(Sequential, RepeatedInsertDelete) {
  BPlusTree t;
  for (int round = 0; round < 5; ++round) {
    for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
    for (uint64_t i = 1; i <= 50; ++i) t.Delete(i);
    EXPECT_EQ(t.Size(), 0u);
  }
}

// ===========================================================================
// 3. Random insertion (15 tests)
// ===========================================================================

TEST(Random, Insert500Shuffled) {
  std::vector<uint64_t> keys(500);
  std::iota(keys.begin(), keys.end(), 1);
  std::mt19937_64 rng(12345);
  std::shuffle(keys.begin(), keys.end(), rng);

  BPlusTree t;
  for (auto k : keys) t.Insert(k, V(k * 2));
  for (auto k : keys) EXPECT_EQ(U(t.Search(k)), k * 2) << "key=" << k;
}

TEST(Random, Insert1000Shuffled) {
  std::vector<uint64_t> keys(1000);
  std::iota(keys.begin(), keys.end(), 1);
  std::mt19937_64 rng(99999);
  std::shuffle(keys.begin(), keys.end(), rng);

  BPlusTree t;
  for (auto k : keys) t.Insert(k, V(k));
  for (auto k : keys) EXPECT_EQ(U(t.Search(k)), k);
}

TEST(Random, Delete200FromShuffled1000) {
  std::vector<uint64_t> keys(1000);
  std::iota(keys.begin(), keys.end(), 1);
  std::mt19937_64 rng(54321);
  std::shuffle(keys.begin(), keys.end(), rng);

  BPlusTree t;
  for (auto k : keys) t.Insert(k, V(k));

  std::vector<uint64_t> to_delete(keys.begin(), keys.begin() + 200);
  std::unordered_set<uint64_t> deleted(to_delete.begin(), to_delete.end());

  for (auto k : to_delete) EXPECT_TRUE(t.Delete(k));
  EXPECT_EQ(t.Size(), 800u);

  for (auto k : keys) {
    if (deleted.count(k))
      EXPECT_EQ(t.Search(k), nullptr) << "key=" << k;
    else
      EXPECT_EQ(U(t.Search(k)), k) << "key=" << k;
  }
}

TEST(Random, RandomMixedOps500) {
  std::mt19937_64 rng(11111);
  std::uniform_int_distribution<uint64_t> key_dist(1, 200);
  std::uniform_int_distribution<int> op_dist(0, 2);  // 0=insert, 1=delete, 2=search

  BPlusTree t;
  std::unordered_map<uint64_t, uint64_t> ref;

  for (int i = 0; i < 500; ++i) {
    uint64_t k = key_dist(rng);
    int op = op_dist(rng);
    if (op == 0) {
      t.Insert(k, V(k + 1));
      ref[k] = k + 1;
    } else if (op == 1) {
      bool tree_del = t.Delete(k);
      bool ref_del = ref.erase(k) > 0;
      EXPECT_EQ(tree_del, ref_del) << "key=" << k;
    } else {
      void* result = t.Search(k);
      if (ref.count(k))
        EXPECT_EQ(U(result), ref[k]) << "key=" << k;
      else
        EXPECT_EQ(result, nullptr) << "key=" << k;
    }
  }
}

TEST(Random, UniqueRandomKeys300) {
  std::mt19937_64 rng(77777);
  std::vector<uint64_t> keys;
  std::unordered_set<uint64_t> seen;
  while (keys.size() < 300) {
    uint64_t k = rng();
    if (seen.insert(k).second) keys.push_back(k);
  }

  BPlusTree t;
  for (auto k : keys) t.Insert(k, V(1));
  for (auto k : keys) EXPECT_NE(t.Search(k), nullptr) << "key=" << k;
  EXPECT_EQ(t.Size(), 300u);
}

TEST(Random, RandomKeys_DeleteHalf) {
  std::mt19937_64 rng(33333);
  std::vector<uint64_t> keys(400);
  std::iota(keys.begin(), keys.end(), 100);
  std::shuffle(keys.begin(), keys.end(), rng);

  BPlusTree t;
  for (auto k : keys) t.Insert(k, V(k));

  std::vector<uint64_t> del_half(keys.begin(), keys.begin() + 200);
  for (auto k : del_half) t.Delete(k);

  std::unordered_set<uint64_t> deleted(del_half.begin(), del_half.end());
  for (auto k : keys) {
    if (deleted.count(k))
      EXPECT_EQ(t.Search(k), nullptr);
    else
      EXPECT_EQ(U(t.Search(k)), k);
  }
}

TEST(Random, InsertRepeatKeys) {
  std::mt19937_64 rng(55555);
  std::uniform_int_distribution<uint64_t> dist(1, 20);

  BPlusTree t;
  std::unordered_map<uint64_t, uint64_t> ref;
  for (int i = 0; i < 200; ++i) {
    uint64_t k = dist(rng);
    t.Insert(k, V(k * 3));
    ref[k] = k * 3;
  }
  for (auto& [k, v] : ref) EXPECT_EQ(U(t.Search(k)), v);
}

TEST(Random, ShuffledThenDeleteAll) {
  std::vector<uint64_t> keys(300);
  std::iota(keys.begin(), keys.end(), 1);
  std::mt19937_64 rng(22222);
  std::shuffle(keys.begin(), keys.end(), rng);

  BPlusTree t;
  for (auto k : keys) t.Insert(k, V(k));
  std::shuffle(keys.begin(), keys.end(), rng);
  for (auto k : keys) EXPECT_TRUE(t.Delete(k));
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Random, SearchNonExistentAmongPresent) {
  BPlusTree t;
  for (uint64_t i = 0; i < 100; i += 3) t.Insert(i, V(i));
  for (uint64_t i = 1; i < 100; i += 3) EXPECT_EQ(t.Search(i), nullptr);
}

TEST(Random, LargeRandomInsert2000) {
  std::vector<uint64_t> keys(2000);
  std::iota(keys.begin(), keys.end(), 1);
  std::mt19937_64 rng(13579);
  std::shuffle(keys.begin(), keys.end(), rng);

  BPlusTree t;
  for (auto k : keys) t.Insert(k, V(k));
  for (auto k : keys) EXPECT_EQ(U(t.Search(k)), k);
  EXPECT_EQ(t.Size(), 2000u);
}

TEST(Random, InsertDelete_SizeConsistent) {
  std::mt19937_64 rng(86420);
  std::uniform_int_distribution<uint64_t> dist(1, 100);

  BPlusTree t;
  std::unordered_set<uint64_t> present;

  for (int i = 0; i < 300; ++i) {
    uint64_t k = dist(rng);
    if (present.count(k)) {
      t.Delete(k);
      present.erase(k);
    } else {
      t.Insert(k, V(k));
      present.insert(k);
    }
    EXPECT_EQ(t.Size(), present.size());
  }
}

TEST(Random, BulkInsertThenBulkSearch) {
  BPlusTree t;
  for (uint64_t i = 500; i >= 1; --i) t.Insert(i, V(i * 7));
  for (uint64_t i = 1; i <= 500; ++i) EXPECT_EQ(U(t.Search(i)), i * 7);
}

TEST(Random, RandomKeyValuePairs) {
  std::mt19937_64 rng(24680);
  std::map<uint64_t, uint64_t> ref;
  BPlusTree t;

  for (int i = 0; i < 300; ++i) {
    uint64_t k = rng() % 500 + 1;
    uint64_t v = rng() % 10000 + 1;
    t.Insert(k, V(v));
    ref[k] = v;
  }
  for (auto& [k, v] : ref) EXPECT_EQ(U(t.Search(k)), v);
}

TEST(Random, InterleavedInsertSearch) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 100; ++i) {
    t.Insert(i, V(i));
    for (uint64_t j = 1; j <= i; ++j)
      EXPECT_EQ(U(t.Search(j)), j) << "after insert " << i << " search " << j;
  }
}

TEST(Random, InterleavedDeleteSearch) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 50; ++i) {
    t.Delete(i);
    for (uint64_t j = 1; j <= i; ++j)
      EXPECT_EQ(t.Search(j), nullptr) << "after delete " << i << " search " << j;
    for (uint64_t j = i + 1; j <= 50; ++j) EXPECT_EQ(U(t.Search(j)), j);
  }
}

// ===========================================================================
// 4. Node split behavior (15 tests)
// ===========================================================================

TEST(Split, ForcesRootSplit) {
  BPlusTree t;
  // K=8 keys fills root; K+1 forces a split
  for (uint64_t i = 1; i <= K + 1; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= K + 1; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Split, AllSearchableAfterMultipleSplits) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 200; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 200; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Split, SplitWithDescendingInsert) {
  BPlusTree t;
  for (uint64_t i = 100; i >= 1; --i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 100; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Split, SizeCorrectAfterSplits) {
  BPlusTree t;
  for (uint64_t i = 0; i < 300; ++i) t.Insert(i, V(i));
  EXPECT_EQ(t.Size(), 300u);
}

TEST(Split, OverwriteAfterSplit) {
  BPlusTree t;
  for (uint64_t i = 0; i < 100; ++i) t.Insert(i, V(i));
  for (uint64_t i = 0; i < 100; ++i) t.Insert(i, V(i + 500));
  for (uint64_t i = 0; i < 100; ++i) EXPECT_EQ(U(t.Search(i)), i + 500);
  EXPECT_EQ(t.Size(), 100u);
}

TEST(Split, SearchBetweenSplitBoundary) {
  BPlusTree t;
  for (uint64_t i = 0; i < 50; i += 2) t.Insert(i, V(i));  // even keys only
  for (uint64_t i = 0; i < 50; i += 2) EXPECT_EQ(U(t.Search(i)), i);
  for (uint64_t i = 1; i < 50; i += 2) EXPECT_EQ(t.Search(i), nullptr);
}

TEST(Split, DeleteAfterMultipleSplits) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 150; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 150; i += 3) EXPECT_TRUE(t.Delete(i));
  for (uint64_t i = 1; i <= 150; i += 3) EXPECT_EQ(t.Search(i), nullptr);
  for (uint64_t i = 2; i <= 150; i += 3) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Split, SplitAtExactFanout) {
  BPlusTree t;
  for (uint64_t i = 0; i < K; ++i) t.Insert(i, V(i));
  EXPECT_EQ(t.Size(), K);
  for (uint64_t i = 0; i < K; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Split, SplitOnePastFanout) {
  BPlusTree t;
  for (uint64_t i = 0; i <= K; ++i) t.Insert(i, V(i));
  EXPECT_EQ(t.Size(), K + 1);
  for (uint64_t i = 0; i <= K; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Split, SplitTwice) {
  BPlusTree t;
  for (uint64_t i = 0; i < K * 3; ++i) t.Insert(i, V(i));
  for (uint64_t i = 0; i < K * 3; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Split, LargeTreeIntegrity) {
  BPlusTree t;
  for (uint64_t i = 0; i < 500; ++i) t.Insert(i, V(i * 2));
  for (uint64_t i = 0; i < 500; ++i) EXPECT_EQ(U(t.Search(i)), i * 2);
}

TEST(Split, SplitWithGappedKeys) {
  BPlusTree t;
  for (uint64_t i = 0; i < 200; i += 5) t.Insert(i, V(i));
  for (uint64_t i = 0; i < 200; i += 5) EXPECT_EQ(U(t.Search(i)), i);
  for (uint64_t i = 1; i < 200; i += 5) EXPECT_EQ(t.Search(i), nullptr);
}

TEST(Split, DeleteAllAfterManySplits) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 200; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 200; ++i) t.Delete(i);
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Split, RandomInsertThenDeleteAll) {
  std::vector<uint64_t> keys(200);
  std::iota(keys.begin(), keys.end(), 1);
  std::mt19937_64 rng(42);
  std::shuffle(keys.begin(), keys.end(), rng);

  BPlusTree t;
  for (auto k : keys) t.Insert(k, V(k));
  std::shuffle(keys.begin(), keys.end(), rng);
  for (auto k : keys) EXPECT_TRUE(t.Delete(k));
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Split, InsertAlternatingHighLow) {
  BPlusTree t;
  for (uint64_t i = 0; i < 100; ++i) {
    t.Insert(i, V(i));
    t.Insert(1000 - i, V(1000 - i));
  }
  for (uint64_t i = 0; i < 100; ++i) EXPECT_EQ(U(t.Search(i)), i);
  for (uint64_t i = 901; i <= 1000; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

// ===========================================================================
// 5. Deletion edge cases (20 tests)
// ===========================================================================

TEST(Delete, DeleteMinKeyOfMany) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
  EXPECT_TRUE(t.Delete(1));
  EXPECT_EQ(t.Search(1), nullptr);
  EXPECT_EQ(U(t.Search(2)), 2u);
}

TEST(Delete, DeleteMaxKeyOfMany) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
  EXPECT_TRUE(t.Delete(50));
  EXPECT_EQ(t.Search(50), nullptr);
  EXPECT_EQ(U(t.Search(49)), 49u);
}

TEST(Delete, DeleteMiddleKey) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
  EXPECT_TRUE(t.Delete(25));
  EXPECT_EQ(t.Search(25), nullptr);
  EXPECT_EQ(U(t.Search(24)), 24u);
  EXPECT_EQ(U(t.Search(26)), 26u);
}

TEST(Delete, DeleteCausesUnderflowBorrowLeft) {
  BPlusTree t;
  // Insert enough to have at least two leaves, delete to force borrow
  for (uint64_t i = 1; i <= 20; ++i) t.Insert(i, V(i));
  // Delete from right side to trigger borrow from left
  for (uint64_t i = 15; i <= 20; ++i) t.Delete(i);
  for (uint64_t i = 1; i <= 14; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Delete, DeleteCausesUnderflowBorrowRight) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 20; ++i) t.Insert(i, V(i));
  // Delete from left side to trigger borrow from right
  for (uint64_t i = 1; i <= 6; ++i) t.Delete(i);
  for (uint64_t i = 7; i <= 20; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Delete, DeleteCausesMerge) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 30; ++i) t.Insert(i, V(i));
  // Delete enough to force leaf merges
  for (uint64_t i = 1; i <= 20; ++i) t.Delete(i);
  EXPECT_EQ(t.Size(), 10u);
  for (uint64_t i = 21; i <= 30; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Delete, DeleteCausesRootShrink) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 20; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 20; ++i) t.Delete(i);
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Delete, DeleteAllOneByOne_LargeTree) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 200; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 200; ++i) {
    EXPECT_TRUE(t.Delete(i));
    EXPECT_EQ(t.Search(i), nullptr);
  }
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Delete, DeleteNonExistentAfterInsert) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 10; ++i) t.Insert(i, V(i));
  EXPECT_FALSE(t.Delete(0));
  EXPECT_FALSE(t.Delete(11));
  EXPECT_EQ(t.Size(), 10u);
}

TEST(Delete, AlternateDeleteInsert) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 25; ++i) {
    t.Delete(i);
    t.Insert(i + 100, V(i + 100));
  }
  for (uint64_t i = 26; i <= 50; ++i) EXPECT_EQ(U(t.Search(i)), i);
  for (uint64_t i = 101; i <= 125; ++i) EXPECT_EQ(U(t.Search(i)), i);
  for (uint64_t i = 1; i <= 25; ++i) EXPECT_EQ(t.Search(i), nullptr);
}

TEST(Delete, DeleteAtSplitBoundary) {
  BPlusTree t;
  for (uint64_t i = 0; i < K * 4; ++i) t.Insert(i, V(i));
  // Delete keys right around split boundaries
  t.Delete(K - 1);
  t.Delete(K);
  t.Delete(K + 1);
  EXPECT_EQ(t.Search(K - 1), nullptr);
  EXPECT_EQ(t.Search(K), nullptr);
  EXPECT_EQ(t.Search(K + 1), nullptr);
  EXPECT_EQ(U(t.Search(K - 2)), K - 2);
  EXPECT_EQ(U(t.Search(K + 2)), K + 2);
}

TEST(Delete, DeleteFirstInsertedKey) {
  BPlusTree t;
  t.Insert(100, V(1));
  for (uint64_t i = 1; i <= 30; ++i) t.Insert(i, V(i));
  EXPECT_TRUE(t.Delete(100));
  EXPECT_EQ(t.Search(100), nullptr);
  for (uint64_t i = 1; i <= 30; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Delete, DeleteLastInsertedKey) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 30; ++i) t.Insert(i, V(i));
  t.Insert(100, V(100));
  EXPECT_TRUE(t.Delete(100));
  EXPECT_EQ(t.Search(100), nullptr);
  for (uint64_t i = 1; i <= 30; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Delete, ReinsertDeletedKey) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 30; ++i) t.Insert(i, V(i));
  t.Delete(15);
  t.Insert(15, V(999));
  EXPECT_EQ(U(t.Search(15)), 999u);
}

TEST(Delete, DeleteSameKeyTwice) {
  BPlusTree t;
  t.Insert(5, V(5));
  EXPECT_TRUE(t.Delete(5));
  EXPECT_FALSE(t.Delete(5));
}

TEST(Delete, BulkDeleteThenInsertAgain) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 50; ++i) t.Delete(i);
  for (uint64_t i = 51; i <= 100; ++i) t.Insert(i, V(i));
  for (uint64_t i = 51; i <= 100; ++i) EXPECT_EQ(U(t.Search(i)), i);
  for (uint64_t i = 1; i <= 50; ++i) EXPECT_EQ(t.Search(i), nullptr);
}

TEST(Delete, DeleteMaintainsNeighbors) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 30; ++i) t.Insert(i, V(i));
  t.Delete(15);
  EXPECT_EQ(U(t.Search(14)), 14u);
  EXPECT_EQ(U(t.Search(16)), 16u);
}

TEST(Delete, MassDelete_Half) {
  BPlusTree t;
  for (uint64_t i = 0; i < 300; ++i) t.Insert(i, V(i));
  for (uint64_t i = 0; i < 300; i += 2) t.Delete(i);
  for (uint64_t i = 0; i < 300; i += 2) EXPECT_EQ(t.Search(i), nullptr);
  for (uint64_t i = 1; i < 300; i += 2) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Delete, DeleteInReverseOrder) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 100; ++i) t.Insert(i, V(i));
  for (uint64_t i = 100; i >= 1; --i) EXPECT_TRUE(t.Delete(i));
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Delete, DeleteOnlyKeyOfSplitTree) {
  BPlusTree t;
  // Insert enough to split, then delete all but one
  for (uint64_t i = 0; i < K * 2 + 1; ++i) t.Insert(i, V(i));
  for (uint64_t i = 0; i < K * 2; ++i) t.Delete(i);
  EXPECT_EQ(t.Size(), 1u);
  EXPECT_EQ(U(t.Search(K * 2)), K * 2);
}

// ===========================================================================
// 6. Range / boundary (10 tests)
// ===========================================================================

TEST(Boundary, SearchZeroNotInserted) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 1000; ++i) t.Insert(i, V(i));
  EXPECT_EQ(t.Search(0), nullptr);
}

TEST(Boundary, Search1001NotInserted) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 1000; ++i) t.Insert(i, V(i));
  EXPECT_EQ(t.Search(1001), nullptr);
}

TEST(Boundary, Insert_Delete_Reinsert_SameKey) {
  BPlusTree t;
  for (int round = 0; round < 10; ++round) {
    t.Insert(42, V(round));
    EXPECT_EQ(U(t.Search(42)), static_cast<uint64_t>(round));
    t.Delete(42);
    EXPECT_EQ(t.Search(42), nullptr);
  }
}

TEST(Boundary, MaxUInt64Key) {
  BPlusTree t;
  t.Insert(UINT64_MAX, V(1));
  EXPECT_EQ(U(t.Search(UINT64_MAX)), 1u);
  EXPECT_TRUE(t.Delete(UINT64_MAX));
  EXPECT_EQ(t.Search(UINT64_MAX), nullptr);
}

TEST(Boundary, ZeroKey) {
  BPlusTree t;
  t.Insert(0, V(7));
  EXPECT_EQ(U(t.Search(0)), 7u);
  EXPECT_TRUE(t.Delete(0));
  EXPECT_EQ(t.Search(0), nullptr);
}

TEST(Boundary, ZeroAndMaxKey) {
  BPlusTree t;
  t.Insert(0, V(1));
  t.Insert(UINT64_MAX, V(2));
  EXPECT_EQ(U(t.Search(0)), 1u);
  EXPECT_EQ(U(t.Search(UINT64_MAX)), 2u);
  EXPECT_EQ(t.Search(1), nullptr);
  EXPECT_EQ(t.Search(UINT64_MAX - 1), nullptr);
}

TEST(Boundary, ExactlyKKeys) {
  BPlusTree t;
  for (uint64_t i = 0; i < K; ++i) t.Insert(i, V(i));
  EXPECT_EQ(t.Size(), K);
  for (uint64_t i = 0; i < K; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Boundary, ExactlyK1Keys) {
  BPlusTree t;
  for (uint64_t i = 0; i < K + 1; ++i) t.Insert(i, V(i));
  EXPECT_EQ(t.Size(), K + 1);
  for (uint64_t i = 0; i < K + 1; ++i) EXPECT_EQ(U(t.Search(i)), i);
}

TEST(Boundary, SearchEmptyAfterDeleteAll) {
  BPlusTree t;
  for (uint64_t i = 0; i < 50; ++i) t.Insert(i, V(i));
  for (uint64_t i = 0; i < 50; ++i) t.Delete(i);
  for (uint64_t i = 0; i < 50; ++i) EXPECT_EQ(t.Search(i), nullptr);
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Boundary, NonContiguousLargeKeys) {
  BPlusTree t;
  const uint64_t gap = UINT64_MAX / 100;
  for (uint64_t i = 0; i < 50; ++i) t.Insert(i * gap, V(i));
  for (uint64_t i = 0; i < 50; ++i) EXPECT_EQ(U(t.Search(i * gap)), i);
  for (uint64_t i = 0; i < 50; ++i) EXPECT_EQ(t.Search(i * gap + 1), nullptr);
}

// ===========================================================================
// 7. Stress tests (5 tests)
// ===========================================================================

TEST(Stress, Insert10000SearchAll) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 10000; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 10000; ++i) EXPECT_EQ(U(t.Search(i)), i);
  EXPECT_EQ(t.Size(), 10000u);
}

TEST(Stress, Insert10000DeleteAll) {
  BPlusTree t;
  for (uint64_t i = 1; i <= 10000; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 10000; ++i) EXPECT_TRUE(t.Delete(i));
  EXPECT_EQ(t.Size(), 0u);
}

TEST(Stress, RandomMix5000Ops) {
  std::mt19937_64 rng(99887766);
  std::uniform_int_distribution<uint64_t> key_dist(1, 500);
  std::uniform_int_distribution<int> op_dist(0, 1);

  BPlusTree t;
  std::unordered_map<uint64_t, uint64_t> ref;

  for (int i = 0; i < 5000; ++i) {
    uint64_t k = key_dist(rng);
    if (op_dist(rng) == 0) {
      t.Insert(k, V(k));
      ref[k] = k;
    } else {
      bool tr = t.Delete(k);
      bool rr = ref.erase(k) > 0;
      EXPECT_EQ(tr, rr);
    }
  }
  for (auto& [k, v] : ref) EXPECT_EQ(U(t.Search(k)), v);
  EXPECT_EQ(t.Size(), ref.size());
}

TEST(Stress, AlternatingInsertDelete1000) {
  BPlusTree t;
  for (int round = 0; round < 10; ++round) {
    for (uint64_t i = 1; i <= 100; ++i) t.Insert(i, V(i + round));
    for (uint64_t i = 1; i <= 100; ++i) {
      EXPECT_EQ(U(t.Search(i)), i + round);
    }
    for (uint64_t i = 1; i <= 100; ++i) t.Delete(i);
    EXPECT_EQ(t.Size(), 0u);
  }
}

TEST(Stress, LargeShuffledInsertDeleteConsistency) {
  std::vector<uint64_t> keys(3000);
  std::iota(keys.begin(), keys.end(), 1);
  std::mt19937_64 rng(12345678);
  std::shuffle(keys.begin(), keys.end(), rng);

  BPlusTree t;
  for (auto k : keys) t.Insert(k, V(k * 2));
  for (auto k : keys) EXPECT_EQ(U(t.Search(k)), k * 2);

  std::shuffle(keys.begin(), keys.end(), rng);
  for (auto k : keys) EXPECT_TRUE(t.Delete(k));
  EXPECT_EQ(t.Size(), 0u);
}

// ===========================================================================
// Range scan tests
// ===========================================================================

TEST(Scan, FullRangeMatchesSortedInsertOrder) {
  BPlusTree t;
  for (uint64_t k = 1; k <= 50; ++k) t.Insert(k, V(k * 10));

  std::vector<uint64_t> got;
  t.Scan(1, 50, [&](uint64_t k, void*) { got.push_back(k); });

  ASSERT_EQ(got.size(), 50u);
  for (uint64_t i = 0; i < 50; ++i) EXPECT_EQ(got[i], i + 1);
}

TEST(Scan, BoundedRangeReturnsSubset) {
  BPlusTree t;
  for (uint64_t k = 1; k <= 100; ++k) t.Insert(k, V(k));

  std::map<uint64_t, uint64_t> ref;
  for (uint64_t k = 20; k <= 60; ++k) ref[k] = k;

  std::map<uint64_t, uint64_t> got;
  t.Scan(20, 60, [&](uint64_t k, void* v) { got[k] = U(v); });

  EXPECT_EQ(got, ref);
}

TEST(Scan, EmptyRangeProducesNoResults) {
  BPlusTree t;
  for (uint64_t k = 1; k <= 20; ++k) t.Insert(k, V(k));

  int count = 0;
  t.Scan(50, 100, [&](uint64_t, void*) { ++count; });
  EXPECT_EQ(count, 0);
}

TEST(Scan, AfterDeleteSkipsRemovedKeys) {
  BPlusTree t;
  for (uint64_t k = 1; k <= 20; ++k) t.Insert(k, V(k));
  for (uint64_t k = 1; k <= 20; k += 2) t.Delete(k);  // remove odd keys

  std::vector<uint64_t> got;
  t.Scan(1, 20, [&](uint64_t k, void*) { got.push_back(k); });

  for (uint64_t k : got) EXPECT_EQ(k % 2, 0u) << "odd key " << k << " survived delete";
}
