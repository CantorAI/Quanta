import sys
import os

xlang_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)

import xlang3

quanta = xlang3.importModule("quanta", fromPath="Quanta")

db_path = "D:\\CantorStorage-SH\\vdb\\clip_512"
print(f"Loading PartitionedVdb from {db_path}...")

vdb = quanta.partitioned_vdb(
    prefix="clip",
    path=db_path,
    dim=512,
    granularity="monthly"
)

print("Listing partitions...")
try:
    partitions = vdb.ListPartitions()
    print("Partitions:", partitions)
except Exception as e:
    print(f"Failed to list partitions: {e}")

print("Attempting to query to force partition load...")
import numpy as np
query_vec = np.random.rand(512).astype(np.float32).tolist()

try:
    print("Executing Lookup...")
    # This will force LoadPartition for relevant time ranges
    results = vdb.Lookup(query_vec, 5, 0.5, 0)
    print(f"Results: {results}")
except Exception as e:
    print(f"Lookup failed with error: {e}")

print("Done.")
