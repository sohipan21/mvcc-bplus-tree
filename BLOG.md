# Building an MVCC B+Tree with Snapshot Isolation

Notes from building a concurrent search tree where reads never block, writes don't block readers, and old versions get cleaned up safely. Mostly about two bugs — one ThreadSanitizer caught, one it couldn't — and how they shaped the design.

## Architecture

MVCC keeps old versions of data around instead of overwriting in place. A reader grabs a timestamp once, then reads whatever version of each key was live at that timestamp. Writers create new versions without touching what readers are looking at.

The tree has three layers:

1. **Copy-on-write B+Tree**: published nodes are immutable. A writer (serialized behind one mutex) copies the path it modifies and publishes the new tree with a single atomic store of the root pointer.
2. **Version chains**: one linked list per key, newest first, with a per-key lock so unrelated keys don't contend.
3. **Epoch-based reclamation (EBR)**: defers freeing replaced nodes and old versions until no reader can possibly reach them.

A reader calls `BeginRead()` once, captures a timestamp, and then traverses the whole tree and all version chains without acquiring any mutex.

The first version of the tree was not copy-on-write. It mutated nodes in place with atomic fields, and it had a linearizability bug TSan couldn't see — which is most of why this post exists.

## An early use-after-free

My first `MVCCTree::Read` looked like this:

```cpp
void* MVCCTree::Read(uint64_t key, uint64_t snapshot_ts) const {
  void* existing = tree_.Search(key);   // enters and exits the EBR epoch internally
  if (existing == nullptr) return nullptr;
  return static_cast<VersionChain*>(existing)->Read(snapshot_ts);
}
```

Single-threaded tests passed. Under the concurrent tests, TSan flagged a use-after-free about one run in fifty. The problem: `tree_.Search()` exits the EBR epoch before returning, so there's a gap where no epoch is active but we're about to walk the version chain. If another thread runs `PruneVersionNodes()` in that gap, it sees no active readers and frees nodes we're about to dereference. The fix was a `SearchRaw()` variant that skips the epoch enter/exit, with `Read` holding the epoch across both the traversal and the chain walk.

EBR had a second, dumber bug: thread slots were never reset on thread exit, so any test that spawned more than 64 threads over its lifetime exhausted the slot pool and aborted. A thread-local RAII guard fixed it.

## The bug TSan couldn't see

The original tree mutated nodes in place. Every field a reader touched was atomic: `keys[]` and `children[]` were arrays of `std::atomic`, and `num_keys` was published with a trailing release store, acquire-loaded by readers. All tests passed and TSan was silent.

Then I wrote a stricter test (`tests/test_linearizability.cpp`): keep a band of "stable" keys permanently in the tree, churn the keys around them with inserts and deletes to force splits and merges on the stable keys' search paths, and assert that a stable key never reads back as absent. It failed within milliseconds:

```
Read(61, snap) = 0, expected frozen value 610
```

A key that was present the entire run read back as null, on a run where TSan reported zero races. Data-race freedom is not linearizability: every access was atomic, so TSan had nothing to complain about, but a reader that loaded the *old* `num_keys` of an internal node had no happens-before edge to the in-place key shifts of a concurrent merge. It read a torn separator array, descended into the wrong subtree, and reported "not found."

I considered patching the read path (a B-link-style high key would let a misrouted reader recover), but in-place mutation was the wrong foundation for a read-optimized tree. So structural writes became copy-on-write: a writer copies the root-to-leaf path it modifies (plus any sibling consumed by a merge), applies the change to the fresh nodes, and publishes the whole new version with one `root_.store(release)`. A reader does one `root_.load(acquire)` and traverses a frozen snapshot. Torn intermediate states are now unrepresentable — published nodes never change, so there's nothing to observe halfway. The failing test stays on as a regression guard.

A nice side effect: node fields no longer need to be atomic at all. The single release/acquire pair on the root covers everything reachable from it, which turns `SearchNode` into plain loads and the correctness argument into one sentence instead of a page of per-field ordering.

The costs are real: every write allocates O(log n) fresh nodes, and there's no leaf `next` pointer (it would dangle across snapshot versions), so range scans re-descend once per leaf boundary. For a read-optimized tree, both are acceptable.

## Memory ordering

Copy-on-write collapsed most of the memory-ordering surface into two spots.

**One release/acquire pair for the tree.** The writer builds fresh nodes with plain stores and publishes with `root_.store(release)`; the reader's `root_.load(acquire)` synchronizes with it and makes every reachable field visible. The linearization point of a read is the root load.

**Seq_cst in the EBR announcements.** Readers announce their epochs with seq_cst stores, and the reclaimer opens its scan with a seq_cst fence. This Dekker-style pairing is what makes deferred freeing safe: either the reclaimer sees the announcement and spares the retired node, or the reader's announcement — and all its traversal loads — is ordered after the scan, meaning the reader saw the post-retirement root and can't reach the node being freed. With weaker ordering, a CPU could hoist a tree load above the announcement and touch freed memory.

## Snapshot isolation and the model checker

I believed the version-chain design was airtight, and an early draft of this post claimed `UpdateVersion()` left "no moment where neither version is visible." A model checker proved that wrong.

The checker (`tests/test_model_check.cpp`) records every operation with the `global_version` values loaded immediately before and after it. Since every commit draws its timestamp from one `fetch_add`, each write's timestamp is bracketed: `before < ts <= after`. Keys are partitioned across writer threads, so each key's writes form one program order, and the set of writes visible at any snapshot must be a prefix of that sequence. After the run, every recorded read is checked against every prefix the brackets allow; if it matches none, no serial history explains it.

First run: 32 violations out of 20,000 reads, like this one —

```
Read(key=51, snap=20) = 0; candidates: 53608459, 53608464
```

Two consecutive inserts can never produce "absent," yet a reader saw it. Two bugs fell out:

1. **Tombstone before publish.** `UpdateVersion` marked the old head deleted *before* swinging the chain head to the new version, so a reader in between saw a tombstoned head with no successor and the key vanished. Fix: publish the new head first, then tombstone the old one.

2. **Timestamps readable before their writes are published.** A writer drew `ts = fetch_add + 1` and *then* built and published the version. A reader could get a snapshot >= ts, look up the key before the writer finished, see nothing — then see the value on a second read at the same snapshot. Non-repeatable reads at a fixed snapshot are exactly what snapshot isolation forbids.

The second bug is structural, and the fix is the one production MVCC engines use: a **commit watermark**. A writer announces an in-flight commit in its epoch slot before drawing the timestamp, and `BeginRead()` scans the slots and caps its snapshot below the oldest in-flight commit. Since the announcement is seq_cst-ordered before the `fetch_add`, a reader either sees the announcement or sees the cleared slot of a fully published write — a snapshot can never straddle a half-published commit.

With both fixes the checker passes repeatedly under TSan, and re-breaking the watermark on purpose makes it fail within milliseconds, which is what makes it worth trusting.

## Benchmarks

Apple M-series laptop (10 physical cores), 10k-key space. Three baselines: `std::map` behind a single `std::shared_mutex`, `std::map` split into 64 locked shards (same data structure, no global lock), and libcuckoo (a concurrent hash map — the strongest point-op baseline, but no ordered scan).

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

The global lock collapses under contention, as global locks do; beating it 19x is uninteresting. The real comparison is the 64-shard map: it removes the collapse but still doesn't scale, because every read takes a `shared_mutex` and the bookkeeping inside it bounces cache lines between cores. The MVCC tree's reads take no lock, so they pull ahead under load — about 5x read-only at 16 threads. That gap is the point of the lock-free read path.

The mixed column is where copy-on-write charges for it. Every write clones an O(log n) path, so mixed throughput tops out at 14.9M at 8 threads and dips when oversubscribed. The earlier in-place tree posted 25.8M at 16 threads on this workload — and returned wrong results under exactly this contention, so that number doesn't count. The drop to 12.3 is what correctness cost.

libcuckoo is faster on raw point ops across most of the range (3x single-threaded — a hash probe vs. a multi-level pointer chase plus a version-chain walk). The tree's reads overtake it at 16 threads, but the honest framing isn't "the tree is fast": it's that the read path scales like a lock-free structure should while also providing ordered range scans and snapshot isolation, which a hash map can't.

Caveats: single-run numbers, and the 16-thread rows oversubscribe a 10-core machine (libcuckoo visibly dips there). Google Benchmark misreports `mhz_per_cpu` on Apple Silicon, so I ignore it.

## Trade-offs

- **Serialized, copy-on-write writes.** B-link trees can do lock-free structural changes, but need right-sibling pointers and recovery logic. Keeping writes behind one mutex and making them copy-on-write costs write amplification (O(log n) allocations per write) and the leaf chain (scans re-descend per leaf boundary). If writes dominated, per-subtree locks plus B-link reader recovery would be the move — a very different complexity budget.
- **`BeginRead()` scans the slot array.** The commit watermark makes it O(kMaxThreads) instead of one atomic load. Batching reads per snapshot amortizes it.
- **Version accumulation.** Heavy updates to one key grow its chain until `PruneVersionNodes()` runs; call it from a background thread on a timer.
- **EBR over hazard pointers.** Hazard pointers scan per retired node; EBR scans once per reclamation batch, which was cheaper for this workload. EBR also sidesteps ABA entirely: a node is never freed while any reader's epoch can reference it.
- **Global retire lock.** The single `g_retire_mutex` would bottleneck at very high core counts. Per-thread retire queues would fix it; for now structural writes hit the write mutex first.
