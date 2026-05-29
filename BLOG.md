# Building a Lock-Free B+Tree with MVCC Snapshot Isolation

A writeup on building a concurrent search tree where reads never block, writes don't block readers, and old versions of data get cleaned up safely. This covers the design decisions, where I went wrong the first time, and what the memory ordering actually needs to be.

## Architecture Overview

The core idea behind Multi-Version Concurrency Control (MVCC) is to keep old versions of data around instead of overwriting them in place. Readers grab a timestamp once, then read whatever version of each key was live at that timestamp. Writers create new versions without touching what readers are currently looking at.

The tree is split into three layers:

1. **Structural Index**: a B+Tree with atomic child pointers and per-node key counts. Writers only need to hold a mutex during splits and merges, not during the whole operation.
2. **Version Chains**: one linked list per key, holding all historical values newest-first. Each key gets its own lock so unrelated keys do not contend.
3. **Epoch-Based Reclamation (EBR)**: per-thread epoch tracking that defers freeing old version nodes until no reader can possibly reach them.

The result: a reader calls `BeginRead()` once, captures a timestamp in a single atomic load, and then traverses the entire tree and all version chains without acquiring any mutex.

## What I Tried First (and Why It Broke)

My first implementation of `MVCCTree::Read` looked roughly like this:

```cpp
void* MVCCTree::Read(uint64_t key, uint64_t snapshot_ts) const {
  void* existing = tree_.Search(key);   // this exits the EBR epoch internally
  if (existing == nullptr) return nullptr;
  return static_cast<VersionChain*>(existing)->Read(snapshot_ts);
}
```

This compiled and all the single-threaded tests passed. Then I ran the concurrent tests and TSan flagged a use-after-free about one in every fifty runs.

The problem is that `tree_.Search()` enters and exits the EBR epoch internally. So after it returns, there is a gap where the EBR epoch is no longer active but we are still about to walk the version chain. If another thread calls `PruneVersionNodes()` inside that gap, it can see that no reader is active and free version nodes that we are about to dereference.

The fix was to add a `SearchRaw()` variant that skips the epoch enter/exit, and have `MVCCTree::Read` manage the epoch itself so it stays active across both the tree traversal and the version chain walk. Not complicated once you see the bug, but annoying to track down because it only showed up under race conditions with specific timing.

A similar thing happened with thread slot cleanup in EBR. My first version never reset the thread-local slot index when a thread exited, so slots leaked permanently. After running a test that spawned more than 64 threads total, the slot pool would exhaust and the program would abort. Added a thread-local RAII guard to reset the slot on thread exit and that fixed it.

## Memory Ordering

The lock-free reads only work because of careful memory ordering. Here is where the tricky parts are.

### Acquire and Release

The B+Tree node has two atomic fields readers care about:
- `num_keys`: how many keys are live in this node
- `keys[]`: the actual key values

When a writer inserts a key, it writes all the key data first and then does `num_keys.fetch_add(1, memory_order_release)`. That release store is the publication point. Any reader that later loads `num_keys` with `memory_order_acquire` is guaranteed to see all the key stores that happened before the release.

In `SearchNode` this looks like:

```cpp
uint64_t n = node->num_keys.load(memory_order_acquire);  // acquire fence
for (int i = 0; i < n; ++i) {
  uint64_t key = node->keys[i].load(memory_order_relaxed);  // safe after the acquire
  ...
}
```

The acquire on `num_keys` pulls in everything the writer did before its release store. That is why the subsequent key loads can be relaxed.

The same pattern applies to MVCC. The writer increments `global_version` with `fetch_add(release)` after publishing a version node. The reader loads `global_version` with acquire in `BeginRead()`. The acquire/release pair ensures the reader sees the version node.

### Where Relaxed Breaks Down

Inside a mutex-protected section, relaxed atomics are fine because the mutex provides the synchronization. But `global_version` cannot be relaxed. If the writer did `fetch_add(relaxed)`, the CPU could reorder that increment after the version-chain writes. A reader doing an acquire load of the new counter value would not see the version node it was supposed to see.

### Seq_Cst in ThreadEnterEpoch

There is one place that uses `memory_order_seq_cst`: when a reader announces its epoch in `ThreadEnterEpoch`. The reason is that we need a full fence to prevent any subsequent tree load from being reordered before the announcement.

```cpp
g_epoch_slots[slot].store(epoch, memory_order_seq_cst);  // full fence
```

Without the full fence, a CPU could speculatively execute tree loads before the epoch announcement is visible to the reclaimer. The reclaimer might then decide it is safe to free a node that the reader is already holding a pointer to.

## Snapshot Isolation

MVCC snapshot isolation works at the hardware level via this chain:

1. Writer increments `global_version` with `fetch_add(release)` after publishing its version node
2. The release carries a happens-before edge over all prior version-chain writes
3. Reader calls `BeginRead()` and loads `global_version` with acquire
4. By transitivity the reader sees all version nodes published before its snapshot timestamp

One thing that has to be careful: when `UpdateVersion()` runs, it marks the old version deleted and prepends the new version in the same critical section, both with the same timestamp. This means there is never a moment where neither version is visible. A reader at that exact timestamp sees the new version; a reader from just before sees the old one.

## Benchmark Results

Tested on Apple M-series (10 cores), 80/20 read/write workload:

| Threads | MVCC (ops/s) | std::map + shared_mutex (ops/s) | Speedup |
|---------|-------------|--------------------------------|---------|
| 1       | 6.8M        | 7.1M                           | 0.96x   |
| 4       | 11.2M       | 3.8M                           | 2.9x    |
| 8       | 13.4M       | 2.1M                           | 6.4x    |
| 16      | 15.9M       | 0.77M                          | 20.6x   |

At 1 thread there is no contention, so the version chain overhead and EBR batching cost more than they save. At 16 threads, `shared_mutex` becomes a bottleneck because every write blocks all readers. The per-key version lock lets unrelated keys proceed in parallel.

Read-only at 16 threads: 45.8M ops/s vs 3.3M for shared_mutex.

## Trade-offs and Limitations

### Serialized Splits and Merges

B-link trees can do lock-free structural modifications, but they require right-sibling pointers and more involved recovery logic. I kept splits and merges behind a single `write_mutex_`. This simplifies the code a lot and is fine for read-heavy workloads. If writes dominate, this becomes the bottleneck.

### Version Accumulation

Every write adds a version node. Under heavy updates to a single key the chain grows until `PruneVersionNodes()` is called. In practice call it from a background thread on a timer rather than from the write path.

### EBR vs. Hazard Pointers

Hazard pointers are another approach to safe memory reclamation. The main difference is that hazard pointers scan a hazard list per node retired, while EBR only scans once per reclamation batch. For this workload EBR's amortized cost was lower, so that is what I went with.

### The ABA Problem

EBR sidesteps ABA entirely. A node is never freed while any reader's epoch can reference it. Once freed it goes back to the allocator, not back into the tree structure.

### The Global Retire Lock

The single `g_retire_mutex` in the EBR reclamation code would become a bottleneck at very high core counts (100+ cores). Per-thread retire queues would fix this. For now it is not the bottleneck since structural writes under `write_mutex_` hit first.
