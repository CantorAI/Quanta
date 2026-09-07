import sys
import os
import time
import shutil
import sqlite3
import numpy as np

xlang_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)

import xlang3
quanta = xlang3.importModule("quanta", fromPath="Quanta")

def run_tests():
    vdb_path = os.path.join(os.path.dirname(__file__), "data", "vdb_cross_bucket")
    if os.path.exists(vdb_path):
        shutil.rmtree(vdb_path, ignore_errors=True)
    os.makedirs(vdb_path, exist_ok=True)
    
    print(f"[{time.strftime('%H:%M:%S')}] Writing vectors to force bucket spilling (Tier 3)...")

    # Step 1: Write data to force multiple buckets
    vdb = quanta.partitioned_vdb(
        prefix="cbc",
        path=vdb_path,
        dimension=4, 
        max_memory_gb=0.000001,  # ultra low to force cascading
        granularity="hourly",
        max_loaded_read_only_partitions=1 # Extremely tight LRU to force unloads
    )
    
    base_ts = int(time.time() * 1000)
    
    for b in range(5):
        vecs = []
        for i in range(1000):
            # The first float in the vector identifies the batch easily
            vecs.append([float(b), 1.0, 0.0, float(i)/1000.0])
        vnp = np.array(vecs, dtype=np.float32)
        ts = base_ts + (b * 10000)
        
        vdb.AddVectors(b*1000, vnp, chunks=[f"chunk_{b}_{i}"]*1000, timestamp=ts, partition="CAM_1")
        print(f"  -> Inserted Batch {b} (1000 vectors) at TS: {ts}")

    print(f"\n[{time.strftime('%H:%M:%S')}] Sealing and closing database...")
    vdb.Close()
    
    # Verify Manifest
    print(f"\n[{time.strftime('%H:%M:%S')}] Validating SQLite Manifest for Buckets:")
    conn = sqlite3.connect(os.path.join(vdb_path, "cbc_manifest.db"))
    c = conn.cursor()
    c.execute("SELECT key, bucket_number, ts_start, ts_end FROM buckets ORDER BY bucket_number")
    rows = c.fetchall()
    for r in rows:
        print(f"  -> Created Bucket: {r[0]} | TS Bounds: {r[2]} to {r[3]}")
    conn.close()
    
    assert len(rows) >= 4, "Failed to cascade buckets correctly."
    
    # Step 2: Querying constraints and memory hydration assertions
    print(f"\n[{time.strftime('%H:%M:%S')}] Re-initializing engine in read-only hydration mode...")
    
    vdb_read = quanta.partitioned_vdb(
        prefix="cbc",
        path=vdb_path,
        dimension=4,
        max_memory_gb=0.000001,
        max_loaded_read_only_partitions=1 # Only 1 bucket allowed in RAM at a time!
    )
    
    def print_loaded_buckets(vdb_instance):
        parts = vdb_instance.ListPartitions()
        loaded = [p['key'] for p in parts if p.get('loaded', False)]
        print(f"  Currently Loaded Buckets in RAM: {loaded}")
        return loaded
        
    print_loaded_buckets(vdb_read) # Should be empty
    
    # Query for Batch 0
    print(f"\n[{time.strftime('%H:%M:%S')}] Testing Query for Batch 0 (Targeting TS: {base_ts})...")
    q0 = np.array([0.0, 1.0, 0.0, 0.0], dtype=np.float32)
    res0 = vdb_read.Lookup(q0, 5, ts_start=base_ts-1000, ts_end=base_ts+1000)
    print(f"  Query Results: {[(r[0], r[1]) for r in res0]}")
    
    loaded_0 = print_loaded_buckets(vdb_read)
    assert any("_0000" in key for key in loaded_0), "Did not hydrate Bucket 0000!"
    
    # Query for Batch 4 (Far future)
    ts_batch_4 = base_ts + 40000
    print(f"\n[{time.strftime('%H:%M:%S')}] Testing Query for Batch 4 (Targeting TS: {ts_batch_4})...")
    q4 = np.array([4.0, 1.0, 0.0, 0.0], dtype=np.float32)
    res4 = vdb_read.Lookup(q4, 5, ts_start=ts_batch_4-1000, ts_end=ts_batch_4+1000)
    print(f"  Query Results: {[(r[0], r[1]) for r in res4]}")
    
    loaded_4 = print_loaded_buckets(vdb_read)
    assert not any("_0000" in key for key in loaded_4), "LRU Eviction failed! Bucket 0000 is still in memory!"
    assert any("_0004" in key for key in loaded_4), "Did not hydrate Bucket 0004!"
    
    print(f"\n[{time.strftime('%H:%M:%S')}] Success! Engine mathematically proved independent load/unload bucket boundaries.")
    vdb_read.Close()

if __name__ == "__main__":
    run_tests()
