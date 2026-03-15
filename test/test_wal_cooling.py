import sys
import os
import time
import shutil

xlang_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)

import xlang
import numpy as np

def print_dir_state(test_dir, phase=""):
    print(f"\n--- Directory State [{phase}] ---")
    wals = []
    hnsws = []
    vdbs = []
    if os.path.exists(test_dir):
        for f in os.listdir(test_dir):
            if ".wal_" in f: wals.append(f)
            elif ".hnsw" in f: hnsws.append(f)
            elif ".vdb" in f: vdbs.append(f)
    print(f"  WAL files ({len(wals)}): {wals}")
    print(f"  HNSW files ({len(hnsws)}): {hnsws}")
    print(f"  VDB files ({len(vdbs)}): {vdbs}")
    print("-" * 40)
    return len(wals)

def init_engine(db_path, cooling_sec=3):
    print(f"\n[+] Initializing engine at {db_path} with wal_cooling_time_seconds={cooling_sec}...")
    try:
        if os.path.exists(db_path):
            shutil.rmtree(db_path)
    except:
        pass

    quanta = xlang.importModule("quanta", fromPath="Quanta")
    engine = quanta.partitioned_vdb(path=db_path, prefix="cool_test", dimension=512, granularity="hourly", max_memory_gb=0.1, wal_cooling_time_seconds=cooling_sec)
    return engine

def test_cooling_flush_alive():
    print("\n--- Test 1: Live Process Cooling Flush ---")
    db_path = "D:\\CantorAI\\Quanta\\test\\data\\cooling_test_live"
    engine = init_engine(db_path, cooling_sec=2)
    
    # Add exactly 20 vectors (less than wal_rotation_threshold=100)
    print("  -> Inserting 20 vectors. This will NOT trigger capacity rotation limit.")
    vecs = []
    for i in range(20):
        v = [0.1] * 512
        v[0] = float(i) / 1000.0
        vecs.append(v)
    vnp = np.array(vecs, dtype=np.float32)
    
    # Use explicit timestamp
    ts = 1800000000000
    engine.AddVectors(list(range(20)), vnp, timestamp=ts, partition="CAM_1")
    
    # Check directory immediately, WAL should exist
    wal_files_pre = [f for f in os.listdir(db_path) if ".wal_" in f]
    print(f"  -> Immediately after insert, WAL files: {wal_files_pre}")
    if len(wal_files_pre) == 0:
        print("  [ERROR] WAL file was not created!")
        sys.exit(1)
        
    print("  -> Sleeping for 8 seconds to exceed cooling time (2s) and the C++ thread 5s loop window...")
    time.sleep(8.0)
    
    print_dir_state(db_path, "Post-Cooling")
    
    # Check directory again, WAL should have been rotated and merged by the maintenance loop
    wal_files_post = [f for f in os.listdir(db_path) if ".wal_" in f]
    print(f"  -> After cooling wait, WAL files: {wal_files_post}")
    
    if len(wal_files_post) > 0:
        print(f"  [ERROR] WAL files still exist! They were not flushed by cooling time. Found: {wal_files_post}")
        sys.exit(1)
        
    # Verify via lookup
    print("  -> Verifying native vector graph lookup.")
    query = np.array([[float(5)/1000.0] + [0.1]*511], dtype=np.float32)
    res = engine.Lookup(query, 1, partition="CAM_1")
    print(f"  -> Lookup returned: {res}")
    
    res = res or []
    if len(res) == 0 or res[0][0] != 5:
        print("  [ERROR] Struct lookup failed.")
        sys.exit(1)
        
    engine.Close()
    print("  [+] Test 1 Passed.")

def test_crash_orphans_recovery():
    print("\n--- Test 2: Crash Simulation Immediate Hydration ---")
    db_path = "D:\\CantorAI\\Quanta\\test\\data\\cooling_test_crash"
    engine = init_engine(db_path, cooling_sec=1000) # very long cooling time
    
    print("  -> Inserting 20 vectors into a fresh pool.")
    vecs = []
    for i in range(20):
        v = [0.1] * 512
        v[0] = float(i) / 1000.0
        vecs.append(v)
    vnp = np.array(vecs, dtype=np.float32)
    
    ts = 1800000000000
    engine.AddVectors(list(range(20)), vnp, timestamp=ts, partition="CAM_2")
    
    wal_files = [f for f in os.listdir(db_path) if ".wal_" in f]
    print(f"  -> Pre-crash WALs present: {wal_files}")
    if len(wal_files) == 0:
        print("  [ERROR] No WAL generated.")
        sys.exit(1)
        
    # Kill process immediately (simulating crash without closing) so WAL is abandoned.
    # To simulate we just drop the engine without Close, leaving WAL physically on disk.
    del engine
    
    # Re-init (this should instantly queue orphaned WALs and merge them independently of Cooling Time)
    print("  -> Rebooting system (Init should spot orphaned WAL instantly).")
    quanta2 = xlang.importModule("quanta", fromPath="Quanta")
    engine2 = quanta2.partitioned_vdb(path=db_path, prefix="cool_test", dimension=512, granularity="hourly", max_memory_gb=0.1, wal_cooling_time_seconds=1000)
    
    # Wait briefly for background thread to consume the pending queue
    time.sleep(1)
    
    wal_files_recovered = [f for f in os.listdir(db_path) if ".wal_" in f]
    print(f"  -> Post-reboot WALs remaining: {wal_files_recovered}")
    if len(wal_files_recovered) > 0:
        print("  [ERROR] Orphaned WALs were not hydrated automatically on Boot!")
        sys.exit(1)
        
    print("  -> Verifying structures...")
    query = np.array([[float(10)/1000.0] + [0.1]*511], dtype=np.float32)
    res = engine2.Lookup(query, 1, partition="CAM_2")
    print(f"  -> Lookup returned: {res}")
    
    if len(res) == 0 or res[0][0] != 10:
        print("  [ERROR] Structural data lost!")
        sys.exit(1)
        
    engine2.Close()
    print("  [+] Test 2 Passed.")

if __name__ == "__main__":
    test_cooling_flush_alive()
    test_crash_orphans_recovery()
    print("\n[SUCCESS] WAL Time-Based Cooling verified natively!")
