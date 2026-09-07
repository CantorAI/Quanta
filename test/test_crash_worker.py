import sys
import os
import time
import numpy as np

xlang_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)

import xlang3
quanta = xlang3.importModule("quanta", fromPath="Quanta")

def run_worker():
    crash_dir = "D:\\CantorAI\\Quanta\\test\\data\\vdb_crash_test"
    db = quanta.partitioned_vdb(
        path=crash_dir, 
        prefix="crash_clip", 
        dimension=512, 
        granularity="hourly",
        max_memory_gb=0.0001, # extremely small to force cascade
        ttl_minutes=1
    )
    
    print("[Worker] Engine started. Generating highly identifiable vectors...", flush=True)
    
    # We generate up to 2500 vectors.
    for i in range(2500):
        # Create a predictable vector: first element is ID/10000.0, rest is 0.5
        vec = [0.5] * 512
        vec[0] = float(i) / 10000.0
        vnp = np.array([vec], dtype=np.float32)
        
        db.AddVectors([i], vnp, timestamp=1773530000000 + i*10, partition="CAM_1")
        
        if (i + 1) % 500 == 0:
            print(f"[Worker] Successfully inserted {i+1} vectors. Latest ID: {i}", flush=True)
            
        time.sleep(0.002) # throttle slightly to ensure WALs pile up concurrently

    print("[Worker] Finished inserting all vectors. Waiting for OS kill...", flush=True)
    while True:
        time.sleep(1)

if __name__ == "__main__":
    run_worker()
