# MVCC B+Tree

A concurrent B+Tree in C++17 with lock-free snapshot reads. Readers never take a lock and never block writers; writers are serialized behind a single mutex and publish each new tree version with one atomic swap of the root pointer.

Built as a personal project to get hands-on with C++ atomics, concurrent data structures, and database-style MVCC.

## Design

Three layers:

- **Copy-on-write B+Tree** — published nodes are immutable. A writer copies the root-to-leaf path it modifies, applies the change to the fresh copies, and publishes the new tree with a single atomic store of the root. Readers traverse a frozen snapshot, so they can never see a half-applied split or merge.
- **Version chains** — each key keeps its historical values, newest first. `BeginRead()` returns a timestamp; a read walks the chain to the value that was live at that timestamp. A commit watermark keeps a snapshot from ever straddling a half-published write.
- **Epoch-based reclamation** — replaced tree nodes and old versions are freed once no active reader can still reach them. Call `PruneVersionNodes()` periodically (e.g. from a background thread) to trim version chains.

## Building

Requires a C++17 compiler and CMake 3.15+. GoogleTest is fetched automatically.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Tests:

```bash
build/bin/test_btree_basic      # B+Tree correctness
build/bin/test_mvcc             # snapshot isolation
build/bin/test_concurrent       # multi-threaded safety + reclamation
build/bin/test_linearizability  # lock-free read guarantees under churn
build/bin/test_model_check      # model-based snapshot-isolation checker
```

For race and memory checking, configure a separate build with `-DENABLE_TSAN=ON` or `-DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"`.

## Example

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

## Performance

Apple M-series laptop (10 physical cores), 10k-key space. Baselines: `std::map` behind a single `std::shared_mutex`, `std::map` split into 64 locked shards, and libcuckoo (a concurrent hash map — point ops only, no ordered scan).

Mixed 80/20 read/write (M ops/s):

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

Reads scale near-linearly because they take no lock. Mixed throughput peaks at 8 threads: every write clones its root-to-leaf path, which is the cost of the immutable read path. libcuckoo wins raw point ops at low thread counts, but it has no ordered scans and no snapshots — the two things this tree is for.

Numbers are from a single run, and the 16-thread rows oversubscribe a 10-core machine. Reproduce with `build_release/bin/benchmark_btree`.

See [BLOG.md](BLOG.md) for the design decisions and the bugs found along the way.
