import os
import sys
import time
if len(sys.argv) > 2:
    sys.path.insert(0, sys.argv[2])
    import xlang3
    quanta = xlang3.importModule("quanta", fromPath=os.path.join(sys.argv[2], "Quanta"))
else:
    from Quanta import quanta


def progress(message):
    print(message, flush=True)
    sys.stdout.flush()
    with open(os.path.join(sys.argv[1], "progress.log"), "a") as log:
        log.write(message + "\n")


def fails(callback):
    caught = False
    try:
        callback()
    except Exception:
        caught = True
    assert caught, "invalid input was accepted"


def check_basic(root):
    db = quanta.vdb(dimension=3, max_elements=128)
    assert db.AddVectors(10, [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]], chunks=["x", "y", "z"]) == 12
    rows = db.Lookup([0., 1., 0.], 3)
    assert rows[0][0] == 11 and rows[0][2] == "y", rows
    assert abs(rows[0][1] - 1.) < 1e-6
    filename = os.path.join(root, "basic.vdb")
    assert db.Save(filename)
    loaded = quanta.vdb(dimension=1, max_elements=32)
    assert loaded.Load(filename)
    assert loaded.Lookup([0., 0., 1.], 1)[0][0] == 12
    cosine = quanta.vdb(dimension=3, max_elements=16, space="cosine")
    cosine.AddVectors(20, [[0., 2., 0.], [2., 0., 0.], [0., 0., 2.]])
    assert cosine.Lookup([0., 7., 0.], 1)[0][0] == 20
    assert abs(cosine.Lookup([0., 7., 0.], 1)[0][1] - 1.) < 1e-6
    fails(lambda: db.AddVectors(0, [1., 2.]))
    fails(lambda: db.Lookup([0., 1., 0.], -1))
    fails(lambda: db.AddVectors([1], [1., 0., 0., 0., 1., 0.]))
    fails(lambda: db.AddVectors(1, ["not a number", 0., 1.]))
    progress("basic vectors, cosine, save/reload passed")


def wait_result(db, query, count):
    deadline = time.time() + 15
    while time.time() < deadline:
        rows = db.Lookup(query, count)
        if len(rows) == count:
            return rows
        time.sleep(.05)
    raise AssertionError("ingestion did not become queryable: " + str(db.GetHealth()))


def check_partitioned(root):
    path = os.path.join(root, "partitioned")
    db = quanta.partitioned_vdb(path=path, prefix="test", dimension=3, max_memory_gb=.001,
                               wal_cooling_time_seconds=1, auto_save_seconds=1)
    assert quanta.GetPartitionedVdb(path=path, prefix="test") is db
    assert db.AddVectors(101, [[1., 0., 0.], [0., 1., 0.]], chunks=["one", "two"],
                         timestamp=1700000000000, partition="camera-a") == 102
    rows = wait_result(db, [1., 0., 0.], 2)
    assert rows[0][0] == 101 and rows[0][2] == "one", rows
    assert db.GetTotalRecords() == 2
    assert db.QueryLabelByID(101) == "one"
    assert db.QueryLabelByID(102, partition="camera-a", timestamp=1700000000000) == "two"
    index = db.GetPartitionInfo("camera-a")["customIndex"]
    assert db.AddPartitionTag(index, "alias-a")
    assert db.QueryLabelByID(101, partition="alias-a") == "one"
    items = [{"image_id": row[0], "key": row[3]} for row in rows]
    groups = db.Grouping(items, .9, full_partition_key="key")
    assert sorted([item for group in groups for item in group]) == [101, 102], groups
    progress("partition labels and grouping passed")
    tracker = db.CreateTracker(threshold=.8, method="centroid", tracker_id="camera-a")
    first = tracker.Append(101, embedding=[1., 0., 0.], timestamp=1700000000000)
    assert first["scene_id"] == 0, first
    second = tracker.Append(102, embedding=[1., .01, 0.], timestamp=1700000001000)
    assert second["scene_frame_count"] == 2, second
    third = tracker.Append(103, embedding=[0., 1., 0.], timestamp=1700000002000)
    assert third["is_new_scene"] and third["completed_scene"]["frame_count"] == 2, third
    del tracker
    progress("first tracker released")
    tracker = db.CreateTracker(threshold=.8, method="centroid", tracker_id="camera-a")
    assert tracker.GetState()["scene_id"] == 1
    del tracker
    progress("reloaded tracker released; closing database")
    assert db.Close()
    progress("database closed")
    reopened = quanta.partitioned_vdb(path=path, prefix="test", wal_cooling_time_seconds=1)
    assert reopened is not db
    assert wait_result(reopened, [1., 0., 0.], 2)[0][0] == 101
    assert reopened.QueryLabelByID(101, partition="alias-a") == "one"
    assert reopened.Close()
    progress("partitioned ingestion, cache, tracker, persistence passed")


def check_close_drain(root):
    path = os.path.join(root, "drain")
    db = quanta.partitioned_vdb(path=path, prefix="drain", dimension=3, max_memory_gb=.001)
    for batch in range(20):
        db.AddVectors(1000 + batch * 10, [1., 0., 0.] * 10, chunks="queued")
    assert db.Close()
    reopened = quanta.partitioned_vdb(path=path, prefix="drain", wal_cooling_time_seconds=1)
    rows = wait_result(reopened, [1., 0., 0.], 200)
    assert sorted([row[0] for row in rows]) == list(range(1000, 1200))
    assert reopened.GetTotalRecords() == 200
    assert reopened.Close()
    progress("immediate Close drained 200 queued vectors")

    large = quanta.partitioned_vdb(path=path, prefix="large", dimension=3, max_elements=1000,
                                  wal_cooling_time_seconds=1)
    large.AddVectors(5000, [1., 0., 0.] * 3500, chunks="large batch")
    assert large.Close()
    large = quanta.partitioned_vdb(path=path, prefix="large", wal_cooling_time_seconds=1)
    rows = wait_result(large, [1., 0., 0.], 3500)
    assert sorted([row[0] for row in rows]) == list(range(5000, 8500))
    assert large.Close()
    progress("3500-vector batch split across capacity buckets")


def check_tensor():
    import tensor as T
    for dtype in [T.float32, T.float64, T.int32, T.int64]:
        db = quanta.vdb(dimension=3, max_elements=16)
        value = T.tensor([[1, 0, 0], [0, 1, 0]], dtype=dtype)
        assert db.AddVectors(1, value) == 2
        assert db.Lookup(T.tensor([0, 1, 0], dtype=dtype), 1)[0][0] == 2
    db = quanta.vdb(dimension=2, max_elements=16)
    view = (T.tensor([[1, 2, 3], [4, 5, 6]]) * T.unary_op("permute", axes=[1, 0])).eval()
    assert db.AddVectors(40, view) == 42
    assert db.Lookup([2, 5], 1)[0][0] == 41
    fails(lambda: db.AddVectors(0, T.input("pending", shape=[2])))
    progress("CPU tensor dtypes, strided views, symbolic rejection passed")


def check_buffers():
    from array import array
    db = quanta.vdb(dimension=3, max_elements=16)
    for fmt in ["f", "d"]:
        value = array(fmt, [1., 0., 0., 0., 1., 0.])
        db.AddVectors(1, value)
        assert db.Lookup(array(fmt, [0., 1., 0.]), 1)[0][0] == 2
    fails(lambda: db.AddVectors(1, b"bad"))
    progress("CPython float32/float64 buffer calls passed")


def check_dfs(root):
    folder = os.path.join(root, "files")
    os.makedirs(folder)
    with open(os.path.join(folder, "needle.txt"), "w") as output:
        output.write("Quanta native test")
    dfs = quanta.dfs()
    assert dfs.Scan(folder)
    index = os.path.join(root, "files.idx")
    dfs.BuildIndex(index)
    assert len(dfs.Query("needle")) == 1
    loaded = quanta.dfs()
    loaded.LoadIndex(index)
    assert loaded.Query("needle") == dfs.Query("needle")
    progress("DFS scan and index reload passed")


def check_errors(root):
    path = os.path.join(root, "broken")
    os.makedirs(path)
    wal = os.path.join(path, "bad.2023-11-14-22_0_0000.wal_1")
    with open(wal, "wb") as output:
        output.write(b"truncated")
    db = quanta.partitioned_vdb(path=path, prefix="bad", dimension=3, max_elements=16)
    deadline = time.time() + 15
    while not db.GetHealth()["error"] and time.time() < deadline:
        time.sleep(.05)
    assert db.GetHealth()["error"], "corrupt WAL was not reported"
    assert os.path.exists(wal), "failed WAL merge deleted the recovery file"
    fails(lambda: db.AddVectors(1, [1., 0., 0.]))
    fails(lambda: db.Close())
    fails(lambda: quanta.partitioned_vdb(path=wal, prefix="invalid"))
    progress("background errors reported; failed WAL retained")


root = sys.argv[1]
progress("Quanta imported through XLang3")
only = os.getenv("QUANTA_TEST_ONLY", "")
if not only or only == "basic": check_basic(root)
if not only or only == "partitioned": check_partitioned(root)
if not only or only == "drain": check_close_drain(root)
if not only or only == "dfs": check_dfs(root)
if not only or only == "errors": check_errors(root)
if not only or only == "tensor":
    if len(sys.argv) > 2:
        check_buffers()
    else:
        check_tensor()
progress("quanta-runtime-passed")
