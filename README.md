# Lock-Free B+Tree with MVCC

A high-performance concurrent B+Tree in C++17.

Reads are fully lock-free — `Search()` never acquires a mutex. Concurrent writes are serialized behind a single mutex. Multi-Version Concurrency Control (MVCC) provides consistent point-in-time snapshots so readers never block writers. Epoch-Based Reclamation (EBR) handles safe deferred memory management without hazard pointers.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

### Running tests

```bash
build/bin/test_btree_basic
```

### ThreadSanitizer build

```bash
cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build_tsan --parallel
build_tsan/bin/test_btree_basic
```

## Architecture

The design has three layers:

1. **Structural Index** — B+Tree with atomic child pointers and per-node key counts; structural modifications (splits and merges) are serialized behind `write_mutex_`, so reads never block on a write lock
2. **Version Chains** — one linked list per key, holding all historical values (newest first); each key has its own `version_lock_` so independent keys don't contend
3. **Epoch-Based Reclamation (EBR)** — per-thread epoch slots let readers announce their snapshot timestamp; a node is freed only when its timestamp falls below the oldest active reader's epoch

The payoff: writers never block readers. A reader calls `BeginRead()` once, captures a timestamp in a single atomic load, and traverses the entire tree and version chains without acquiring any mutex.

More details, benchmark results, and design notes coming as development continues.
