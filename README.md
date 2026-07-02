# LSM-Tree Key-Value Storage Engine

A high-performance, concurrent, log-structured merge-tree (LSM-Tree) key-value storage engine built from scratch in C++. 

This project implements the core components of modern database storage engines (similar to RocksDB or LevelDB) using modern C++17 and raw POSIX system calls. Under concurrency benchmarking, it achieves throughputs exceeding **54,000 operations/second** with sub-30 microsecond median latencies.

---

## Architecture Overview

```
                      +-------------------+
                      |   lsm_cli / App   |
                      +-------------------+
                                |   (Put / Get / Delete)
                                v
                      +-------------------+
                      |   DB Interface    |
                      +-------------------+
                        /               \
                       / (Write WAL)     \ (Write/Read)
                      v                   v
            +------------------+    +---------------------------+
            |  Write-Ahead Log |    |     SkipList MemTable     | (RAM)
            |      (Disk)      |    |        (Active)           |
            +------------------+    +---------------------------+
                                                  |
                                                  | (Flush when full)
                                                  v
                                    +---------------------------+
                                    |     SSTable Files (L0)    | (Disk)
                                    +---------------------------+
                                                  |
                                                  | (Leveled Compaction)
                                                  v
                                    +---------------------------+
                                    |     SSTable Files (L1)    | (Disk)
                                    +---------------------------+
```

1. **SkipList MemTable**: In-memory sorted writes. Synchronization is handled via `std::shared_mutex` (reader-writer lock) to allow multiple concurrent readers but serial write sequences.
2. **Write-Ahead Log (WAL)**: Append-only binary log that records transactions before they hit the MemTable. Flushed using `fsync()` to ensure transactional durability (ACID). FNV-1a checksums guard against data corruption.
3. **Sorted String Tables (SSTables)**: Sorted data structures on disk split into:
   * **Data Block**: Contiguous byte format keys and values.
   * **Sparse Index**: Key-to-offset mappings built for every 16 keys, allowing binary search reads.
   * **Bloom Filter**: Kirsch-Mitzenmacher optimized bit arrays to skip reading files that don't contain the target key.
   * **Footer**: A trailing 40-byte control structure storing offsets and magic format bytes.
4. **Atomic Manifest**: A metadata ledger (`MANIFEST` file) that tracks active SSTable files across levels. Catalog writes are done atomically using POSIX `rename` to prevent database catalog corruption during power loss.
5. **Background Compaction**: A dedicated worker thread automatically merges overlapping Level 0 SSTables into sorted, non-overlapping Level 1 partitions using a streaming merge-sort iterator, performing bottom-level tombstone eviction to reclaim disk space.

---

## Directory Structure

* `src/common.h` — Status structures, serialization helpers, and FNV-1a hashing.
* `src/skiplist.h` — Concurrent SkipList implementation.
* `src/wal.h` / `src/wal.cc` — Write-Ahead Log writer, reader, and crash recovery.
* `src/bloom.h` — Probabilistic bit arrays for disk lookups.
* `src/sstable.h` / `src/sstable.cc` — SSTable blocks serialization, indexing, and iteration.
* `src/manifest.h` / `src/manifest.cc` — Version catalog and atomic state changes.
* `src/compaction.h` / `src/compaction.cc` — Streaming multi-way merge-sort engine.
* `src/db.h` / `src/db.cc` — DB orchestrator and background scheduler.
* `src/cli.cc` — Database interactive shell.
* `src/benchmark.cc` — Latency benchmarking harness.
* `Makefile` — Build setup.

---

## Getting Started

### Prerequisites
A modern compiler supporting C++17 (`g++` or `clang++`) and `make` on a Unix/macOS environment.

### 1. Compile the Source
Build the interactive CLI and benchmark executables:
```bash
make
```

### 2. Run Tests
Execute the automated phase-by-phase test suites verifying all component behaviors:
```bash
make test
```

### 3. Run the Database Shell
Start the interactive database CLI (opens database at `db_data` by default):
```bash
./lsm_cli
```
Example interactions:
```text
lsm-db> put name "Prashant Tak"
OK
lsm-db> get name
Prashant Tak
lsm-db> stats
--- LSM DB Internals ---
MemTable Entries: 1
MemTable Memory Estimate: 87 bytes
--- LSM Levels statistics ---
Level 0: 0 files []
Level 1: 0 files []
Next File Number: 1
```

### 4. Run Concurrency Benchmarks
Simulate heavy write/read workloads across multiple threads:
```bash
./lsm_benchmark [threads] [ops_per_thread] [read_ratio_percent] [key_range]
```
Example running 4 worker threads performing 10,000 operations each (50% reads) across 15,000 keys:
```bash
./lsm_benchmark 4 10000 50 15000
```
Benchmark Results:
```text
==================================================
   LSM-Tree Key-Value Engine Performance Bench   
==================================================
Threads:       4
Ops/Thread:    10000
Read Ratio:    50%
Key Range:     15000 distinct keys

Benchmark Completed in 0.655 seconds.

Throughput Stats:
  Total Operations: 40000
  Writes:           19894
  Reads:            20106 (13410 not found)
  Throughput:       61068.70 ops/sec

Latency Stats (Microseconds):
  Average Latency:  64.2 us
  p50 (Median):     31 us
  p90:              192 us
  p95:              284 us
  p99 (Tail):       475 us
  p99.9 (Worst):    851 us
```

---

## Database Internals Highlighted

* **Durability (ACID)**: Uses raw POSIX file descriptor calls (`open`, `write`, `fsync`) to bypass OS write buffers and guarantee durability.
* **Tombstone Eviction**: Deletions are written as tombstones (`kTypeDeletion`) and are fully cleaned/evicted from the disk once they migrate to the bottom level (Level 1) during compaction.
* **Double Buffering**: Prevents write-stalling during memory-to-disk flushes by using an active MemTable alongside an immutable `frozen_memtable_` that is flushed in the background.
# LSM-Tree-Key-Value-Storage-Engine
