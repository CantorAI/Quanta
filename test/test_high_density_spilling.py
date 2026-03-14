import sys
import os
import time

xlang_bin_dir = "D:\\CantorAI\\xlang\\out\\build\\x64-Debug\\bin"
quanta_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)
sys.path.append(quanta_bin_dir)

import xlang 
quanta = xlang.importModule("quanta", fromPath="Quanta")

def test_spilling():
    vdb_path = "./temp_tier3_test_vdb"
    if os.path.exists(vdb_path):
        import shutil
        shutil.rmtree(vdb_path)
    os.makedirs(vdb_path)

    print("Instantiating VDB...")
    vdb = quanta.partitioned_vdb(
        prefix="dev",
        path=vdb_path,
        dimension=10, 
        max_memory_gb=0.0001, 
        granularity="hourly",
        ttl_minutes=1
    )
    
    vectors = []
    vector = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]

    ts = int(time.time() * 1000)

    import numpy as np
    
    print("Beginning 5 sequential batch injections of 500 vectors to force cascade...")
    for batch_idx in range(5):
        vectors = []
        for i in range(500):
            vectors.append(vector)
            
        vectors_np = np.array(vectors, dtype=np.float32)
        start_id = 1001 + (batch_idx * 500)
        
        vdb.AddVectors(start_id, vectors_np, chunks=[f"chunk_{batch_idx}"] * 500, timestamp=ts, partition="IPC_CAMERA_1")
        print(f"Batch {batch_idx+1}/5 injected.")
    
    print("\nClosing VDB to trigger final cascades and manifest saves...")
    vdb.Close()
    
    print("\nChecking generated files in: ", vdb_path)
    for root, dirs, files in os.walk(vdb_path):
        for f in files:
            print("  - ", os.path.join(root, f))
            
    # Manifest validation
    print("\nReading generated manifest.db bounds...")
    import sqlite3
    try:
        conn = sqlite3.connect(os.path.join(vdb_path, "dev_manifest.db"))
        c = conn.cursor()
        c.execute("SELECT * FROM buckets")
        rows = c.fetchall()
        print(f"Manifest Buckets Created ({len(rows)}):")
        for r in rows:
            print("  ", r)
        conn.close()
    except Exception as e:
        print("Manifest validation error:", str(e))

if __name__ == "__main__":
    test_spilling()