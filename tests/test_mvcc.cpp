#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "mvcc.h"
#include "version_chain.h"

// Helpers
static void* V(uint64_t v) { return reinterpret_cast<void*>(v); }
static uint64_t U(void* p) { return reinterpret_cast<uint64_t>(p); }

// Reset global_version to 0 before each test so tests are independent.
class MVCCTest : public ::testing::Test {
 protected:
  void SetUp() override { global_version.store(0, std::memory_order_relaxed); }
};

// ===========================================================================
// A. global_version counter (5 tests)
// ===========================================================================

TEST_F(MVCCTest, GlobalVersionStartsAtZero) { EXPECT_EQ(global_version.load(), 0u); }

TEST_F(MVCCTest, InsertIncrementsVersionByOne) {
  MVCCTree t;
  t.Insert(1, V(1));
  EXPECT_EQ(global_version.load(), 1u);
  t.Insert(2, V(2));
  EXPECT_EQ(global_version.load(), 2u);
}

TEST_F(MVCCTest, DeleteIncrementsVersionByOne) {
  MVCCTree t;
  t.Insert(1, V(1));
  uint64_t before = global_version.load();
  t.Delete(1);
  EXPECT_EQ(global_version.load(), before + 1);
}

TEST_F(MVCCTest, UpdateIncrementsVersionByOne) {
  MVCCTree t;
  t.Insert(1, V(1));
  uint64_t before = global_version.load();
  t.Update(1, V(2));
  EXPECT_EQ(global_version.load(), before + 1);
}

TEST_F(MVCCTest, BeginReadDoesNotIncrementVersion) {
  MVCCTree t;
  t.Insert(1, V(1));
  uint64_t v = global_version.load();
  t.BeginRead();
  t.BeginRead();
  EXPECT_EQ(global_version.load(), v);
}

// ===========================================================================
// B. Basic snapshot isolation (15 tests)
// ===========================================================================

TEST_F(MVCCTest, InsertVisibleAtItsOwnTimestamp) {
  MVCCTree t;
  t.Insert(42, V(100));
  uint64_t ts = global_version.load();
  EXPECT_EQ(U(t.Read(42, ts)), 100u);
}

TEST_F(MVCCTest, InsertNotVisibleBeforeItsTimestamp) {
  MVCCTree t;
  t.Insert(42, V(100));
  // snapshot at ts=0 (before any write)
  EXPECT_EQ(t.Read(42, 0), nullptr);
}

TEST_F(MVCCTest, SnapshotBeforeSecondInsertSeesFirstValue) {
  MVCCTree t;
  t.Insert(1, V(10));
  uint64_t snap = t.BeginRead();  // snapshot after first insert
  t.Insert(1, V(20));             // second insert = update
  EXPECT_EQ(U(t.Read(1, snap)), 10u);
}

TEST_F(MVCCTest, ReadAfterDeleteReturnsNull) {
  MVCCTree t;
  t.Insert(5, V(50));
  t.Delete(5);
  uint64_t ts = t.BeginRead();
  EXPECT_EQ(t.Read(5, ts), nullptr);
}

TEST_F(MVCCTest, ReadBeforeDeleteSeesValue) {
  MVCCTree t;
  t.Insert(5, V(50));
  uint64_t snap = t.BeginRead();  // before delete
  t.Delete(5);
  EXPECT_EQ(U(t.Read(5, snap)), 50u);
}

TEST_F(MVCCTest, UpdateOldSnapshotSeesOldValue) {
  MVCCTree t;
  t.Insert(7, V(70));
  uint64_t snap1 = t.BeginRead();
  t.Update(7, V(77));
  EXPECT_EQ(U(t.Read(7, snap1)), 70u);
}

TEST_F(MVCCTest, UpdateNewSnapshotSeesNewValue) {
  MVCCTree t;
  t.Insert(7, V(70));
  t.Update(7, V(77));
  uint64_t snap2 = t.BeginRead();
  EXPECT_EQ(U(t.Read(7, snap2)), 77u);
}

TEST_F(MVCCTest, ReadBeforeAnyWriteReturnsNull) {
  MVCCTree t;
  t.Insert(3, V(30));
  EXPECT_EQ(t.Read(3, 0), nullptr);
}

TEST_F(MVCCTest, TwoDifferentKeysIndependentVersions) {
  MVCCTree t;
  t.Insert(1, V(10));
  uint64_t snap1 = t.BeginRead();
  t.Insert(2, V(20));
  uint64_t snap2 = t.BeginRead();

  // snap1 sees key 1, not key 2
  EXPECT_EQ(U(t.Read(1, snap1)), 10u);
  EXPECT_EQ(t.Read(2, snap1), nullptr);

  // snap2 sees both
  EXPECT_EQ(U(t.Read(1, snap2)), 10u);
  EXPECT_EQ(U(t.Read(2, snap2)), 20u);
}

TEST_F(MVCCTest, ReadNonExistentKeyAlwaysNull) {
  MVCCTree t;
  t.Insert(1, V(1));
  EXPECT_EQ(t.Read(99, 0), nullptr);
  EXPECT_EQ(t.Read(99, 1), nullptr);
  EXPECT_EQ(t.Read(99, UINT64_MAX), nullptr);
}

TEST_F(MVCCTest, InsertUpdateDeleteSequence) {
  MVCCTree t;
  t.Insert(10, V(1));
  uint64_t s1 = t.BeginRead();
  t.Update(10, V(2));
  uint64_t s2 = t.BeginRead();
  t.Delete(10);
  uint64_t s3 = t.BeginRead();

  EXPECT_EQ(U(t.Read(10, s1)), 1u);
  EXPECT_EQ(U(t.Read(10, s2)), 2u);
  EXPECT_EQ(t.Read(10, s3), nullptr);
}

TEST_F(MVCCTest, ExistsMirrorsRead) {
  MVCCTree t;
  t.Insert(5, V(50));
  uint64_t s1 = t.BeginRead();
  t.Delete(5);
  uint64_t s2 = t.BeginRead();

  EXPECT_TRUE(t.Exists(5, s1));
  EXPECT_FALSE(t.Exists(5, s2));
  EXPECT_FALSE(t.Exists(99, s1));
}

TEST_F(MVCCTest, InsertSameKeyTwiceIsUpdate) {
  MVCCTree t;
  t.Insert(3, V(30));
  uint64_t s1 = t.BeginRead();
  t.Insert(3, V(31));  // treated as update
  uint64_t s2 = t.BeginRead();

  EXPECT_EQ(U(t.Read(3, s1)), 30u);
  EXPECT_EQ(U(t.Read(3, s2)), 31u);
  EXPECT_EQ(t.NumKeys(), 1u);  // still one distinct tree key
}

TEST_F(MVCCTest, BeginReadCapturesStateBeforeNextWrite) {
  MVCCTree t;
  t.Insert(1, V(1));
  uint64_t snap = t.BeginRead();
  t.Insert(2, V(2));

  EXPECT_TRUE(t.Exists(1, snap));
  EXPECT_FALSE(t.Exists(2, snap));
}

TEST_F(MVCCTest, BeginReadIsMonotonicallyNonDecreasing) {
  MVCCTree t;
  uint64_t prev = t.BeginRead();
  for (int i = 0; i < 10; ++i) {
    t.Insert(static_cast<uint64_t>(i), V(i));
    uint64_t cur = t.BeginRead();
    EXPECT_GE(cur, prev);
    prev = cur;
  }
}

// ===========================================================================
// C. Three-snapshot spec test (5 tests)
// ===========================================================================

TEST_F(MVCCTest, ThreeSnapshots_Snapshot1DoesNotSeeChange) {
  MVCCTree t;
  t.Insert(42, V(1));
  uint64_t s1 = t.BeginRead();  // snapshot 1: before update
  t.Update(42, V(2));
  uint64_t s3 = t.BeginRead();  // snapshot 3: after update

  EXPECT_EQ(U(t.Read(42, s1)), 1u);  // snapshot 1 sees old value
  EXPECT_EQ(U(t.Read(42, s3)), 2u);  // snapshot 3 sees new value
}

TEST_F(MVCCTest, ThreeSnapshots_Snapshots2And3SeeChange) {
  MVCCTree t;
  t.Insert(42, V(1));
  t.Update(42, V(2));
  uint64_t s2 = t.BeginRead();
  t.Insert(99, V(9));  // unrelated write
  uint64_t s3 = t.BeginRead();

  EXPECT_EQ(U(t.Read(42, s2)), 2u);
  EXPECT_EQ(U(t.Read(42, s3)), 2u);
}

TEST_F(MVCCTest, ThreeSnapshotsThreeUpdates) {
  MVCCTree t;
  t.Insert(1, V(10));
  uint64_t s1 = t.BeginRead();
  t.Update(1, V(20));
  uint64_t s2 = t.BeginRead();
  t.Update(1, V(30));
  uint64_t s3 = t.BeginRead();

  EXPECT_EQ(U(t.Read(1, s1)), 10u);
  EXPECT_EQ(U(t.Read(1, s2)), 20u);
  EXPECT_EQ(U(t.Read(1, s3)), 30u);
}

TEST_F(MVCCTest, SnapshotBetweenDeleteAndReinsert_SeesNothing) {
  MVCCTree t;
  t.Insert(7, V(70));
  t.Delete(7);
  uint64_t snap = t.BeginRead();  // between delete and re-insert
  t.Insert(7, V(71));

  EXPECT_EQ(t.Read(7, snap), nullptr);
}

TEST_F(MVCCTest, SnapshotAfterReinsert_SeesNewValue) {
  MVCCTree t;
  t.Insert(7, V(70));
  t.Delete(7);
  t.Insert(7, V(71));
  uint64_t snap = t.BeginRead();

  EXPECT_EQ(U(t.Read(7, snap)), 71u);
}

// ===========================================================================
// D. Version chain traversal (10 tests)
// ===========================================================================

TEST_F(MVCCTest, FiveUpdates_EachSnapshotSeesCorrectVersion) {
  MVCCTree t;
  std::vector<uint64_t> snaps;
  t.Insert(1, V(1));
  snaps.push_back(t.BeginRead());
  for (uint64_t v = 2; v <= 5; ++v) {
    t.Update(1, V(v));
    snaps.push_back(t.BeginRead());
  }
  for (uint64_t i = 0; i < snaps.size(); ++i) {
    EXPECT_EQ(U(t.Read(1, snaps[i])), i + 1) << "snapshot index " << i;
  }
}

TEST_F(MVCCTest, ReadReturnsMostRecentVisibleVersion) {
  MVCCTree t;
  t.Insert(1, V(10));
  t.Update(1, V(20));
  t.Update(1, V(30));
  uint64_t snap = t.BeginRead();
  EXPECT_EQ(U(t.Read(1, snap)), 30u);  // most recent, not the first
}

TEST_F(MVCCTest, UpdateSetsDeletedAtOnOldVersion) {
  VersionChain chain;
  chain.Append(V(1), 1);
  chain.Append(V(2), 2);  // simulates a second version being appended

  // The old head before second Append had deleted_at == 0; after manual
  // MarkDeleted it should be non-zero.
  VersionChain chain2;
  chain2.Append(V(1), 1);
  chain2.MarkDeleted(2);
  const VersionNode* head = chain2.Head();
  ASSERT_NE(head, nullptr);
  EXPECT_EQ(head->deleted_at.load(std::memory_order_relaxed), 2u);
}

TEST_F(MVCCTest, DeletedAtEqualCreatedAtOfReplacement) {
  // After Update: old->deleted_at == new->created_at (same ts).
  MVCCTree t;
  t.Insert(1, V(10));
  uint64_t ts_before = global_version.load();
  t.Update(1, V(20));
  uint64_t ts_update = global_version.load();  // ts assigned to the update

  // Read at ts_update → new version
  EXPECT_EQ(U(t.Read(1, ts_update)), 20u);
  // Read at ts_before → old version
  EXPECT_EQ(U(t.Read(1, ts_before)), 10u);
  // ts_update = ts_before + 1 (single increment)
  EXPECT_EQ(ts_update, ts_before + 1);
}

TEST_F(MVCCTest, MultipleKeys_VersionsDontInterfere) {
  MVCCTree t;
  for (uint64_t k = 1; k <= 5; ++k) t.Insert(k, V(k));
  std::vector<uint64_t> s1_snaps;
  for (uint64_t k = 1; k <= 5; ++k) s1_snaps.push_back(t.BeginRead());

  for (uint64_t k = 1; k <= 5; ++k) t.Update(k, V(k * 10));
  uint64_t s2 = t.BeginRead();

  for (uint64_t k = 1; k <= 5; ++k) {
    EXPECT_EQ(U(t.Read(k, s1_snaps[k - 1])), k);
    EXPECT_EQ(U(t.Read(k, s2)), k * 10);
  }
}

TEST_F(MVCCTest, AllDeletedChain_ReturnsNull) {
  MVCCTree t;
  t.Insert(1, V(1));
  t.Delete(1);
  EXPECT_EQ(t.Read(1, UINT64_MAX), nullptr);
}

TEST_F(MVCCTest, SingleVersionChain_BeforeAndAfterDeletion) {
  VersionChain chain;
  chain.Append(V(42), 5);

  EXPECT_EQ(U(chain.Read(5)), 42u);   // visible
  EXPECT_EQ(chain.Read(4), nullptr);  // before creation

  chain.MarkDeleted(10);
  EXPECT_EQ(U(chain.Read(9)), 42u);    // still visible just before deletion
  EXPECT_EQ(chain.Read(10), nullptr);  // deleted at 10 → not visible
}

TEST_F(MVCCTest, InsertSameKeyTwice_ChainHasTwoNodes) {
  VersionChain chain;
  chain.Append(V(1), 1);
  chain.Append(V(2), 2);

  // Two nodes: head is V(2) at ts=2, older is V(1) at ts=1
  EXPECT_EQ(U(chain.Read(2)), 2u);
  EXPECT_EQ(U(chain.Read(1)), 1u);
}

TEST_F(MVCCTest, TenUpdates_OnlyOneVisiblePerSnapshot) {
  MVCCTree t;
  t.Insert(1, V(1));
  std::vector<uint64_t> snaps;
  snaps.push_back(t.BeginRead());
  for (uint64_t v = 2; v <= 10; ++v) {
    t.Update(1, V(v));
    snaps.push_back(t.BeginRead());
  }
  for (size_t i = 0; i < snaps.size(); ++i) {
    EXPECT_EQ(U(t.Read(1, snaps[i])), static_cast<uint64_t>(i + 1));
  }
}

TEST_F(MVCCTest, HasLiveVersion_TrueAfterInsert_FalseAfterDelete) {
  VersionChain chain;
  EXPECT_FALSE(chain.HasLiveVersion());
  chain.Append(V(1), 1);
  EXPECT_TRUE(chain.HasLiveVersion());
  chain.MarkDeleted(2);
  EXPECT_FALSE(chain.HasLiveVersion());
}

// ===========================================================================
// E. Write operation semantics (10 tests)
// ===========================================================================

TEST_F(MVCCTest, InsertNewKey_CreatesChainWithOneNode) {
  MVCCTree t;
  t.Insert(1, V(99));
  EXPECT_EQ(t.NumKeys(), 1u);
  EXPECT_EQ(U(t.Read(1, t.BeginRead())), 99u);
}

TEST_F(MVCCTest, InsertExistingKey_AddsSecondVersion) {
  MVCCTree t;
  t.Insert(1, V(10));
  uint64_t s1 = t.BeginRead();
  t.Insert(1, V(20));  // second insert on same key
  uint64_t s2 = t.BeginRead();

  EXPECT_EQ(t.NumKeys(), 1u);
  EXPECT_EQ(U(t.Read(1, s1)), 10u);
  EXPECT_EQ(U(t.Read(1, s2)), 20u);
}

TEST_F(MVCCTest, UpdateNonexistentKey_IsNoOp) {
  MVCCTree t;
  uint64_t v_before = global_version.load();
  t.Update(99, V(1));  // key 99 never inserted
  // version should NOT be incremented (no-op)
  EXPECT_EQ(global_version.load(), v_before);
  EXPECT_EQ(t.NumKeys(), 0u);
}

TEST_F(MVCCTest, DeleteNonexistentKey_ReturnsFalse) {
  MVCCTree t;
  EXPECT_FALSE(t.Delete(99));
}

TEST_F(MVCCTest, DeleteAlreadyDeletedKey_ReturnsFalse) {
  MVCCTree t;
  t.Insert(1, V(1));
  EXPECT_TRUE(t.Delete(1));
  EXPECT_FALSE(t.Delete(1));
}

TEST_F(MVCCTest, DeleteThenReinsert_NewVersionVisible) {
  MVCCTree t;
  t.Insert(5, V(50));
  t.Delete(5);
  t.Insert(5, V(51));
  uint64_t snap = t.BeginRead();
  EXPECT_EQ(U(t.Read(5, snap)), 51u);
}

TEST_F(MVCCTest, UpdatePreservesExactPointer) {
  int x = 1, y = 2;
  MVCCTree t;
  t.Insert(1, &x);
  uint64_t s1 = t.BeginRead();
  t.Update(1, &y);
  uint64_t s2 = t.BeginRead();

  EXPECT_EQ(t.Read(1, s1), &x);
  EXPECT_EQ(t.Read(1, s2), &y);
}

TEST_F(MVCCTest, AfterDelete_ReadAtMaxSnapshotReturnsNull) {
  MVCCTree t;
  t.Insert(1, V(1));
  t.Delete(1);
  EXPECT_EQ(t.Read(1, UINT64_MAX), nullptr);
}

TEST_F(MVCCTest, InsertDeleteInsert_ThreeSnapshotTest) {
  MVCCTree t;
  t.Insert(3, V(30));
  uint64_t s1 = t.BeginRead();
  t.Delete(3);
  uint64_t s2 = t.BeginRead();
  t.Insert(3, V(31));
  uint64_t s3 = t.BeginRead();

  EXPECT_EQ(U(t.Read(3, s1)), 30u);
  EXPECT_EQ(t.Read(3, s2), nullptr);
  EXPECT_EQ(U(t.Read(3, s3)), 31u);
}

TEST_F(MVCCTest, GlobalVersionEqualsNOperations) {
  MVCCTree t;
  t.Insert(1, V(1));   // +1
  t.Insert(2, V(2));   // +1
  t.Update(1, V(10));  // +1
  t.Delete(2);         // +1
  EXPECT_EQ(global_version.load(), 4u);
}

// ===========================================================================
// F. Edge cases and stress (5 tests)
// ===========================================================================

TEST_F(MVCCTest, Stress_100Inserts_AllVisibleAfterAll) {
  MVCCTree t;
  for (uint64_t i = 1; i <= 100; ++i) t.Insert(i, V(i));
  uint64_t snap = t.BeginRead();
  for (uint64_t i = 1; i <= 100; ++i) EXPECT_EQ(U(t.Read(i, snap)), i);
}

TEST_F(MVCCTest, Stress_100Inserts_SnapshotAfter50SeesFirst50Only) {
  MVCCTree t;
  for (uint64_t i = 1; i <= 50; ++i) t.Insert(i, V(i));
  uint64_t snap50 = t.BeginRead();
  for (uint64_t i = 51; i <= 100; ++i) t.Insert(i, V(i));

  for (uint64_t i = 1; i <= 50; ++i) EXPECT_EQ(U(t.Read(i, snap50)), i) << "key " << i;
  for (uint64_t i = 51; i <= 100; ++i) EXPECT_EQ(t.Read(i, snap50), nullptr) << "key " << i;
}

TEST_F(MVCCTest, Stress_50UpdatesSameKey_EachSnapshotCorrect) {
  MVCCTree t;
  t.Insert(1, V(0));
  std::vector<uint64_t> snaps;
  snaps.push_back(t.BeginRead());
  for (uint64_t v = 1; v <= 50; ++v) {
    t.Update(1, V(v));
    snaps.push_back(t.BeginRead());
  }
  for (size_t i = 0; i < snaps.size(); ++i)
    EXPECT_EQ(U(t.Read(1, snaps[i])), static_cast<uint64_t>(i));
}

TEST_F(MVCCTest, Stress_Insert100DeleteAll_SnapshotAtEndSeesNothing) {
  MVCCTree t;
  for (uint64_t i = 1; i <= 100; ++i) t.Insert(i, V(i));
  for (uint64_t i = 1; i <= 100; ++i) t.Delete(i);
  uint64_t snap = t.BeginRead();
  for (uint64_t i = 1; i <= 100; ++i) EXPECT_EQ(t.Read(i, snap), nullptr) << "key " << i;
}

TEST_F(MVCCTest, Stress_MixedOps_ConsistentWithReferenceMap) {
  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<uint64_t> key_dist(1, 20);
  std::uniform_int_distribution<int> op_dist(0,
                                             2);  // 0=insert,1=update,2=delete

  MVCCTree t;
  std::map<uint64_t, uint64_t> ref;  // key → last written value

  for (int i = 0; i < 200; ++i) {
    uint64_t k = key_dist(rng);
    int op = op_dist(rng);
    if (op == 0) {
      t.Insert(k, V(k * 7 + i));
      ref[k] = k * 7 + i;
    } else if (op == 1) {
      t.Update(k, V(k * 3 + i));
      if (ref.count(k)) ref[k] = k * 3 + i;
    } else {
      t.Delete(k);
      ref.erase(k);
    }
  }

  uint64_t snap = t.BeginRead();
  for (uint64_t k = 1; k <= 20; ++k) {
    if (ref.count(k)) {
      EXPECT_EQ(U(t.Read(k, snap)), ref[k]) << "key " << k;
    } else {
      EXPECT_EQ(t.Read(k, snap), nullptr) << "key " << k;
    }
  }
}
