# Quanta PartitionedVdb: High-Density Architecture Design

## 1. Overview
The `PartitionedVdb` is a native C++ vector database built on `Hnswlib`, designed to ingest, isolate, and index limitless streams of high-dimensional vectors (e.g., streaming IPC video pipelines). 

It guarantees instant horizontal read/write scale without exhausting system RAM by employing a **Three-Tier Hybrid Partitioning Architecture** and a self-governing **Autonomous TTL Cache**.

---

## 2. Core Operational Paradigms

### A. The "Singleton" Service Lifecycle
Quanta is deeply integrated with the CantorAI service backend. It is designed to be loaded once into Cantor via `xlang.importModule("cantor").LoadModule(...)`. 
Subsequent Python agents (like Vision tracking nodes) simply execute `QueryModule("quanta")`. This ensures that 32 parallel worker pipelines all share a single native memory space and utilize the exact same C++ locking primitives, completely bypassing the Python Global Interpreter Lock (GIL) limits.

### B. Late-Binding (Lazy Loading)
When Quanta is instantiated, it **never** loads `.hnsw` payload files into RAM immediately. It only boots the SQLite Manifest database to retrieve configuration states and label metadata. 
A vector shard is only physically loaded into RAM the millisecond its exact historical timeframe or camera tag is explicitly requested.

---

## 3. The Three-Tier Partitioning Architecture

If a vector database attempts to hold an infinite history in a single tree, it will trigger an Out-Of-Memory (OOM) crash. Quanta eliminates this mathematical boundary by dynamically routing data across three distinct dimensional planes.

### Tier 1: Time Granularity Partitioning (`YYYY-MM`)
By default, Quanta shards vectors into isolated time buckets based on its `granularity` setting (`hourly`, `daily`, `monthly`). 
- **Advantage**: If a pipeline requests to view "the last 3 hours" of data, Quanta instantly ignores 99% of the database payloads on disk, isolating the load to just the exact relevant chronologies.

### Tier 2: Custom Index Partitioning (Tagging)
Time alone is insufficient for high-density ingress (e.g., 32 independent cameras creating 1,600 vectors per second natively). A single "Hourly" bucket under this density demands ~34 GB of RAM, causing edge nodes to crash.
- **The Solution**: Quanta exposes a `partition="tag"` kwarg in `AddVectors()`. The system maps this string tag (e.g., `"cam1"`) to a permanent integer (`customIndex=1`). 
- **Advantage**: It totally isolates the video cameras from each other mathematically. `cam1` gets its own private tree, and `cam2` gets its own private tree. Cross-query speed is dramatically boosted since unrelated device vectors are never merged into shared graphs.

### Tier 3: Memory Size Capacity Spilling (The "Bucket Limit")
Even with Time and Device partitioning, unexpected vector surges could still breach hardware limits before the hour closes.
- **The Solution (RAM Sizing)**: We grant the user an initialization parameter: `max_memory_gb` (e.g., 1.5 GB). Since embedding dimensions (e.g., 512 vs 1024) drastically alter vector byte footprints, relying purely on vector "counts" is unsafe. The engine dynamically calculates bytes-used to enforce physical RAM safety.
  - *Example Setup:* A user with 60 IPCs running on a 64GB node configures `max_memory_gb=0.9`. The 60 active writing streams mathematically cap out at exactly 54GB of RAM (safely under the 64GB hardware limit) regardless of vector dimension.
- **Implementation**: The physical file suffix becomes `_[customIndex]_[bucket_number].hnsw` (e.g., `_1_0000.hnsw`). When the active RAM partition for a camera hits the `max_memory_gb` byte ceiling, Quanta seals `_1_0000.hnsw` and cascades into an empty `_1_0001.hnsw` graph.
- **The Bucket Metadata Manifest**: Because a single month (e.g., `2024-03`) might now contain 50 split buckets (`_0000` to `_0049`), we utilize the SQLite `manifest.db` to record the exact bounding times of each bucket container dynamically.

#### Database Manifest Example
| key (Partition) | ts_start (ms) | ts_end (ms) |
|-----------------|--------------|------------|
| 2024-03_1_0000 | 1773000000 | 1773050000 |
| 2024-03_1_0001 | 1773050001 | 1773090000 |

- **Filtering Advantage**: When `Lookup()` receives a query with `ts_start` constraints, Quanta queries the SQLite manifest first. If the requested time bypasses `_0000`, the database mathematically skips loading `_0000.hnsw` entirely, routing queries instantly to `_0001.hnsw` with zero RAM waste. Peak memory limits remain 100% mathematically stable irrespective of volume.

---

## 4. Write-Ahead Log (WAL) Async Merging Pipeline
To prevent data loss during high-density video ingress while ensuring instantaneous `Lookup()` performance, `PartitionedVdb` employs an isolated Write-Ahead Log queue.

### A. The Synchronous Fast-Path
When `AddVectors()` executes, incoming floats are inserted into the live RAM structures but physically bypassed from synchronous graph compilation. Instead, the raw vectors are binary-appended to a microscopic `[bucket_prefix].wal_[timestamp]` file.
- **Aggressive Rotation**: After an extremely small threshold (e.g., 100 vectors), the active `.wal` is sealed, and its filename is dispatched to an **In-Memory Thread Queue**. The system immediately spawns a new chronologically-named file for the next write.

### B. Parallelized Background Graph Compilation
The Quanta maintenance thread actively monitors the in-memory array for mature `.wal` filenames. 
1. When a `.wal` matures, the thread pops the file, parses the records, and structurally builds the `.hnsw` graph arrays in the background.
2. **Multi-Threading**: This build process takes advantage of parallel multiprocessing (`omp` / `num_threads`), drastically reducing serialization time.
3. Once constructed, the `.hnsw` and `.vdb` layers are written via secure atomic `.tmp` renames, and the consumed `.wal` is permanently deleted.

### C. Boot-Time Crash Recovery
If a hard crash obliterates the RAM queues, `Init()` scans the directory, locates orphaned `.wal_*` micro-batches, strictly orders them by timestamp, and forces structural hydration before the IPC pipelines are permitted to open.

---

## 5. The Autonomous Maintenance Thread (Memory Defense)
Because Quanta operates globally over the lifetime of the server, it features an embedded C++ `std::thread` that loops continuously every 5 seconds, acting as its immune system.

### A. Auto-Save (The Dirty Buffer)
As the API receives millions of vectors, `PartitionedVdb` writes them exclusively into RAM for maximum throughput. If a partition's `is_dirty_` flag is flipped, the background thread calculates its age. If it exceeds `auto_save_seconds_` (default: 5 minutes), the thread safely pushes the vectors to solid-state disk autonomously.

### B. Time-To-Live Memory Eviction (TTL)
While `Lazy Loading` defends against OOM crashes during boot sequences, the TTL logic defends against OOM crashes during sustained uptime operations.
- Every `Partition` continuously tracks its `last_access_ms`. 
- If a camera stream writes heavily for an hour, but is then paused, the background thread will notice that the `idle_time_ms` has surpassed `ttl_minutes_` (default: 60 minutes).
- The thread will instantly lock the index, confirm it is flushed to disk, and execute `partitions_.erase()`, surgically nuking the 1GB payload from system RAM. 

---

## 6. Query Scalability: Overload RAM Throttling (Read-Only Swapping)
The TTL Cache perfectly protects the database during standard write operations by dumping stale data past 1 hour. But it does **NOT** protect the system from massive query attacks.

If a single API request asks to search "the last 3 months" of data across 32 cameras, Quanta would be forced to rapidly load **thousands** of `.hnsw` files into RAM simultaneously to serve the concurrent search. This would instantly shatter the 64GB RAM limits, crashing the server entirely.

### The Solution: The Read-Only RAM Quota (LRU Eviction)
To defeat Query-Spike OOMs without killing wide-scale deployments, Quanta introduces a strict **Maximum Read-Only Partitions Quota** (`max_loaded_read_only_partitions_`), fundamentally turning the historical query engine into an LRU (Least-Recently-Used) swapper.

1. **Active vs. Historical**: Buckets currently receiving `AddVectors()` streams (e.g., the current hour of 60 live IPCs) are **immune** to the quota. They stay in RAM permanently until the TTL detects they are idle.
2. **Configurable Memory Sandbox**: The user sets a hard limit exclusively for historical searches (e.g., `max_loaded_read_only_partitions=50`) via the initialization kwargs.
3. **Read-Only Sweeping**: When `Lookup()` receives a massive 3-month query, the HNSW engine checks the total *historical* partition count before opening `month-1_cam-1.hnsw`.
4. **Synchronous Unloading**: If opening the next queried historical chunk would exceed `max_loaded_read_only_partitions_`, Quanta synchronously pauses the query thread and forcibly unloads the oldest historical bucket currently in RAM before executing the load. Active camera write-buckets are never touched.
5. **Result**: A 3-month global query executes safely within the configured 50-bucket RAM limit. The `Lookup()` takes longer to respond (due to disk I/O thrashing to swap partitions in and out), but the entire Quanta system remains 100% memory stable, defending the active video-ingestion writes from being disrupted by heavy searches.

---

## 7. Summary of Physical Disk Artifacts
Under the finalized `Prefix_Time_Index_Bucket` structure, a standard write will produce the following synchronized binaries on solid-state media:

```text
/data/vectors/
├── vision_manifest.db                # SQLite: Handles Config and Tag-to-Index Maps
├── vision_2026-03_1_0000.hnsw        # Mmap: The core graph arrays and connections
└── vision_2026-03_1_0000.vdb         # Custom: Embedded labels, timestamp keys, bytes
```
