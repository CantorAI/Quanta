import sys
import os
import time
import shutil
import subprocess
import numpy as np

xlang_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)

import xlang

def print_dir_state(test_dir, phase=""):
    print(f"\n--- Directory State [{phase}] ---", flush=True)
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
    print("-" * 40, flush=True)
    return len(wals)

def main():
    print("=========================================================================", flush=True)
    print("=== Quanta VDB: Advanced Async Crash, Add & Lookup Parallel Test ===", flush=True)
    print("=========================================================================", flush=True)
    
    test_dir = "D:\\CantorAI\\Quanta\\test\\data\\vdb_crash_test"
    if os.path.exists(test_dir):
        shutil.rmtree(test_dir, ignore_errors=True)
    os.makedirs(test_dir, exist_ok=True)

    print("\n[Phase 1] Spawning Worker for Data Ingestion...", flush=True)
    worker_script = os.path.join("Quanta", "test", "test_crash_worker.py")
    process = subprocess.Popen([sys.executable, worker_script])
    
    # Let the worker run for a few seconds to generate data and WAL files
    time.sleep(3.5)
    
    print("\n[Phase 2] Simulating Hard OS Crash (SIGKILL)...", flush=True)
    process.kill()
    process.wait()
    time.sleep(1.0) # Wait for filesystem sync
    
    print("\n[Phase 3] Validating Orphaned WALs...", flush=True)
    wals_left = print_dir_state(test_dir, "Post-Crash")
    
    if wals_left == 0:
        print("[!] FAILED: Worker did not generate WAL orphans before being killed. Increase sleep time.", flush=True)
        return
    else:
        print(f"[+] Verified {wals_left} orphaned WAL(s) remain on disk, simulating a true power-loss event.", flush=True)
        
    print("\n[Phase 4] Rebooting Quanta Instance to trigger `Init()` Async WAL Recovery...", flush=True)
    quanta = xlang.importModule("quanta", fromPath="Quanta")
    
    start_time = time.time()
    recovery_db = quanta.partitioned_vdb(
        path=test_dir, 
        prefix="crash_clip", 
        dimension=512, 
        granularity="hourly",
        max_memory_gb=0.0001,
        ttl_minutes=1
    )
    recovery_duration = time.time() - start_time
    print(f"[+] Engine recovered (Init returned) in {recovery_duration:.4f} seconds! Background thread is now actively merging.", flush=True)

    print("\n[Phase 5] IMMEDIATE Concurrent 'AddVectors' while background recovery is running...", flush=True)
    new_start_id = 9000
    print(f"[+] Pushing vectors ID {new_start_id} to {new_start_id+500} into the active graph.", flush=True)
    for i in range(new_start_id, new_start_id + 500):
        vec = [0.8] * 512
        vec[0] = float(i) / 10000.0
        vnp = np.array([vec], dtype=np.float32)
        recovery_db.AddVectors([i], vnp, timestamp=1773539000000 + i*10, partition="CAM_1")
        if (i - new_start_id + 1) % 100 == 0:
            print(f"  -> Added {i - new_start_id + 1} live vectors natively.", flush=True)
        
    print("[+] Successfully added new live vectors concurrently with async recovery.", flush=True)

    print("\n[Phase 6] Polling for Async Recovery Completion...", flush=True)
    max_retries = 30
    wals_left_post = -1
    for r in range(max_retries):
        wals_left_post = print_dir_state(test_dir, f"Async Poll {r+1}/{max_retries}")
        if wals_left_post == 0:
            break
        time.sleep(1.0)
    
    if wals_left_post == 0:
         print("[+] SUCCESS: All orphaned WALs were merged and securely deleted via Async Queue.", flush=True)
    else:
         print(f"[!] FAILED: {wals_left_post} WAL files are still stranded on disk!", flush=True)
         assert False, "WAL async deletion failed or timed out."

    print("\n[Phase 7] Validating 'Lookup' Accuracy Against Both Pre-Crash & Live Vectors...", flush=True)
    
    # Lookup a vector inserted by the worker (before crash)
    print("\n  -> Testing Lookup for pre-crash vector roughly matching ID 250 (vec[0] = 0.0250)")
    q1 = [0.5] * 512
    q1[0] = 250.0 / 10000.0
    q1_np = np.array([q1], dtype=np.float32)
    res1 = recovery_db.Lookup(q1_np, 3, "CAM_1")
    print(f"  -> Lookup Results: {res1}")
    
    found_250 = False
    for match in res1:
        if isinstance(match, (list, tuple)) and match[0] == 250: found_250 = True
        elif isinstance(match, int) and match == 250: found_250 = True
        elif hasattr(match, 'id') and match.id == 250: found_250 = True
    
    if found_250:
        print("  -> [+] PASS: Database successfully recovered and accurately searched pre-crash vectors.")
    else:
        print("  -> [!] WARN/FAIL: ID 250 not found in top 3. Structural graph integrity might be compromised.", flush=True)
    
    # Lookup a vector inserted post-crash
    print(f"\n  -> Testing Lookup for live-added vector roughly matching ID {new_start_id+50} (vec[0] = {str((new_start_id+50)/10000.0)})")
    q2 = [0.8] * 512
    q2[0] = (new_start_id + 50) / 10000.0
    q2_np = np.array([q2], dtype=np.float32)
    res2 = recovery_db.Lookup(q2_np, 3, "CAM_1")
    print(f"  -> Lookup Results: {res2}")
    
    found_post = False
    for match in res2:
        if isinstance(match, (list, tuple)) and match[0] == (new_start_id + 50): found_post = True
        elif isinstance(match, int) and match == (new_start_id + 50): found_post = True
        elif hasattr(match, 'id') and match.id == (new_start_id + 50): found_post = True
        
    if found_post:
        print("  -> [+] PASS: Database successfully indexed and searched live vectors added post-recovery.")
    else:
        print(f"  -> [!] WARN/FAIL: ID {new_start_id+50} not found in top 3.", flush=True)
        
    print("\n[Phase 8] Shutting down gracefully...", flush=True)
    recovery_db.Close()
    
    final_wals = print_dir_state(test_dir, "Final Clean Shutdown")
    assert final_wals == 0, "[!] FAILED: Clean shutdown left WALs behind."
    if not found_250 or not found_post:
        assert False, "[!] FAILED: Structural lookup completely failed to correlate physical entries!"
    
    print("\n=========================================================================", flush=True)
    print("=== ADVANCED LIFECYCLE TEST COMPLETED WITH 100% INTEGRITY & SUCCESS ===", flush=True)
    print("=========================================================================", flush=True)

if __name__ == "__main__":
    main()
