import time
import cantor thru 'lrpc:1000'
from Quanta import quanta

quanta.cantor = cantor
dfs = quanta.dfs()
# dfs.Scan("C:/")
dfs.LoadIndex("d:/test101.idx")

start_time = time.time()
files = dfs.Query("D:/CantorAI/Quanta/src/main/main.cpp")
elapsed_time = time.time() - start_time

# print("Files found:", files)
print("D:/CantorAI/Quanta/src/main/main.cpp dfs.Query took:", elapsed_time*1000, "ms",",FileCount:", len(files))

start_time = time.time()
files = dfs.Query("README.md")
elapsed_time = time.time() - start_time

# print("Files found:", files)
print("README.md dfs.Query took:", elapsed_time*1000, "ms",",FileCount:",len(files))

start_time = time.time()
files = dfs.Query("PyEngHostImpl.cpp")
elapsed_time = time.time() - start_time

# print("Files found:", files)
print("PyEngHostImpl.cpp dfs.Query took:", elapsed_time*1000, "ms",",FileCount:", len(files))


print("Dfs_Scan done")
