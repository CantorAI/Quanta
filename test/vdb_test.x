import cantor thru 'lrpc:1000'
from Quanta import quanta
quanta.cantor = cantor
import time
import embedding_api

quanta.cantor = cantor


vdb = quanta.vdb()

vdb.Load("d:/Test/test104.vdb")

embedder = embedding_api.EmbeddingAPI()
embedder.set_active_model("mpnet")

start_time = time.time()
inStr = ""
while inStr != "Quit":
    inStr = input()
    if inStr == "Quit":
        break
    start_time = time.time()
    py_emb = embedder.embed_text(inStr)
    emb = to_xlang(py_emb)
    xyz = vdb.Lookup(emb, 2)
    end_time = time.time()
    lookup_time = end_time - start_time
    print("Lookup time (ms):", lookup_time*1000)
    print("Results:",xyz)
print("done")
