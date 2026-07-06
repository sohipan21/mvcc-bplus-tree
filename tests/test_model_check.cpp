#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "mvcc.h"

// ===========================================================================
// Model-based snapshot-isolation checker.
//
// Every operation on the MVCCTree is recorded together with the
// global_version values observed immediately before and after it. Because
// every committing write obtains its timestamp from a single
// global_version.fetch_add, a write's (unknown) commit timestamp is bracketed:
//
//     before < ts <= after
//
// Keys are PARTITIONED across writer threads (key % kWriters == writer id),
// so each key's write sequence is one thread's program order and its commit
// timestamps are strictly increasing. The set of writes visible to a snapshot
// `snap` is therefore always a PREFIX of that key's write sequence:
//
//     writes with after <= snap   are definitely   in the prefix
//     writes with before >= snap  are definitely NOT in the prefix
//     writes with before < snap < after are ambiguous (either side is legal)
//
// A recorded Read(key, snap) is CORRECT iff its result equals the key's model
// state after SOME prefix length inside that ambiguity window. Any result
// outside the window is a snapshot-isolation violation: the read observed a
// value that no linearization of the recorded history can explain.
//
// This is deterministic post-mortem checking (no interleaving search): the
// concurrency happened for real, the validation is exact.
// ===========================================================================

namespace {

void* V(uint64_t v) { return reinterpret_cast<void*>(v); }
uint64_t U(void* p) { return reinterpret_cast<uint64_t>(p); }

enum class WriteKind { kInsert, kUpdate, kDelete };

struct WriteOp {
  uint64_t key;
  WriteKind kind;
  void* value;      // meaningful for insert/update
  uint64_t before;  // global_version loaded immediately before the call
  uint64_t after;   // global_version loaded immediately after the call
};

struct ReadOp {
  uint64_t key;
  uint64_t snap;
  void* result;
};

// Model state of one key after applying a prefix of its write sequence.
// Mirrors MVCCTree semantics exactly:
//   Insert: upsert — sets the value whether the key is absent, live, or
//           tombstoned.
//   Update: sets the value only if the key currently has a live version;
//           otherwise a no-op.
//   Delete: tombstones the live version; no-op if absent/tombstoned.
void* ApplyWrite(void* state, const WriteOp& w) {
  switch (w.kind) {
    case WriteKind::kInsert:
      return w.value;
    case WriteKind::kUpdate:
      return state != nullptr ? w.value : state;
    case WriteKind::kDelete:
      return nullptr;
  }
  return state;
}

}  // namespace

TEST(ModelCheck, SnapshotReadsMatchSomeConsistentPrefix) {
  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  constexpr uint64_t kNumKeys = 64;  // key k is owned by writer (k % kWriters)
  constexpr int kWritesPerWriter = 2000;
  constexpr int kReadsPerReader = 5000;

  MVCCTree tree;

  std::vector<std::vector<WriteOp>> writer_logs(kWriters);
  std::vector<std::vector<ReadOp>> reader_logs(kReaders);

  std::atomic<bool> writers_done{false};

  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w]() {
      std::mt19937_64 rng(0xC0FFEE + static_cast<uint64_t>(w));
      auto& log = writer_logs[w];
      log.reserve(kWritesPerWriter);
      uint64_t seq = 0;

      for (int i = 0; i < kWritesPerWriter; ++i) {
        // Pick a key from this writer's band: key % kWriters == w.
        uint64_t key = (rng() % (kNumKeys / kWriters)) * kWriters + w + 1;
        int action = static_cast<int>(rng() % 4);
        // Unique, nonzero value per write for exact matching.
        void* value = V((key << 20) | (static_cast<uint64_t>(w) << 16) | ++seq);

        WriteOp op;
        op.key = key;
        op.value = value;
        // RAW clock loads, not BeginRead(): the window must bracket the ts
        // this op draws (before < ts <= after). BeginRead() is capped by the
        // commit watermark and can return less than our own ts.
        op.before = global_version.load(std::memory_order_acquire);
        if (action < 2) {
          op.kind = WriteKind::kInsert;
          tree.Insert(key, value);
        } else if (action == 2) {
          op.kind = WriteKind::kUpdate;
          tree.Update(key, value);
        } else {
          op.kind = WriteKind::kDelete;
          tree.Delete(key);
        }
        op.after = global_version.load(std::memory_order_acquire);
        log.push_back(op);
      }
    });
  }

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r]() {
      std::mt19937_64 rng(0xBEEF + static_cast<uint64_t>(r));
      auto& log = reader_logs[r];
      log.reserve(kReadsPerReader);

      uint64_t snap = tree.BeginRead();
      for (int i = 0; i < kReadsPerReader; ++i) {
        if (i % 16 == 0) snap = tree.BeginRead();
        uint64_t key = rng() % kNumKeys + 1;
        void* result = tree.Read(key, snap);
        log.push_back({key, snap, result});
        // Keep reading after writers finish too — quiescent snapshots must
        // also validate.
        if (writers_done.load(std::memory_order_acquire) && i > kReadsPerReader / 2) break;
      }
    });
  }

  for (auto& t : writers) t.join();
  writers_done.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();

  // -------------------------------------------------------------------------
  // Build the per-key ground truth: write sequence in commit order (= the
  // owning writer's program order) and the model state after each prefix.
  // -------------------------------------------------------------------------
  std::vector<std::vector<WriteOp>> writes_by_key(kNumKeys + 1);
  for (int w = 0; w < kWriters; ++w) {
    for (const WriteOp& op : writer_logs[w]) writes_by_key[op.key].push_back(op);
  }

  // states_by_key[k][j] = model value of key k after its first j writes.
  std::vector<std::vector<void*>> states_by_key(kNumKeys + 1);
  for (uint64_t k = 1; k <= kNumKeys; ++k) {
    const auto& ws = writes_by_key[k];
    auto& states = states_by_key[k];
    states.resize(ws.size() + 1);
    states[0] = nullptr;
    for (size_t j = 0; j < ws.size(); ++j) states[j + 1] = ApplyWrite(states[j], ws[j]);
  }

  // -------------------------------------------------------------------------
  // Validate every recorded read against the model.
  // -------------------------------------------------------------------------
  size_t total_reads = 0;
  size_t violations = 0;
  std::string first_violation;

  for (int r = 0; r < kReaders; ++r) {
    for (const ReadOp& read : reader_logs[r]) {
      ++total_reads;
      const auto& ws = writes_by_key[read.key];
      const auto& states = states_by_key[read.key];

      // Prefix bounds implied by the timestamp windows.
      size_t lo = 0;          // last write that is definitely committed at snap
      size_t hi = ws.size();  // last write that COULD be committed at snap
      for (size_t j = 0; j < ws.size(); ++j) {
        // before == after proves the op consumed NO timestamp (any fetch_add
        // inside the window would force after > before): it early-returned
        // (Update/Delete on an absent key) and changed no state, so it is
        // transparent to prefix classification.
        if (ws[j].before == ws[j].after) continue;
        if (ws[j].after <= read.snap) lo = j + 1;
        if (ws[j].before >= read.snap) {
          hi = j;
          break;
        }
      }
      ASSERT_LE(lo, hi) << "harness bug: empty prefix window";

      bool ok = false;
      for (size_t j = lo; j <= hi && !ok; ++j) ok = (states[j] == read.result);

      if (!ok) {
        ++violations;
        if (first_violation.empty()) {
          std::ostringstream oss;
          oss << "Read(key=" << read.key << ", snap=" << read.snap << ") = " << U(read.result)
              << " matches NO consistent prefix in [" << lo << ", " << hi << "] of " << ws.size()
              << " recorded writes; candidate values:";
          for (size_t j = lo; j <= hi; ++j) oss << " " << U(states[j]);
          first_violation = oss.str();
        }
      }
    }
  }

  EXPECT_EQ(violations, 0u) << "snapshot-isolation violations: " << violations << " of "
                            << total_reads << " reads\nfirst: " << first_violation;
  // Sanity: the run must have exercised a meaningful number of reads.
  EXPECT_GT(total_reads, static_cast<size_t>(kReaders) * kReadsPerReader / 4);
}
