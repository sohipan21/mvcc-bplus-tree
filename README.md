# Lock-Free B+Tree with MVCC

A concurrent B+Tree in C++17 that lets multiple threads read at the same time without blocking each other or blocking writers.

The main ideas:
- **Lock-free reads**: searching the tree never acquires a mutex
- **Snapshot reads**: each reader gets a consistent point-in-time view, even while writes are happening in parallel
- **Safe memory cleanup**: old versions of data are freed automatically once no reader can reach them

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

See [BLOG.md](BLOG.md) for a detailed writeup on design decisions and trade-offs.
