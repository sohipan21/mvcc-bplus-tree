# MVCC B+Tree

A concurrent B+Tree in C++17 that lets multiple threads read at the same time without blocking each other or blocking writers.

The main ideas:
- **Lock-free, linearizable reads**: searching the tree never acquires a mutex, and a reader can never observe a half-applied split or merge — structural writes are copy-on-write, so every read traverses an immutable snapshot of the tree
- **Snapshot reads**: each reader gets a consistent point-in-time view, even while writes are happening in parallel; a commit watermark guarantees a snapshot never straddles a half-published write
- **Range scans**: iterate over a key range at a snapshot — results are consistent even with concurrent writes
- **Safe memory cleanup**: replaced tree nodes and old versions are freed automatically once no reader can reach them (epoch-based reclamation, two clocks)

Reads are lock-free; writes are serialized behind a single write mutex and publish each new tree version with one atomic root swing.

Built as a personal project to get hands-on with C++ atomics, concurrent data structures, and database-style multi-version concurrency control (MVCC).

## Requirements

- C++17 compiler (GCC 10+ or Clang 12+)
- CMake 3.15 or newer
- GoogleTest (fetched automatically by CMake)

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

## Running the tests

```bash
build/bin/test_btree_basic      # correctness tests for the B+Tree
build/bin/test_mvcc             # snapshot isolation tests
build/bin/test_concurrent      # multi-threaded safety + EBR reclamation tests
build/bin/test_linearizability  # strict lock-free read guarantees under churn
build/bin/test_model_check      # model-based snapshot-isolation checker
```

## Quick example

```cpp
MVCCTree tree;

// Insert a value
int val = 42;
tree.Insert(1, &val);

// Take a read snapshot
uint64_t snap = tree.BeginRead();
void* result = tree.Read(1, snap);  // returns &val

// Update -- old snapshot still sees the original value
int updated = 99;
tree.Update(1, &updated);
assert(tree.Read(1, snap) == &val);                // old snapshot unchanged
assert(tree.Read(1, tree.BeginRead()) == &updated); // fresh snapshot sees new value

// Delete
tree.Delete(1);

// Range scan at a snapshot — only sees versions visible at snap
tree.Scan(1, 100, snap, [](uint64_t key, void* value) {
    // called for each key in [1, 100] that existed at snap
});
```

## How it works

The tree is split into three layers that each handle one concern:

**1. B+Tree (the index) — copy-on-write**
Stores keys and pointers to version chains. Published nodes are immutable: a writer copies the root-to-leaf path it modifies, applies the split/merge/insert to the fresh copies, and publishes the whole new tree version with a single atomic store of the root pointer. A reader loads the root once and traverses a frozen snapshot — it can never see a torn key array or a half-applied merge, which makes reads linearizable by construction. Range scans walk one snapshot end-to-end, hopping between leaves by re-descending on the next separator.

**2. Version chains**
Each key has a linked list of all its historical values, newest first. When you call `BeginRead()` you get a timestamp. When you read a key at that timestamp, you walk the chain to find the value that was live at that point in time. This means readers and writers never wait on each other. A commit watermark makes commits atomic to readers: writers announce an in-flight timestamp before publishing, and `BeginRead()` caps its snapshot below the oldest in-flight commit, so a snapshot can never see "half" of a write.

**3. Epoch-Based Reclamation (EBR)**
Replaced tree nodes and superseded versions cannot be freed the moment they are unlinked, because a concurrent reader might still be traversing them. EBR runs two clocks: a structural epoch (advanced on every root swing, reclaims retired tree nodes) and the MVCC snapshot clock (reclaims pruned version nodes). Readers announce both on entry; retired memory is freed once every active reader has moved past it. Call `PruneVersionNodes()` periodically from a background thread to trim version chains.

## Sanitizer builds

**ThreadSanitizer** (detects data races):
```bash
cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build_tsan --parallel
build_tsan/bin/test_btree_basic
build_tsan/bin/test_mvcc
build_tsan/bin/test_concurrent
```

**AddressSanitizer** (detects memory errors):
```bash
cmake -B build_asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build_asan --parallel
build_asan/bin/test_concurrent
```

## Performance

Measured on an Apple M-series laptop (10 physical cores), 10k-key space, 80/20
read/write workload. Four implementations under the same harness:

- **MVCC** — this tree (lock-free reads, snapshot isolation)
- **1 global lock** — `std::map` + a single `std::shared_mutex` (the naive baseline)
- **64 shards** — `std::map` split into 64 independently-locked stripes (a fair *ordered* baseline)
- **libcuckoo** — a production concurrent hash map (point ops only; no ordered scan)

Mixed 80/20 (M ops/s):

| Threads | MVCC | 1 global lock | 64 shards | libcuckoo |
|---------|------|---------------|-----------|-----------|
| 1       | 6.2  | 7.3           | 8.5       | 19.2      |
| 4       | 12.3 | 1.3           | 9.9       | 31.8      |
| 8       | 14.9 | 0.6           | 8.6       | 44.8      |
| 16      | 12.3 | 0.8           | 8.1       | 25.4      |

Read-only (M ops/s):

| Threads | MVCC | 1 global lock | 64 shards | libcuckoo |
|---------|------|---------------|-----------|-----------|
| 1       | 8.8  | 8.4           | 9.6       | 27.6      |
| 4       | 33.6 | 2.7           | 14.0      | 44.8      |
| 8       | 50.8 | 3.4           | 12.2      | 52.2      |
| 16      | 65.5 | 3.4           | 12.4      | 25.4      |

How to read this honestly:

- **Read-only is the design's home turf:** near-linear scaling to 65.5 M ops/s at 16 threads —
  ~5x the 64-shard map (whose `shared_mutex` bookkeeping bounces cache lines) and past libcuckoo
  at high thread counts. Reads take no lock and traverse immutable snapshots; this column is the
  entire point of the tree.
- **Mixed shows the price of copy-on-write:** every write clones its root-to-leaf path, so mixed
  throughput peaks at 8 threads (14.9M) and dips when oversubscribed. Still ~1.5x the sharded map
  under load and ~19x the global lock, but writes are the serial bottleneck by design. An earlier
  in-place version of this tree posted higher mixed numbers — and had a linearizability bug the
  in-place design made unavoidable (see BLOG.md); those numbers don't count.
- **vs. libcuckoo:** a tuned concurrent hash map wins raw point ops at low-to-mid concurrency
  (single-threaded the tree is ~3x slower). The honest takeaway: libcuckoo is a *hash* map — no
  ordered range scans, no snapshot isolation. Those two features are the tree's reason to exist,
  not winning a point-lookup race.

Caveats: the 16-thread rows oversubscribe a 10-core machine, so those points are noisier
(libcuckoo in particular dips). `mhz_per_cpu` is misreported by Google Benchmark on Apple Silicon
and is ignored. Numbers are a single run — treat them as directional, and reproduce with
`build_release/bin/benchmark_btree`.

See [BLOG.md](BLOG.md) for a detailed writeup on design decisions and trade-offs.
