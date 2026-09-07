import xlang3
import time
import numpy as np

def test_ttl():
    quanta = xlang3.importModule("quanta", fromPath="Quanta")

    print("[Quanta TTL Test] Creating VDB instance with 1-minute TTL and 5-sec auto save...")
    vdb = quanta.partitioned_vdb(
        prefix="ttl_test",
        path="./data/ttl_test",
        dim=4,
        ttl_minutes=1, # 1 minute for fast testing
        auto_save_seconds=5 # 5 seconds for fast testing
    )
    
    # 1. Insert vector into partition 1 (Timestamp A)
    ts_yesterday = int(time.time() * 1000) - (86400 * 1000)
    emb = np.array([1, 0, 0, 0], dtype=np.float32)
    vdb.AddVectors(1, emb, timestamp=ts_yesterday, partition="camA")
    print(f"Added vector 1 to yesterday's partition (ts={ts_yesterday})")

    # 2. Insert vector into partition 2 (Timestamp B)
    ts_today = int(time.time() * 1000)
    emb2 = np.array([0, 1, 0, 0], dtype=np.float32)
    vdb.AddVectors(2, emb2, timestamp=ts_today, partition="camA")
    print(f"Added vector 2 to today's partition (ts={ts_today})")

    partitions_before = vdb.ListPartitions()
    print(f"Partitions immediately after insert ({len(partitions_before)} expected 2):")
    for p in partitions_before:
        print(f"  - {p['key']} (loaded={p['loaded']}, count={p['count'] if p['loaded'] else 0})")

    print("\nWaiting 6 seconds for auto-save thread to flush dirty partitions...")
    time.sleep(6)

    print("\nWaiting 65 seconds for TTL thread to dump partitions from RAM...")
    time.sleep(65)

    partitions_after_ttl = vdb.ListPartitions()
    print(f"\nPartitions after TTL expiry ({len(partitions_after_ttl)} expected 2):")
    for p in partitions_after_ttl:
        print(f"  - {p['key']} (loaded={p['loaded']}, count={p.get('count', 0)})")
        if p['loaded']:
            print("[WARNING] Partition was NOT successfully offloaded from RAM!")
        else:
            print("[SUCCESS] Partition securely dumped from memory via TTL thread!")

    # 3. Prove lazy loading still works (Cache Miss query)
    print("\nSimulating a lookup query matching yesterday's partition (Cache Miss)...")
    results = vdb.Lookup(emb, 1, ts_start=ts_yesterday-1000, ts_end=ts_yesterday+1000)
    print(f"Returned IDs: {[r[0] for r in results]}")

    partitions_final = vdb.ListPartitions()
    print("\nPartitions after lazy load lookup:")
    for p in partitions_final:
        print(f"  - {p['key']} (loaded={p['loaded']}, count={p.get('count', 0)})")
    
    print("\nDone. Autonomous TTL Memory Eviction architecture visually verified.")

if __name__ == "__main__":
    test_ttl()
