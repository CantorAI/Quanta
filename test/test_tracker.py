import sys
import os
import time
import shutil

# Add CantorAI binary path to module search path
xlang_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)

import xlang
quanta = xlang.importModule("quanta", fromPath="Quanta")

def test_scene_tracker():
    print("Initializing Quanta VDB...")
    db_path = os.path.join(os.path.dirname(__file__), "data", "test_vdb_clip")
    if os.path.exists(db_path):
        shutil.rmtree(db_path, ignore_errors=True)
    os.makedirs(db_path, exist_ok=True)
    
    vdb = quanta.partitioned_vdb(
        prefix="clip",
        path=db_path,
        dim=4,  # Small dimension for testing
        granularity="monthly",
        max_memory_gb=0.01
    )
    
    device_id = "test_cam_01"
    
    print("\n--- Test 1: Creating New Tracker ---")
    tracker = vdb.CreateTracker(threshold=0.85, tracker_id=device_id, window_size=3)
    
    print("Appending Frame 1 (New Scene)...")
    emb1 = [1.0, 0.0, 0.0, 0.0]
    res1 = tracker.Append(1001, timestamp=1000, partitionTag=device_id, score=1.0, embedding=emb1)
    print("Result 1:", res1)
    assert res1["is_new_scene"] == False
    
    print("Appending Frame 2 (Same Scene)...")
    emb2 = [0.9, 0.1, 0.0, 0.0]
    res2 = tracker.Append(1002, timestamp=2000, partitionTag=device_id, score=0.9, embedding=emb2)
    print("Result 2:", res2)
    assert res2["is_new_scene"] == False
    assert res2["scene_frame_count"] == 2
    
    print("Appending Frame 3 (New Scene Catalyst)...")
    emb3 = [0.0, 1.0, 0.0, 0.0] # Completely different orthognal vector
    res3 = tracker.Append(1003, timestamp=3000, partitionTag=device_id, score=0.95, embedding=emb3)
    print("Result 3:", res3)
    assert res3["is_new_scene"] == True # Threshold < 0.85 should trigger cut
    assert res3["scene_frame_count"] == 1
    
    print("\n--- Test 2: Simulating Crash / Reload ---")
    del tracker
    
    print(f"Checking disk for binary checkpoint: {db_path}/trackers/{device_id}.bin")
    bin_file = os.path.join(db_path, "trackers", f"{device_id}.bin")
    if os.path.exists(bin_file):
        print(f"SUCCESS: Binary file found at {bin_file} ({os.path.getsize(bin_file)} bytes)")
    else:
        print("ERROR: Binary file not generated!")
        sys.exit(1)
        
    print("Re-instantiating tracker (should load from .bin automatically)...")
    tracker2 = vdb.CreateTracker(threshold=0.85, tracker_id=device_id, window_size=3)
    
    state = tracker2.GetState()
    print("Recovered State:", state)
    
    assert state["scene_id"] == 1
    assert state["frame_count"] == 1
    assert state["method"] == "window"
    
    print("\n✅ All Dual-Mode and Binary Persistence tests PASSED flawlessly!")

if __name__ == "__main__":
    test_scene_tracker()
