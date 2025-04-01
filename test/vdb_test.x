import cantor thru 'lrpc:1000'
from Quanta import quanta
quanta.cantor = cantor
import time

# Set up the connection to cantor
quanta.cantor = cantor

# Define the capacity for 1M vectors
capacity = 1000000
vdb = quanta.vdb(1024, capacity)

# Add 1,000,000 vectors to the database
# for i in range(0, capacity):
#    t = tensor.randwithshape([1024], min = 0, max = 1, dtype = tensor.float32)
#    print("Adding vector", i)
#    vdb.AddVector(i, t)

# Save the vector database
# vdb.Save("d:/Test/test101.vdb")
vdb.Load("d:/Test/test101.vdb")
# Generate a random query vector
t1 = tensor.randwithshape([1024], min = 0, max = 1, dtype = tensor.float32)

# Measure the performance of the Lookup operation
start_time = time.time()
xyz = vdb.Lookup(t1, 10)
end_time = time.time()

lookup_time = end_time - start_time

print("Lookup result:", xyz)
print("Lookup time (ms):", lookup_time*1000)
print("done")
