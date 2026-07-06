# Building an MVCC B+Tree with Snapshot Isolation

A writeup on building a concurrent search tree where reads never block, writes don't block readers, and old versions of data get cleaned up safely. This covers the design decisions, where I went wrong the first time, and what the memory ordering actually needs to be.

## Architecture Overview

The core idea behind Multi-Version Concurrency Control (MVCC) is to keep old versions of data around instead of overwriting them in place. Readers grab a timestamp once, then read whatever version of each key was live at that timestamp. Writers create new versions without touching what readers are currently looking at.

The tree is split into three layers:

1. **Structural Index**: a copy-on-write B+Tree. Published nodes are immutable; a writer (serialized behind one mutex) copies the path it modifies and publishes the new tree version with a single atomic store of the root pointer. Readers traverse frozen snapshots.
2. **Version Chains**: one linked list per key, holding all historical values newest-first. Each key gets its own lock so unrelated keys do not contend.
3. **Epoch-Based Reclamation (EBR)**: per-thread epoch tracking that defers freeing replaced tree nodes and old version nodes until no reader can possibly reach them.

The result: a reader calls `BeginRead()` once, captures a timestamp, and then traverses the entire tree and all version chains without acquiring any mutex.

The first version of this tree did NOT use copy-on-write — it mutated nodes in place with atomic fields. That version had a linearizability bug that ThreadSanitizer could not see, and finding it is the best story in this writeup (see "The bug TSan couldn't see" below).

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

## The bug TSan couldn't see (and why the tree is copy-on-write now)

The first version of the tree mutated nodes in place. Every field a reader touched was atomic: `keys[]` and `children[]` were arrays of `std::atomic`, and `num_keys` was published with a trailing release store after the relaxed in-place shifts, acquire-loaded first by readers. All tests passed. ThreadSanitizer was silent.

Then I wrote a stricter test (`tests/test_linearizability.cpp`): keep a band of "stable" keys permanently in the tree, churn the keys between them with inserts and deletes to force splits and merges on the stable keys' search paths, and assert that a stable key NEVER reads back as absent. It failed within milliseconds:

```
Read(61, snap) = 0, expected frozen value 610
```

A key that was present the entire run read back as nullptr — while TSan reported zero races on the same run. That is the important lesson: **data-race freedom is not linearizability.** Every access was atomic, so TSan had nothing to complain about, but a reader that loaded the *old* `num_keys` of an internal node had no happens-before edge to the in-place key shifts of a concurrent merge. It read a torn separator array, descended into the wrong subtree, and honestly reported "not found."

I considered patching the read path (a B-link-style high key on internal nodes would let a misrouted reader recover), but the honest conclusion was that in-place mutation was the wrong foundation for a read-optimized tree. The fix was to make structural writes **copy-on-write**:

- a writer copies the root-to-leaf path it modifies (plus any sibling consumed by a merge), applies the change to the fresh nodes, and publishes the entire new tree version with ONE `root_.store(release)`;
- a reader does ONE `root_.load(acquire)` and traverses a frozen, immutable snapshot.

Torn intermediate states are now unrepresentable — there is nothing to observe halfway because published nodes never change. The failing tests were re-enabled and act as permanent regression guards. As a bonus, node fields no longer need to be atomic at all: the single release/acquire pair on the root pointer covers everything reachable from it, which simplifies `SearchNode` to plain loads and makes the proof of correctness one sentence instead of a page of per-field ordering arguments.

The costs are real and worth stating: every write now allocates O(log n) nodes (write amplification), and deleting the leaf-chain pointer (a chain would dangle across snapshot versions) means range scans re-descend once per leaf boundary instead of following a `next` pointer. For a read-optimized MVCC tree, both are the right trade.

## Memory Ordering

Copy-on-write collapsed most of the memory-ordering surface into two spots.

### One release/acquire pair for the whole tree

The writer builds fresh nodes with plain stores, then publishes with `root_.store(new_root, memory_order_release)`. The reader's `root_.load(memory_order_acquire)` synchronizes with it, which makes every plain field of every reachable node visible. There is no per-node protocol anymore — the linearization point of a read is the root load.

The same pattern applies to MVCC. The writer increments `global_version` after... actually, the writer *announces* first — see the commit watermark below.

### Seq_Cst in the EBR announcements

Readers announce their epochs with `memory_order_seq_cst` stores in `ThreadEnterEpoch`, and the reclaimer opens its scan with a seq_cst fence. This Dekker-style pairing is what makes deferred freeing safe: either the reclaimer observes the reader's announcement (and spares the retired node), or the reader's announcement — and therefore all of its traversal loads — is ordered after the reclaimer's scan, which means the reader observed the post-retirement root and can never reach the node being freed. Without the full-fence semantics, a CPU could hoist a tree load above the announcement and dereference memory the reclaimer already freed.

## Snapshot Isolation, and the second bug a model checker caught

I originally believed the version-chain design was airtight, and the first draft of this post claimed that `UpdateVersion()` — which tombstones the old head and prepends the new version under the per-key lock — left "no moment where neither version is visible." A model checker proved that claim wrong.

The checker (`tests/test_model_check.cpp`) records every operation together with the `global_version` values loaded immediately before and after it. Since every committing write draws its timestamp from one `fetch_add`, each write's unknown commit timestamp is bracketed: `before < ts <= after`. Keys are partitioned across writer threads, so each key's writes form one thread's program order and the set of writes visible at any snapshot must be a *prefix* of that sequence. After the run, every recorded `Read(key, snap)` is checked against every prefix the timestamp brackets allow. If the result matches none of them, no serial history explains it.

First run: 32 violations out of 20,000 reads, like this one —

```
Read(key=51, snap=20) = 0; candidates: 53608459, 53608464
```

Two consecutive inserts can never produce "absent," yet a concurrent reader saw it. Two distinct bugs fell out:

1. **Tombstone before publish.** `UpdateVersion` stored `deleted_at = ts` on the old head *before* swinging the chain head to the new version. A lock-free reader in between saw a tombstoned head with no successor: the key vanished at snapshots where it must exist. Fix: publish the new head first, then tombstone the old one — every interleaving is now consistent.

2. **Timestamps become readable before their writes are published.** Every writer drew `ts = fetch_add + 1` and *then* built and published the version. A reader could call `BeginRead()`, get a snapshot >= ts, look up the key before the writer finished, and see nothing — then see the value on a second read *at the same snapshot*. Non-repeatable reads at a fixed snapshot are precisely what snapshot isolation forbids.

The second bug is structural, and the fix is the same one production MVCC engines use: a **commit watermark**. A writer announces an in-flight commit in its epoch slot *before* drawing the timestamp (`claiming` → draws ts → announces ts → publishes → clears). `BeginRead()` loads `global_version` and then scans the slots: any writer whose timestamp could be covered by the snapshot has already announced (the announcement is seq_cst-ordered before its `fetch_add`), so the reader either sees the announcement and caps its snapshot below the oldest in-flight commit, or sees the cleared slot of a fully published write. A snapshot can therefore never straddle a half-published commit.

With both fixes, the checker passes repeatedly under TSan — and re-breaking the watermark on purpose makes it fail within milliseconds, which is the property that makes it worth trusting.

The ordering chain that makes snapshot reads correct end-to-end:

1. Writer announces `commit_ts` (seq_cst), draws `ts` from `global_version` (seq_cst), publishes the version node under the per-key lock, clears the announcement (release)
2. Reader's `BeginRead()` loads `global_version` and scans announcements (seq_cst) — the returned snapshot covers only fully published timestamps
3. Reader walks the chain with acquire loads, pairing with the writer's release publication
4. By transitivity the reader sees every version its snapshot covers, and only those

## Benchmark Results

Measured on an Apple M-series laptop (10 physical cores), 10k-key space, 80/20 read/write workload. I compare against three baselines instead of one, because a fair comparison matters more than a flattering one:

- **1 global lock** — `std::map` + a single `std::shared_mutex`. The naive baseline.
- **64 shards** — `std::map` split into 64 independently-locked stripes. A *fair ordered* baseline: same data structure, but the global lock is gone.
- **libcuckoo** — a production concurrent hash map. The strongest point-operation baseline (but a hash map, so no ordered scan).

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

The single global lock *collapses* under contention — from 7.3M down to 0.8M on the mixed workload — because every write excludes all readers. Beating it by ~19x is real but uninteresting; that is just what global locks do.

The honest comparison is the **64-shard map**. It removes the global-lock collapse but still doesn't scale: every read takes a `shared_mutex` in shared mode, and the atomic bookkeeping inside `shared_mutex` bounces cache lines between cores. The MVCC tree's reads take *no* lock, so they pull ahead under load — ~5x read-only at 16 threads (65.5 vs 12.4). That gap is the entire point of the lock-free read path.

**The mixed column is where copy-on-write sends its bill.** Every write clones an O(log n) path, so mixed throughput tops out at 14.9M at 8 threads and dips when oversubscribed, staying ~1.5-1.9x over the sharded map rather than 3x. Worth being explicit: the earlier in-place version of this tree posted better mixed numbers (25.8M at 16 threads) — and it was *wrong*, in the reproducible-linearizability-violation sense described above. A benchmark number from a data structure that returns incorrect results under exactly the contention being benchmarked is not a number; the regression from 25.8 to 12.3 is what buying correctness cost, and I would make that trade every time.

**libcuckoo is the humbling one, and it should be.** A well-tuned concurrent hash map is faster on raw point operations across most of the range — single-threaded it is ~3x faster than this tree, because a hash lookup is one probe while a B+Tree lookup chases pointers down several levels and then walks a version chain. The tree's read path overtakes it at 16 threads (65.5 vs 25.4), but I would not lead with that: libcuckoo is a hash map. It has no ordered range scans and no snapshot isolation. Those are the two things this tree is *for*. The right reading of this table is not "the tree is fast," it is "the tree's read path scales like a lock-free structure should, while also giving ordered + versioned reads a hash map can't."

Caveats worth stating: the 16-thread rows oversubscribe a 10-core machine, so they are noisier (libcuckoo visibly dips there). Google Benchmark misreports `mhz_per_cpu` on Apple Silicon, so I ignore it. These are single-run numbers — directional, not publication-grade.

## Trade-offs and Limitations

### Serialized, Copy-on-Write Structural Writes

B-link trees can do lock-free structural modifications, but they require right-sibling pointers and more involved recovery logic. I kept writes behind a single `write_mutex_` and made them copy-on-write. Two costs follow:

- **Write amplification**: every insert/delete allocates fresh copies of the O(log n) nodes on its path, even a single-value overwrite. Read-heavy workloads never notice; write-heavy workloads pay for the immutable read path.
- **No leaf chain**: a `next` pointer would dangle across snapshot versions, so range scans re-descend from the (snapshot's) root once per leaf boundary — O(log n) per leaf instead of O(1). The scan still observes exactly one structural version of the tree, which is what makes it snapshot-consistent.

If writes dominated, the fixes would be a per-subtree write lock plus B-link-style reader recovery — a very different complexity budget.

### BeginRead Scans the Slot Array

The commit watermark makes `BeginRead()` O(kMaxThreads) — it scans all 64 epoch slots for in-flight commits — instead of a single atomic load. Batching reads per snapshot amortizes it, and correctness (repeatable snapshot reads) is worth a bounded, contention-free scan.

### Version Accumulation

Every write adds a version node. Under heavy updates to a single key the chain grows until `PruneVersionNodes()` is called. In practice call it from a background thread on a timer rather than from the write path.

### EBR vs. Hazard Pointers

Hazard pointers are another approach to safe memory reclamation. The main difference is that hazard pointers scan a hazard list per node retired, while EBR only scans once per reclamation batch. For this workload EBR's amortized cost was lower, so that is what I went with.

### The ABA Problem

EBR sidesteps ABA entirely. A node is never freed while any reader's epoch can reference it. Once freed it goes back to the allocator, not back into the tree structure.

### The Global Retire Lock

The single `g_retire_mutex` in the EBR reclamation code would become a bottleneck at very high core counts (100+ cores). Per-thread retire queues would fix this. For now it is not the bottleneck since structural writes under `write_mutex_` hit first.
