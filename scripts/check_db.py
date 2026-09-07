import sys
import os
import argparse
import time
import pprint

xlang_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)

import xlang3
quanta = xlang3.importModule("quanta", fromPath="Quanta")

def get_default_prefix(path):
    """Scan the path for a _config.db file to determine the prefix."""
    try:
        for f in os.listdir(path):
            if f.endswith("_config.db"):
                # strip _config.db
                return f[:-10]
    except Exception as e:
        pass
    return "vdb" # fallback
    
def main():
    parser = argparse.ArgumentParser(description="Quanta PartitionedVdb Inspection and Health Check Tool")
    parser.add_argument("--path", type=str, required=True, help="Path to the PartitionedVdb directory")
    parser.add_argument("--prefix", type=str, default=None, help="Prefix of the PartitionedVdb (default: auto-detect from path, fallback 'vdb')")
    parser.add_argument("--full-scan", action="store_true", help="Perform a deep full scan counting vectors and buckets across all files")
    
    args = parser.parse_args()

    # Ensure path is absolute
    args.path = os.path.abspath(args.path)
    
    # Auto-detect prefix if not provided
    if args.prefix is None:
        args.prefix = get_default_prefix(args.path)

    # Load quanta module (done globally above, like test script)
    if quanta is None:
        print(f"Failed to load quanta module.")
        sys.exit(1)

    # Initialize vdb without defining dimensions since we are just inspecting
    print(f"Loading PartitionedVdb at: {args.path} (prefix: {args.prefix})")
    
    # We create the PVDB instance and load it. Since we only want to read, 
    # we initialize it. The C++ code will load the configuration from SQLite.
    vdb = quanta.partitioned_vdb(prefix=args.prefix, path=args.path)
    
    print("\n--- VDB Health Info ---")
    try:
        health = vdb.GetHealth()
        pprint.pprint(health)
    except Exception as e:
        print(f"Error calling GetHealth(): {e}")

    print("\n--- Loaded Partitions (Fast Estimate) ---")
    try:
        total_records = vdb.GetTotalRecords()
        print(f"Total Records (Fast Atomic Estimate from DB): {total_records}")
        
        parts = vdb.ListPartitions()
        loaded_parts = [p for p in parts if p.get("loaded", False)]
        print(f"Loaded Partitions: {len(loaded_parts)} / {len(parts)} total")
        for p in loaded_parts:
            print(f"  {p['key']}: {p.get('count', 0)} vectors, tags: {p.get('tags', [])}")
            
    except Exception as e:
        print(f"Error fetching partition info: {e}")

    if args.full_scan:
        print("\n--- Full Disk Scan ---")
        print("Starting full scan (this may take a while for large databases)...")
        start_time = time.time()
        try:
            scan_result = vdb.PerformFullScan()
            elapsed = time.time() - start_time
            print(f"Scan completed in {elapsed:.2f} seconds.")
            pprint.pprint(scan_result)
        except Exception as e:
            print(f"Error during PerformFullScan(): {e}")
            
    else:
        print("\n(Tip: Run with --full-scan to do a comprehensive disk scan of all partitions/WALs)")

    # Explicitly Close the DB securely to flush and join threads properly
    vdb.Close()

if __name__ == "__main__":
    main()
