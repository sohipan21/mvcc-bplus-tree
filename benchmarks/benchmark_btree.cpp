#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <vector>

#include "epoch_reclamation.h"
#include "mvcc.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint64_t kKeySpace = 10000;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void* V(uint64_t v) { return reinterpret_cast<void*>(v); }

// ---------------------------------------------------------------------------
// BM_MVCCMixed — 80% reads / 20% writes on pre-warmed MVCCTree.
//
// Shared tree is created once (first thread wins the once_flag) and reused
// across all threads in a given run. Each thread has its own PRNG seeded by
// thread index for deterministic but non-overlapping key access patterns.
// ---------------------------------------------------------------------------

namespace {
MVCCTree* g_mvcc_tree = nullptr;
std::once_flag g_mvcc_flag;
}  // namespace

static void BM_MVCCMixed(benchmark::State& state) {
  std::call_once(g_mvcc_flag, []() {
    g_mvcc_tree = new MVCCTree();
    for (uint64_t k = 1; k <= kKeySpace; ++k) g_mvcc_tree->Insert(k, V(k * 10));
  });

  // Per-thread PRNG — deterministic across runs for reproducibility.
  std::mt19937_64 rng(static_cast<uint64_t>(state.thread_index()) * 6364136223846793005ULL + 1);
  std::uniform_int_distribution<uint64_t> key_dist(1, kKeySpace);
  std::bernoulli_distribution is_read(0.80);

  uint64_t snap = g_mvcc_tree->BeginRead();

  for (auto _ : state) {
    uint64_t key = key_dist(rng);
    if (is_read(rng)) {
      benchmark::DoNotOptimize(g_mvcc_tree->Read(key, snap));
    } else {
      g_mvcc_tree->Update(key, V(key));
      snap = g_mvcc_tree->BeginRead();
    }
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_MVCCMixed)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->UseRealTime()
    ->MinTime(10.0);

// ---------------------------------------------------------------------------
// BM_MVCCReadOnly — pure lock-free reads (upper bound, no writer contention).
// ---------------------------------------------------------------------------

static void BM_MVCCReadOnly(benchmark::State& state) {
  // Reuse the same shared tree from BM_MVCCMixed.
  std::call_once(g_mvcc_flag, []() {
    g_mvcc_tree = new MVCCTree();
    for (uint64_t k = 1; k <= kKeySpace; ++k) g_mvcc_tree->Insert(k, V(k * 10));
  });

  std::mt19937_64 rng(static_cast<uint64_t>(state.thread_index()) * 6364136223846793005ULL + 1);
  std::uniform_int_distribution<uint64_t> key_dist(1, kKeySpace);

  uint64_t snap = g_mvcc_tree->BeginRead();

  for (auto _ : state) {
    benchmark::DoNotOptimize(g_mvcc_tree->Read(key_dist(rng), snap));
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_MVCCReadOnly)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->UseRealTime()
    ->MinTime(5.0);

// ---------------------------------------------------------------------------
// BM_MVCCReadLatency — P50/P99/P999 read latency via sampled timestamps.
//
// Samples every 64th read to reduce measurement overhead.
// ---------------------------------------------------------------------------

static void BM_MVCCReadLatency(benchmark::State& state) {
  std::call_once(g_mvcc_flag, []() {
    g_mvcc_tree = new MVCCTree();
    for (uint64_t k = 1; k <= kKeySpace; ++k) g_mvcc_tree->Insert(k, V(k * 10));
  });

  std::mt19937_64 rng(static_cast<uint64_t>(state.thread_index()) * 6364136223846793005ULL + 1);
  std::uniform_int_distribution<uint64_t> key_dist(1, kKeySpace);

  uint64_t snap = g_mvcc_tree->BeginRead();

  // Collect sampled latencies (every 64th iteration).
  std::vector<int64_t> latencies;
  latencies.reserve(1 << 16);
  uint64_t iter_count = 0;

  for (auto _ : state) {
    uint64_t key = key_dist(rng);
    if ((iter_count & 63) == 0) {
      auto t0 = std::chrono::steady_clock::now();
      benchmark::DoNotOptimize(g_mvcc_tree->Read(key, snap));
      auto t1 = std::chrono::steady_clock::now();
      latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    } else {
      benchmark::DoNotOptimize(g_mvcc_tree->Read(key, snap));
    }
    ++iter_count;
  }

  state.SetItemsProcessed(state.iterations());

  // Emit percentiles as custom counters.
  if (!latencies.empty()) {
    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    state.counters["P50_ns"] = benchmark::Counter(static_cast<double>(latencies[n * 50 / 100]));
    state.counters["P99_ns"] = benchmark::Counter(static_cast<double>(latencies[n * 99 / 100]));
    state.counters["P999_ns"] = benchmark::Counter(static_cast<double>(latencies[n * 999 / 1000]));
  }
}

BENCHMARK(BM_MVCCReadLatency)->Threads(1)->Threads(4)->Threads(16)->UseRealTime()->MinTime(5.0);

// ---------------------------------------------------------------------------
// BM_BaselineMixed — std::map<uint64_t, void*> + std::shared_mutex.
// Same 80/20 workload, same key space. The comparison target.
// ---------------------------------------------------------------------------

namespace {
std::map<uint64_t, void*>* g_baseline_map = nullptr;
std::shared_mutex* g_baseline_mu = nullptr;
std::once_flag g_baseline_flag;
}  // namespace

static void BM_BaselineMixed(benchmark::State& state) {
  std::call_once(g_baseline_flag, []() {
    g_baseline_map = new std::map<uint64_t, void*>();
    g_baseline_mu = new std::shared_mutex();
    for (uint64_t k = 1; k <= kKeySpace; ++k) (*g_baseline_map)[k] = V(k * 10);
  });

  std::mt19937_64 rng(static_cast<uint64_t>(state.thread_index()) * 6364136223846793005ULL + 1);
  std::uniform_int_distribution<uint64_t> key_dist(1, kKeySpace);
  std::bernoulli_distribution is_read(0.80);

  for (auto _ : state) {
    uint64_t key = key_dist(rng);
    if (is_read(rng)) {
      std::shared_lock<std::shared_mutex> lock(*g_baseline_mu);
      auto it = g_baseline_map->find(key);
      if (it != g_baseline_map->end()) benchmark::DoNotOptimize(it->second);
    } else {
      std::unique_lock<std::shared_mutex> lock(*g_baseline_mu);
      (*g_baseline_map)[key] = V(key);
    }
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_BaselineMixed)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->UseRealTime()
    ->MinTime(10.0);

// ---------------------------------------------------------------------------
// BM_BaselineReadOnly — pure reads on std::map + shared_mutex.
// ---------------------------------------------------------------------------

static void BM_BaselineReadOnly(benchmark::State& state) {
  std::call_once(g_baseline_flag, []() {
    g_baseline_map = new std::map<uint64_t, void*>();
    g_baseline_mu = new std::shared_mutex();
    for (uint64_t k = 1; k <= kKeySpace; ++k) (*g_baseline_map)[k] = V(k * 10);
  });

  std::mt19937_64 rng(static_cast<uint64_t>(state.thread_index()) * 6364136223846793005ULL + 1);
  std::uniform_int_distribution<uint64_t> key_dist(1, kKeySpace);

  for (auto _ : state) {
    uint64_t key = key_dist(rng);
    std::shared_lock<std::shared_mutex> lock(*g_baseline_mu);
    auto it = g_baseline_map->find(key);
    if (it != g_baseline_map->end()) benchmark::DoNotOptimize(it->second);
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_BaselineReadOnly)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->UseRealTime()
    ->MinTime(5.0);

// ---------------------------------------------------------------------------

BENCHMARK_MAIN();
