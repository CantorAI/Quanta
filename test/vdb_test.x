import cantor thru 'lrpc:1000'
from Quanta import quanta
quanta.cantor = cantor

capacity = 10000*100
vdb = quanta.vdb(1024, capacity)
for i in range(0, 100):
    t = tensor.randwithshape([1024], min = 0, max = 1,dtype=tensor.float32)
    vdb.AddVector(i,t)
vdb.Save("d:/Test/test101.vdb")
t1 = tensor.randwithshape([1024], min = 0, max = 1, dtype = tensor.float32)
xyz = vdb.Lookup(t1,10)
print("xyz=", xyz)
print("done")