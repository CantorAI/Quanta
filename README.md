# Quanta: OOM-Resistant Streaming Vector Database

**Quanta** is a high-performance, native C++ vector database engine built on top of Hnswlib. Unlike traditional vector databases designed primarily for static textual RAG, Quanta is specifically engineered for **high-density streaming data ingestion** (e.g., thousands of continuous IP camera streams, high-frequency IoT telemetry) where memory safety and OOM (Out-of-Memory) resistance are critical.

## 🚀 Core Features

- **Three-Tier Partitioning (Time + Tag + RAM Limit):** Shards vectors not just by time and logical tags, but dynamically strictly enforces a physical RAM byte limit (`max_memory_gb`) to guarantee your edge nodes never crash from sudden data bursts.
- **Autonomous Memory Defense:** 
  - **TTL Eviction:** Automatically identifies idle partitions and flushes them from RAM to disk.
  - **LRU Query Swapping:** Protects the system from massive historical queries (e.g., searching 3 months of data) by throttling loaded read-only partitions, exchanging disk I/O for 100% memory safety.
- **WAL Async Merging Pipeline:** Owned insertion queues and background graph construction. Explicit close drains insertion; background failures are exposed through `GetHealth()["error"]`.
- **XLang3 Native C++ Package:** Uses the public C++ SDK over the C ABI, with database caches isolated per runtime and optional CPython bridge access.

📖 **For an in-depth understanding of the internal design, please refer to the [Architecture Documentation](doc/quanta_vdb_architecture.md).**

---

## 🛠 Compilation & Build

Quanta is a C++ native extension that depends on `xlang3/sdk` and `xlang3_runtime`.

Build from the CantorAI workspace using Visual Studio 2026 and Release configuration. See:
👉 **[doc/BUILD.md](doc/BUILD.md)**

---

## ⚡ Quick Start Example

Run this Python code with `xlang3.exe`. CPython callers can instead use
`import xlang3` and `xlang3.importModule("quanta", fromPath="Quanta")`.

```python
import time
import tensor as T

# 1. Load the native Quanta C++ module
from Quanta import quanta

# 2. Initialize the partitioned VDB with temporal sharding
vdb = quanta.partitioned_vdb(
    prefix="vision",
    path="./my_vectors",
    dim=512,
    granularity="monthly",
    wal_cooling_time_seconds=1
)

# 3. Insert a vector (automatically placed into the correct time-tag partition)
emb = T.tensor([1.] + [0.] * 511, dtype=T.float32)
now_ms = int(time.time() * 1000)

vdb.AddVectors(1001, emb, timestamp=now_ms, partition="camera_01", chunks="metadata")

# 4. Search isolated to a specific time range and tag
ts_start = now_ms - (15 * 24 * 3600 * 1000) # Last 15 days
deadline = time.time() + 15
results = []
while not results and time.time() < deadline:
    if vdb.GetHealth()["error"]:
        raise RuntimeError(vdb.GetHealth()["error"])
    results = vdb.Lookup(emb, 3, partition="camera_01", ts_start=ts_start, ts_end=now_ms)
    if not results:
        time.sleep(.1)

for r in results:
    # Each row: [id, similarity_score, metadata_chunk, partition_key, timestamp_ms]
    print(f"Match ID: {r[0]}, Score: {r[1]:.4f}")
vdb.Close()
```

See [native tests](test/native/README.md) for SDK, tensor, CPython bridge,
concurrency, persistence, and shutdown coverage. Existing `.x` scripts are unchanged.

## 📄 License
This project is open-sourced under the [Apache License, Version 2.0](LICENSE).
For more details, please refer to the included `LICENSE` and `NOTICE` files.
