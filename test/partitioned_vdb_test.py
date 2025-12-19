import os
import time
import numpy as np

import xlang
quanta = xlang.importModule("quanta", fromPath="Quanta")

# Setup
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VDB_PATH = os.path.join(SCRIPT_DIR, "test_pvdb")
DIM = 512

# Create VDB
vdb = quanta.partitioned_vdb(
    prefix="test",
    path=VDB_PATH,
    dim=DIM,
    granularity="monthly"
)
print(f"VDB path: {VDB_PATH}")

# Helper
def random_emb():
    e = np.random.randn(DIM).astype(np.float32)
    return e / np.linalg.norm(e)

# Timestamps (milliseconds)
now_ms = int(time.time() * 1000)
last_month_ms = now_ms - 30 * 24 * 3600 * 1000

# Add vectors
emb1 = random_emb()
emb2 = random_emb()
emb3 = random_emb()

vdb.AddVectors(1, emb1, timestamp=now_ms, partition="default", chunks="doc1")
vdb.AddVectors(2, emb2, timestamp=now_ms, partition="region_us", chunks="doc2")
vdb.AddVectors(3, emb3, timestamp=last_month_ms, partition="region_us", chunks="doc3")
print("Added 3 vectors")

# Save
vdb.Save("")
print("Saved")

# List partitions
print("\nPartitions:")
for p in vdb.ListPartitions():
    print(f"  {p}")

# Query all
print("\nQuery all:")
for r in vdb.Lookup(emb1, 3):
    print(f"  id={r[0]}, score={r[1]:.4f}, chunk={r[2]}, key={r[3]}")

# Query by partition
print("\nQuery region_us only:")
for r in vdb.Lookup(emb1, 3, partition="region_us"):
    print(f"  id={r[0]}, score={r[1]:.4f}, chunk={r[2]}, key={r[3]}")

# Query by time range
print("\nQuery last 15 days:")
ts_start = now_ms - 15 * 24 * 3600 * 1000
for r in vdb.Lookup(emb1, 3, ts_start=ts_start, ts_end=now_ms):
    print(f"  id={r[0]}, score={r[1]:.4f}, chunk={r[2]}, key={r[3]}")

print("\nDone.")