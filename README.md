# MVCC B+Tree

A concurrent B+Tree in C++17 that lets multiple threads read at the same time without blocking each other or blocking writers.

The main ideas:
- **Lock-free reads**: searching the tree never acquires a mutex
- **Snapshot reads**: each reader gets a consistent point-in-time view, even while writes are happening in parallel
- **Range scans**: iterate over a key range at a snapshot — results are consistent even with concurrent writes
- **Safe memory cleanup**: old versions of data are freed automatically once no reader can reach them

Reads are lock-free; structural writes (splits/merges) are serialized behind a single write mutex.

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
build/bin/test_btree_basic     # correctness tests for the B+Tree
build/bin/test_mvcc            # snapshot isolation tests
build/bin/test_concurrent      # multi-threaded safety tests
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

**1. B+Tree (the index)**
Stores keys and pointers to version chains. Reads traverse the tree without holding any lock. Structural changes like node splits and merges go through a single write mutex, which keeps the code manageable.

**2. Version chains**
Each key has a linked list of all its historical values, newest first. When you call `BeginRead()` you get a timestamp. When you read a key at that timestamp, you walk the chain to find the value that was live at that point in time. This means readers and writers never wait on each other.

**3. Epoch-Based Reclamation (EBR)**
Old versions cannot be freed the moment they are replaced because a concurrent reader might still be walking through them. EBR tracks which "epoch" each thread is operating in and defers freeing memory until every reader has moved past the relevant point. Call `PruneVersionNodes()` periodically from a background thread to reclaim memory.

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
| 1       | 6.3  | 7.5           | 9.6       | 22.8      |
| 4       | 16.4 | 1.3           | 10.1      | 30.0      |
| 8       | 21.8 | 0.6           | 8.2       | 46.8      |
| 16      | 25.8 | 0.8           | 7.9       | 21.8      |

Read-only (M ops/s):

| Threads | MVCC | 1 global lock | 64 shards | libcuckoo |
|---------|------|---------------|-----------|-----------|
| 1       | 9.2  | 8.9           | 10.3      | 31.2      |
| 4       | 34.5 | 2.8           | 14.5      | 59.1      |
| 8       | 58.0 | 3.4           | 12.2      | 52.5      |
| 16      | 77.8 | 3.8           | 12.3      | 20.7      |

How to read this honestly:

- **vs. a single global lock:** ~30x faster at 16 threads — but a single global lock is
  the worst case, so this mostly shows that global locks don't scale, not that the tree is fast.
- **vs. 64-way sharded `std::map`** (the fair ordered baseline): the tree wins clearly under
  load — ~3x mixed and ~6x read-only at 16 threads — because its reads take no lock at all,
  while every shard read still pays `shared_mutex` cache-line traffic. This is the result the
  design is actually about.
- **vs. libcuckoo:** a tuned concurrent hash map is faster on raw point operations at low-to-mid
  concurrency (single-threaded the tree is ~3-4x slower). The tree's lock-free reads close the gap
  and overtake at high thread counts here, but the honest takeaway is different: libcuckoo is a
  *hash* map — no ordered range scans, no snapshot isolation. Those two features are the tree's
  reason to exist, not winning a point-lookup race.

Caveats: the 16-thread rows oversubscribe a 10-core machine, so those points are noisier
(libcuckoo in particular dips). `mhz_per_cpu` is misreported by Google Benchmark on Apple Silicon
and is ignored. Numbers are a single run — treat them as directional, and reproduce with
`build_release/bin/benchmark_btree`.

See [BLOG.md](BLOG.md) for a detailed writeup on design decisions and trade-offs.
