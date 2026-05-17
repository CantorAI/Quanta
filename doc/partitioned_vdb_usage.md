# PartitionedVdb — Usage Guide

A time-partitioned, tag-grouped vector database built on HNSW.  
Vectors are bucketed by timestamp granularity (hourly → yearly) and custom partition tags, enabling efficient scoped searches across massive datasets.

---

## Loading the Module

### Option 1: Direct XLang Import (standalone scripts)

```python
import xlang
quanta = xlang.importModule("quanta", fromPath="Quanta")
```

### Option 2: Via Cantor Service (recommended for production)

When Cantor is running, load the module through it. This enables **singleton sharing** — the module is loaded once and shared across all processes.

```python
import xlang

# Connect to Cantor via LRPC
cantor = xlang.importModule("cantor", thru="lrpc:1000")

# First process: Load the module into Cantor
quanta = cantor.LoadModule("quanta", "Quanta")

# Other processes: Query the already-loaded module (no reload, same instance)
quanta = cantor.QueryModule("quanta")
```

> **Why use Cantor?**  
> - **Singleton**: `LoadModule` loads the native library once; `QueryModule` returns the same instance — no duplicate memory  
> - **Multi-process safe**: Multiple Python processes share the same engine via LRPC  
> - **Service lifecycle**: Module stays loaded as long as Cantor runs, independent of any Python process

### Creating a VDB Instance

Once you have `quanta` (from either method above):

```python
import numpy as np
import time

# Create a new VDB
vdb = quanta.partitioned_vdb(
    prefix="my_vdb",           # file prefix for saved data
    path="./data/vectors",     # storage directory
    dim=512,                   # embedding dimension
    granularity="monthly"      # time partition: hourly|daily|weekly|monthly|yearly
)
```

---

## Core API

### `AddVectors(id, embedding, **kwargs)`

Add one or more vectors with metadata.

| Parameter | Type | Description |
|-----------|------|-------------|
| `id` | int or list | External ID(s) — single int for one vector, list for batch |
| `embedding` | numpy array | Float32 array, shape `(dim,)` for single or `(n*dim,)` for batch |
| `timestamp` | int (kwarg) | Millisecond timestamp (e.g. `int(time.time()*1000)`) |
| `partition` | str (kwarg) | Custom partition tag (**MANDATORY** for high-density IPCs, e.g. `"cam_01"`) |
| `chunks` | str or list (kwarg) | Text label(s) stored with each vector |
| `num_threads` | int (kwarg) | Thread count for HNSW insertion (-1 = auto) |

```python
now_ms = int(time.time() * 1000)

# Single vector
emb = np.random.randn(512).astype(np.float32)
emb /= np.linalg.norm(emb)
vdb.AddVectors(1, emb, timestamp=now_ms, partition="cam_01", chunks="frame_001.jpg")

# Batch: 3 vectors with sequential IDs starting at 100
batch = np.random.randn(3, 512).astype(np.float32)
batch = (batch / np.linalg.norm(batch, axis=1, keepdims=True)).flatten()
vdb.AddVectors(100, batch, timestamp=now_ms, partition="cam_02", chunks=["a.jpg","b.jpg","c.jpg"])
```

---

### `Lookup(query, topK, **kwargs)`

Search for the most similar vectors. Returns a list of 5-element tuples.

| Parameter | Type | Description |
|-----------|------|-------------|
| `query` | numpy array | Query embedding, shape `(dim,)` |
| `topK` | int | Max results to return |
| `partition` | str (kwarg) | Filter to a single partition tag |
| `partitions` | list (kwarg) | Filter to multiple partition tags |
| `ts_start` | int (kwarg) | Start of time range filter (ms, inclusive) |
| `ts_end` | int (kwarg) | End of time range filter (ms, inclusive) |
| `dedup` | float (kwarg) | Cosine similarity threshold for deduplication (0–1) |

**Returns**: list of `[id, score, chunk_text, partition_key, timestamp_ms]`

```python
query = np.random.randn(512).astype(np.float32)
query /= np.linalg.norm(query)

# Basic search
results = vdb.Lookup(query, 10)
for r in results:
    print(f"id={r[0]}, score={r[1]:.4f}, chunk={r[2]}, partition={r[3]}, ts={r[4]}")

# Filter by partition
results = vdb.Lookup(query, 10, partition="cam_01")

# Filter by multiple partitions
results = vdb.Lookup(query, 10, partitions=["cam_01", "cam_02"])

# Precise time range (milliseconds)
one_hour_ago = now_ms - 3600 * 1000
results = vdb.Lookup(query, 10, ts_start=one_hour_ago, ts_end=now_ms)

# Combined: partition + time range
results = vdb.Lookup(query, 10, partition="cam_01", ts_start=one_hour_ago, ts_end=now_ms)

# Deduplication: remove results with cosine sim > 0.95
results = vdb.Lookup(query, 10, dedup=0.95)
```

---

### `Close()` / `Load(path)`

Manage database metadata and config safely. Quanta is fully autonomous when running — vectors are **lazy-loaded** into RAM only when explicitly searched. The Background Maintenance Thread automatically saves active buffers to disk every `auto_save_seconds`.

```python
# Gracefully signal the background thread to safely flush all remaining unwritten data to disk before process death
vdb.Close()

# Load into a new instance (Config+Tags only — vectors are NOT immediately loaded to RAM)
vdb2 = quanta.partitioned_vdb(prefix="my_vdb", path="./data/vectors", dim=512)
vdb2.Load("./data/vectors")

# Now vdb2 has all the config. When queried, it will lazy-load the required partition from disk.
results = vdb2.Lookup(query, 5)
```

**Files created on disk:**
```
./data/vectors/
├── my_vdb_manifest.db           # SQLite: config + partition registry
├── my_vdb_2026-03_0.hnsw        # HNSW index (time=2026-03, partition=default)
├── my_vdb_2026-03_0.vdb         # Labels + chunks + timestamps
├── my_vdb_2026-03_1.hnsw        # HNSW index (time=2026-03, partition=cam_01)
└── my_vdb_2026-03_1.vdb         # Labels + chunks + timestamps
```

---

### `ListPartitions()`

List all partitions (on-disk and in-memory).

```python
for p in vdb.ListPartitions():
    print(f"key={p['key']}, ts={p['ts_partition']}, tags={p['tags']}, loaded={p['loaded']}")
```

---

### `AddPartitionTag(index, tag)` / `GetPartitionInfo(tag)`

Manage custom partition tags.

```python
# Add an alias tag to partition index 1
vdb.AddPartitionTag(1, "entrance_camera")

# Query partition info
info = vdb.GetPartitionInfo("cam_01")
print(f"index={info['index']}, tags={info['tags']}")
```

---

### `QueryLabelByID(id, **kwargs)`

Look up the stored chunk text for a specific vector ID.

```python
# Search all partitions
label = vdb.QueryLabelByID(42)

# Narrow search to a specific partition + timestamp
label = vdb.QueryLabelByID(42, partition="cam_01", timestamp=now_ms)
```

---

### `Grouping(items, threshold, **kwargs)`

Cluster vectors by centroid similarity. Input is a list of dicts with IDs.

```python
items = [
    {"image_id": 1, "timestamp": ts1, "device_id": "cam_01"},
    {"image_id": 2, "timestamp": ts2, "device_id": "cam_01"},
    {"image_id": 3, "timestamp": ts3, "device_id": "cam_02"},
]

# Group items with cosine similarity >= 0.85
groups = vdb.Grouping(items, 0.85,
    id_key="image_id",
    partition_key="device_id",
    timestamp_key="timestamp"
)
# Returns: [[1, 2], [3]]  — each sublist is a group of similar IDs
```

---

## Singleton Access via Cantor LRPC

For multi-process deployments, load the module once via Cantor and share it:

```python
import xlang

# Process 1: Load the module
cantor = xlang.importModule("cantor", thru="lrpc:1000")
quanta = cantor.LoadModule("quanta", "Quanta")
vdb = quanta.partitioned_vdb(prefix="shared", path="/data/vdb", dim=512)

# Process 2+: Query the already-loaded module (singleton)
cantor = xlang.importModule("cantor", thru="lrpc:1000")
quanta = cantor.QueryModule("quanta")  # same instance as Process 1
vdb = quanta.partitioned_vdb(prefix="shared", path="/data/vdb", dim=512)
vdb.Load("/data/vdb")
```

---

## CLIP Image Embedding Example

```python
import torch, clip
from PIL import Image

device = "cuda" if torch.cuda.is_available() else "cpu"
model, preprocess = clip.load("ViT-B/32", device=device)

def image_to_embedding(image_path):
    img = Image.open(image_path).convert("RGB")
    img_input = preprocess(img).unsqueeze(0).to(device)
    with torch.no_grad():
        emb = model.encode_image(img_input)
        emb = emb / emb.norm(dim=-1, keepdim=True)
    return emb.cpu().numpy().flatten().astype(np.float32)

def text_to_embedding(text):
    text_input = clip.tokenize([text]).to(device)
    with torch.no_grad():
        emb = model.encode_text(text_input)
        emb = emb / emb.norm(dim=-1, keepdim=True)
    return emb.cpu().numpy().flatten().astype(np.float32)

# Index images
vdb = quanta.partitioned_vdb(prefix="images", path="./image_vdb", dim=512)
for i, path in enumerate(image_paths):
    emb = image_to_embedding(path)
    vdb.AddVectors(i, emb,
        timestamp=int(os.path.getmtime(path) * 1000),
        chunks=os.path.basename(path)
    )
vdb.Close()

# Search by text
results = vdb.Lookup(text_to_embedding("a red car"), 5)

# Search by image similarity
results = vdb.Lookup(image_to_embedding("query.jpg"), 5)

# Search within last 24 hours only
now = int(time.time() * 1000)
results = vdb.Lookup(text_to_embedding("person"), 5,
    ts_start=now - 86400000, ts_end=now)
```

---

## Constructor Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `prefix` | `"vdb"` | Filename prefix for all persisted files |
| `path` | `"."` | Base directory for storage |
| `dim` / `dimension` | `512` | Embedding vector dimension |
| `granularity` | `"hourly"` | Time bucketing: `hourly`, `daily`, `weekly`, `monthly`, `yearly` |
| `space` | `"l2"` | Distance metric: `l2`, `ip` (inner product), `cosine` |
| `max_memory_gb` | `1.0` | Peak memory allowance per partition graph |
| `max_loaded_read_only_partitions` | `50` | LRU Eviction Quota: Maximum historical query graphs allowed in memory |
| `ttl_minutes` | `3` | Idle time before a partition is autonomously offloaded from RAM |
| `auto_save_seconds`| `300` | Autonomous background thread save interval (seconds) |
| `M` | `16` | HNSW graph connectivity |
| `ef_construction` | `200` | HNSW build-time search depth |
| `ef_search` | `50` | HNSW query-time search depth |

---

## 🧠 Memory Management (Autonomous Safeties)

Quanta `PartitionedVdb` implements extreme Memory Swapping safeguards allowing standard servers to securely ingest thousands of vectors per second globally without risking RAM crashes.

### Tier 3 Byte Spilling (The `max_memory_gb` Limit)
The `max_memory_gb` parameter creates a strict capacity ceiling. When the user ingests thousands of frames, Quanta translates the user's byte limit dynamically based on the dataset's embedding dimensions. 
If an active bucket crosses this byte limit, Quanta automatically seals `_0000.hnsw` completely autonomously, saves the bounding SQLite timestamp map to `vision_manifest.db`, and opens `_0001.hnsw`. 
- **User Impact:** Even 100 parallel IPC cameras logging huge traffic spikes are structurally protected from ever overloading server memory allocations natively.

### Auto-Cleanup (TTL) and LRU Quotas
- **Time-To-Live (TTL)**: Actively writing buckets are instantly cleared if idle without queries for `ttl_minutes` (default: 3 minutes), strictly capping peak ingress RAM to immediate camera hours.
- **LRU Read-Only Eviction**: If a cross-month global forensic search forces hundreds of historical graph hydrations, they are strictly throttled by the `max_loaded_read_only_partitions` sandbox (Default: 50 limit). The engine enforces LRU (Least-Recently-Used) swapping upon historical files synchronously with disk loads, securing the engine against query spikes.
- **Dirty Partitions**: You never call `vdb.Save()`. Every `auto_save_seconds`, a C++ thread pushes pending ingestion data seamlessly. Call `vdb.Close()` for safe shutdown.

---

## Time-Range Filtering: How It Works

Filtering operates at **two levels** for performance:

1. **Partition pre-filter** — Entire time partitions (e.g. months) outside the range are skipped entirely. Zero cost for excluded partitions.
2. **Per-record post-filter** — Within matching partitions, each result's stored millisecond timestamp is checked against `[ts_start, ts_end]`. Only exact matches are returned.

This two-level approach gives you both the efficiency of coarse partitioning and the precision of exact millisecond filtering.
