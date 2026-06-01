# Quanta: OOM-Resistant Streaming Vector Database

**Quanta** is a high-performance, native C++ vector database engine built on top of Hnswlib. Unlike traditional vector databases designed primarily for static textual RAG, Quanta is specifically engineered for **high-density streaming data ingestion** (e.g., thousands of continuous IP camera streams, high-frequency IoT telemetry) where memory safety and OOM (Out-of-Memory) resistance are critical.

## 🚀 Core Features

- **Three-Tier Partitioning (Time + Tag + RAM Limit):** Shards vectors not just by time and logical tags, but dynamically strictly enforces a physical RAM byte limit (`max_memory_gb`) to guarantee your edge nodes never crash from sudden data bursts.
- **Autonomous Memory Defense:** 
  - **TTL Eviction:** Automatically identifies idle partitions and flushes them from RAM to disk.
  - **LRU Query Swapping:** Protects the system from massive historical queries (e.g., searching 3 months of data) by throttling loaded read-only partitions, exchanging disk I/O for 100% memory safety.
- **WAL Async Merging Pipeline:** High-throughput synchronous fast-path writing with background OpenMP-parallelized graph compilation. Ensures zero data loss without blocking active streams.
- **GIL-Bypassing Singleton:** Deeply integrated via XLang, allowing dozens of parallel Python worker pipelines to share a single C++ native memory space.

📖 **For an in-depth understanding of the internal design, please refer to the [Architecture Documentation](doc/quanta_vdb_architecture.md).**

---

## 🛠 Compilation & Build

Quanta is a C++ native extension that depends on the `xlang` engine. 

For complete build instructions (including cloning dependencies and running `build.bat` / `build.sh`), please see:
👉 **[doc/BUILD.md](doc/BUILD.md)**

---

## ⚡ Quick Start Example

Quanta runs effortlessly inside Python via the XLang interop layer, entirely bypassing the GIL for native speed.

```python
import time
import numpy as np
import xlang

# 1. Load the native Quanta C++ module into the XLang engine
quanta = xlang.importModule("quanta", fromPath="Quanta")

# 2. Initialize the partitioned VDB with temporal sharding
vdb = quanta.partitioned_vdb(
    prefix="vision",
    path="./my_vectors",
    dim=512,
    granularity="monthly"
)

# 3. Insert a vector (automatically placed into the correct time-tag partition)
emb = np.random.randn(512).astype(np.float32)
emb = emb / np.linalg.norm(emb) # Normalize
now_ms = int(time.time() * 1000)

vdb.AddVectors(1001, emb, timestamp=now_ms, partition="camera_01", chunks="metadata")

# 4. Search isolated to a specific time range and tag
ts_start = now_ms - (15 * 24 * 3600 * 1000) # Last 15 days
results = vdb.Lookup(emb, 3, partition="camera_01", ts_start=ts_start, ts_end=now_ms)

for r in results:
    # returns: (id, similarity_score, metadata_chunk, partition_key)
    print(f"Match ID: {r[0]}, Score: {r[1]:.4f}")
```

## 📄 License
This project is open-sourced under the [Apache License, Version 2.0](LICENSE).
For more details, please refer to the included `LICENSE` and `NOTICE` files.
