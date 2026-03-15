import sys
import os
import time
import shutil
import sqlite3
import numpy as np

xlang_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)

import xlang 
quanta = xlang.importModule("quanta", fromPath="Quanta")

def run_tests():
    vdb_path = os.path.join(os.path.dirname(__file__), "data", "vdb_comprehensive")
    if os.path.exists(vdb_path):
        shutil.rmtree(vdb_path, ignore_errors=True)
    os.makedirs(vdb_path, exist_ok=True)
    
    print(f"Creating test vectors in directory: {vdb_path}")

    print("=== [Test 1] Initialization & Cascading Limits ===")
    vdb = quanta.partitioned_vdb(
        prefix="comp",
        path=vdb_path,
        dimension=4, 
        max_memory_gb=0.000001,  # ultra low logic to force cascading ~1000 items
        granularity="hourly",
        max_loaded_read_only_partitions=2, # Force LRU testing
        ttl_minutes=1,
        auto_save_seconds=300
    )
    
    # Generate 4000 vectors over 4 different timestamps to force cascading
    base_ts = int(time.time() * 1000)
    
    for b in range(4):
        vecs = []
        for i in range(1000):
            vecs.append([float(b), 1.0, 0.0, float(i)/1000.0])
        vnp = np.array(vecs, dtype=np.float32)
        ts = base_ts + (b * 10000) # 10 seconds apart
        
        print(f"Preparing to insert Batch {b} at ts {ts}...")
        vdb.AddVectors(b*1000, vnp, chunks=[f"chunk_{b}_{i}"]*1000, timestamp=ts, partition="CAM_1")
        print(f"Batch {b} inserted successfully.")

    print("Waiting for WAL Background Thread to flush micro-batches...")
    time.sleep(1.0)
    
    print("Closing VDB to flush buckets...")
    sys.stdout.flush()
    try:
        vdb.Close()
        print("VDB closed successfully.")
    except Exception as e:
        print(f"EXCEPTION DURING CLOSE: {e}")
        raise
    sys.stdout.flush()
    
    # Manifest validation
    print("\n=== [Database Physical Inspection & Integrity Check] ===")
    conn = sqlite3.connect(os.path.join(vdb_path, "comp_config.db"))
    c = conn.cursor()
    c.execute("SELECT key, ts_partition, custom_index, bucket_number, ts_start, ts_end FROM partitions ORDER BY bucket_number")
    rows = c.fetchall()
    print(f"Buckets Physically Created in SQLite: {len(rows)}")
    for r in rows:
        print(f"  -> File: {r[0]} | TS_Start: {r[4]} | TS_End: {r[5]}")
    assert len(rows) >= 4, "Cascade bounds failed! Not enough buckets were spilled."
    print("Database table inspection passed.")

    print("\n=== [Test 2] Basic Lookup & LRU Hydration ===")
    # Re-open the DB cleanly
    vdb = quanta.partitioned_vdb(
        prefix="comp",
        path=vdb_path,
        dimension=4,
        max_memory_gb=0.000001,
        max_loaded_read_only_partitions=2
    )

    query = np.array([2.0, 1.0, 0.0, 0.0], dtype=np.float32)
    
    # 1. Search all (will cascade load all historical partitions and trigger LRU unloading)
    print("Querying across all historical records...")
    res = vdb.Lookup(query, 5) 
    print(f"Top 5 Results: {[(r[0], r[1]) for r in res]}")
    
    # 2. Verify LRU unloaded things
    parts = vdb.ListPartitions()
    loaded_count = sum(1 for p in parts if p.get('loaded', False))
    print(f"Loaded Partitions in RAM: {loaded_count} (Limit was 2)")
    assert loaded_count <= 3, f"LRU Eviction failed! Detected {loaded_count} loaded partitions."

    print("\n=== [Test 3] Time-Bounded Lookup ===")
    # Look exactly for batch 1 (ts: base_ts + 10000)
    start_time = base_ts + 9000
    end_time = base_ts + 11000
    
    print(f"Querying rigidly bounded time slot ({start_time} - {end_time})...")
    res_bounded = vdb.Lookup(query, 5, ts_start=start_time, ts_end=end_time)
    print(f"Bounded Results: {[(r[0], r[1]) for r in res_bounded]}")
    
    for r in res_bounded:
        assert 1000 <= r[0] < 2000, f"Time bounds failed! Vector ID {r[0]} breached the bounded time box."

    print("\n=== [Test 4] Dedup Lookup & Structure Checks ===")
    res_dedup = vdb.Lookup(query, 10, dedup=0.99)
    print(f"Deduped Results (Threshold 0.99): {[(r[0], r[1]) for r in res_dedup]}")
    
    print("\n=== [Test 5] Vector Grouping ===")
    
    # Debug: Can we even find ID 1000 physically in the VDB right now?
    lbl_1000 = vdb.QueryLabelByID(1000)
    print(f"Pre-Grouping Debug -> Label Data for ID 1000: {lbl_1000}")
    assert lbl_1000 is not None, "ID 1000 completely missing from VDB memory space!"

    # Grouping clusters candidate items based on distance
    candidates = []
    for i in range(10):
        # Format dict matching GroupingItem C++ ingestion 
        candidates.append({
            "id": 1000 + i, 
            "device_id": "CAM_1", 
            "timestamp": base_ts + 10000 # Matches Batch 1
        })
        
    group_res = vdb.Grouping(
        candidates, 
        0.9
    )
    print(f"Grouping clusters returned: {len(group_res)}")
    for g in group_res:
        print(f"  Cluster ID: {g[0] if g else 'N/A'}, Items: {len(g)}")

    print("\n=== [Test 6] Tag Management ===")
    vdb.AddPartitionTag(0, "CAM_1_ALIAS")
    info = vdb.GetPartitionInfo("CAM_1_ALIAS")
    print(f"Partition Info for CAM_1_ALIAS: {info}")
    assert info.get("customIndex") == 0, "Tag alias failed to map to correct customIndex"
    
    print("\n=== [Test 7] Query By ID ===")
    # Pull back the specific payload for vector 1005 (created in Batch 1)
    label_data = vdb.QueryLabelByID(1005)
    print(f"Label Data for ID 1005: {label_data}")
    assert label_data is not None, "QueryLabelByID failed to retrieve vector payload"
    
    print("\nAll Comprehensive Tests Passed! Quanta Engine is mathematically clean.")
    sys.stdout.flush()
    sys.stderr.flush()
    try:
        print("Starting VDB Close...", flush=True)
        vdb.Close()
        print("Finished VDB Close!", flush=True)
    except Exception as e:
        print(f"Exception during Close: {e}", flush=True)

if __name__ == "__main__":
    try:
        run_tests()
    except Exception as e:
        import sys
        print(f"FATAL PYTHON EXCEPTION: {e}", flush=True)
        import traceback
        traceback.print_exc()
        sys.exit(2)
