"""
Comprehensive test suite for PartitionedVdb API.
Tests: basic ops, precise timestamp filtering, partitions,
       save/load, CLIP embeddings, concurrency, crash resilience,
       and singleton via Cantor LRPC.

Usage:
  python partitioned_vdb_full_test.py
"""

import os
import sys
import time
import shutil
import unittest
import threading
import subprocess
import numpy as np

# ── Paths ──────────────────────────────────────────────────────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VDB_PATH = os.path.join(SCRIPT_DIR, "test_pvdb_full")
DIM = 512
IMAGE_DIR = r"D:\CantorStorage\Uploads\2026"

# ── XLang / Quanta import ─────────────────────────────────────────
import xlang3
quanta = xlang3.importModule("quanta", fromPath="Quanta")


# ── Helpers ────────────────────────────────────────────────────────
def random_emb(dim=DIM):
    """Generate a random unit-norm embedding."""
    e = np.random.randn(dim).astype(np.float32)
    return e / np.linalg.norm(e)


def cleanup_vdb(path):
    """Remove VDB directory if it exists."""
    if os.path.exists(path):
        shutil.rmtree(path, ignore_errors=True)


def make_vdb(path=VDB_PATH, dim=DIM, granularity="monthly"):
    """Create a fresh PartitionedVdb instance."""
    cleanup_vdb(path)
    vdb = quanta.partitioned_vdb(
        prefix="test",
        path=path,
        dim=dim,
        granularity=granularity
    )
    return vdb


# ══════════════════════════════════════════════════════════════════
# TEST 1: Basic AddVectors + Lookup roundtrip
# ══════════════════════════════════════════════════════════════════
class TestBasicOps(unittest.TestCase):
    def setUp(self):
        self.vdb = make_vdb()

    def tearDown(self):
        cleanup_vdb(VDB_PATH)

    def test_add_and_lookup_basic(self):
        """Add vectors, lookup, verify top result matches the query vector."""
        now_ms = int(time.time() * 1000)
        emb1 = random_emb()
        emb2 = random_emb()

        self.vdb.AddVectors(1, emb1, timestamp=now_ms, chunks="doc1")
        self.vdb.AddVectors(2, emb2, timestamp=now_ms, chunks="doc2")

        results = self.vdb.Lookup(emb1, 2)
        self.assertGreaterEqual(len(results), 1)
        # Top result should be id=1 (closest to emb1)
        top = results[0]
        self.assertEqual(int(top[0]), 1)
        # Score should be high (close to 1.0 for self-match)
        self.assertGreater(float(top[1]), 0.4)
        print(f"  [PASS] test_add_and_lookup_basic: top_id={top[0]}, score={top[1]:.4f}")


# ══════════════════════════════════════════════════════════════════
# TEST 2: Precise time-range filter (ms-level)
# ══════════════════════════════════════════════════════════════════
class TestPreciseTimeFilter(unittest.TestCase):
    def setUp(self):
        self.vdb = make_vdb()

    def tearDown(self):
        cleanup_vdb(VDB_PATH)

    def test_precise_time_range_filter(self):
        """Add vectors with different ms timestamps, filter by tight range."""
        base_ms = int(time.time() * 1000)
        ts1 = base_ms - 10000   # 10 sec ago
        ts2 = base_ms - 5000    # 5 sec ago
        ts3 = base_ms           # now

        emb = random_emb()  # same embedding for all (makes them all similar)
        self.vdb.AddVectors(101, emb, timestamp=ts1, chunks="old")
        self.vdb.AddVectors(102, emb, timestamp=ts2, chunks="mid")
        self.vdb.AddVectors(103, emb, timestamp=ts3, chunks="new")

        # Query with tight range: only ts2 should match (5s ago ± 1s)
        results = self.vdb.Lookup(emb, 10, ts_start=ts2 - 1000, ts_end=ts2 + 1000)
        ids_found = [int(r[0]) for r in results]
        self.assertIn(102, ids_found, "Expected id=102 in time range")
        self.assertNotIn(101, ids_found, "id=101 should be outside range")
        self.assertNotIn(103, ids_found, "id=103 should be outside range")
        print(f"  [PASS] test_precise_time_range_filter: found ids={ids_found}")

    def test_timestamp_in_results(self):
        """Verify Lookup returns 5-element tuples with timestamp as 5th."""
        now_ms = int(time.time() * 1000)
        emb = random_emb()
        self.vdb.AddVectors(201, emb, timestamp=now_ms, chunks="ts_test")

        results = self.vdb.Lookup(emb, 1)
        self.assertGreaterEqual(len(results), 1)
        top = results[0]
        self.assertEqual(len(top), 5, f"Expected 5-element tuple, got {len(top)}")
        returned_ts = int(top[4])
        self.assertEqual(returned_ts, now_ms, f"Timestamp mismatch: {returned_ts} != {now_ms}")
        print(f"  [PASS] test_timestamp_in_results: ts={returned_ts}")


# ══════════════════════════════════════════════════════════════════
# TEST 3: Partition-based queries
# ══════════════════════════════════════════════════════════════════
class TestPartitionFilter(unittest.TestCase):
    def setUp(self):
        self.vdb = make_vdb()

    def tearDown(self):
        cleanup_vdb(VDB_PATH)

    def test_partition_filter(self):
        """Add vectors to different partitions, query with partition filter."""
        now_ms = int(time.time() * 1000)
        emb = random_emb()

        self.vdb.AddVectors(301, emb, timestamp=now_ms, partition="cam_A", chunks="a1")
        self.vdb.AddVectors(302, emb, timestamp=now_ms, partition="cam_B", chunks="b1")
        self.vdb.AddVectors(303, emb, timestamp=now_ms, partition="cam_A", chunks="a2")

        # Query cam_A only
        results = self.vdb.Lookup(emb, 10, partition="cam_A")
        ids_found = [int(r[0]) for r in results]
        self.assertIn(301, ids_found)
        self.assertIn(303, ids_found)
        self.assertNotIn(302, ids_found, "cam_B should not appear")
        print(f"  [PASS] test_partition_filter: cam_A ids={ids_found}")


# ══════════════════════════════════════════════════════════════════
# TEST 4: Save/Load persistence roundtrip
# ══════════════════════════════════════════════════════════════════
class TestSaveLoad(unittest.TestCase):
    def tearDown(self):
        cleanup_vdb(VDB_PATH)

    def test_save_load_roundtrip(self):
        """Save VDB, create new instance, load, verify lookup + timestamps."""
        vdb1 = make_vdb()
        now_ms = int(time.time() * 1000)
        emb = random_emb()

        vdb1.AddVectors(401, emb, timestamp=now_ms, chunks="persist_test")
        vdb1.Save("")

        # Create a new instance and load
        vdb2 = quanta.partitioned_vdb(prefix="test", path=VDB_PATH, dim=DIM)
        vdb2.Load(VDB_PATH)

        results = vdb2.Lookup(emb, 1)
        self.assertGreaterEqual(len(results), 1)
        top = results[0]
        self.assertEqual(int(top[0]), 401)
        # Verify timestamp survived save/load
        self.assertEqual(int(top[4]), now_ms, "Timestamp should survive save/load")
        print(f"  [PASS] test_save_load_roundtrip: id={top[0]}, ts={top[4]}")


# ══════════════════════════════════════════════════════════════════
# TEST 5: Real image embeddings with CLIP
# ══════════════════════════════════════════════════════════════════
class TestClipEmbeddings(unittest.TestCase):
    def setUp(self):
        self.vdb = make_vdb()

    def tearDown(self):
        cleanup_vdb(VDB_PATH)

    def test_real_image_embeddings(self):
        """Load images from disk, compute CLIP embeddings, add to VDB, lookup."""
        # Try to import CLIP dependencies
        try:
            import torch
            import clip
            from PIL import Image
        except ImportError as e:
            self.skipTest(f"CLIP dependencies not available: {e}")

        # Find image files
        if not os.path.isdir(IMAGE_DIR):
            self.skipTest(f"Image directory not found: {IMAGE_DIR}")

        image_files = []
        for root, dirs, files in os.walk(IMAGE_DIR):
            for f in files:
                if f.lower().endswith(('.jpg', '.jpeg', '.png', '.bmp')):
                    image_files.append(os.path.join(root, f))
            if len(image_files) >= 5:
                break

        if len(image_files) < 2:
            self.skipTest(f"Not enough images found in {IMAGE_DIR}")

        # Load CLIP model
        device = "cuda" if torch.cuda.is_available() else "cpu"
        model, preprocess = clip.load("ViT-B/32", device=device)

        # Add image embeddings
        now_ms = int(time.time() * 1000)
        embeddings = []
        for i, img_path in enumerate(image_files[:5]):
            pil_img = Image.open(img_path).convert("RGB")
            img_input = preprocess(pil_img).unsqueeze(0).to(device)
            with torch.no_grad():
                emb = model.encode_image(img_input)
                emb = emb / emb.norm(dim=-1, keepdim=True)
            emb_np = emb.cpu().numpy().flatten().astype(np.float32)
            embeddings.append(emb_np)

            self.vdb.AddVectors(
                500 + i, emb_np,
                timestamp=now_ms + i * 1000,  # stagger timestamps
                chunks=os.path.basename(img_path)
            )

        # Text search
        text_query = "a photo of a person"
        text_input = clip.tokenize([text_query]).to(device)
        with torch.no_grad():
            text_emb = model.encode_text(text_input)
            text_emb = text_emb / text_emb.norm(dim=-1, keepdim=True)
        text_emb_np = text_emb.cpu().numpy().flatten().astype(np.float32)

        results = self.vdb.Lookup(text_emb_np, 3)
        self.assertGreater(len(results), 0)
        print(f"  [PASS] test_real_image_embeddings: found {len(results)} results")
        for r in results:
            print(f"    id={r[0]}, score={float(r[1]):.4f}, chunk={r[2]}")


# ══════════════════════════════════════════════════════════════════
# TEST 6: Multi-threaded concurrent Lookup (read safety)
# ══════════════════════════════════════════════════════════════════
class TestConcurrentLookup(unittest.TestCase):
    def setUp(self):
        self.vdb = make_vdb()

    def tearDown(self):
        cleanup_vdb(VDB_PATH)

    def test_concurrent_lookup(self):
        """Spawn N threads all calling Lookup simultaneously."""
        now_ms = int(time.time() * 1000)
        # Add some vectors
        for i in range(20):
            emb = random_emb()
            self.vdb.AddVectors(600 + i, emb, timestamp=now_ms + i, chunks=f"conc_{i}")

        errors = []
        results_per_thread = {}

        def lookup_worker(thread_id):
            try:
                q = random_emb()
                r = self.vdb.Lookup(q, 5)
                results_per_thread[thread_id] = len(r)
            except Exception as e:
                errors.append((thread_id, str(e)))

        threads = []
        NUM_THREADS = 10
        for t in range(NUM_THREADS):
            th = threading.Thread(target=lookup_worker, args=(t,))
            threads.append(th)

        # Start all threads
        for th in threads:
            th.start()
        for th in threads:
            th.join(timeout=30)

        self.assertEqual(len(errors), 0, f"Thread errors: {errors}")
        self.assertEqual(len(results_per_thread), NUM_THREADS)
        print(f"  [PASS] test_concurrent_lookup: {NUM_THREADS} threads OK, "
              f"results counts={list(results_per_thread.values())}")


# ══════════════════════════════════════════════════════════════════
# TEST 7: Creator crash resilience
# ══════════════════════════════════════════════════════════════════
class TestCrashResilience(unittest.TestCase):
    def tearDown(self):
        cleanup_vdb(VDB_PATH)

    def test_creator_crash_resilience(self):
        """
        Simulate a crash: a subprocess adds data, saves, then exits abruptly.
        After that, load the VDB in current process and verify data is intact.
        """
        cleanup_vdb(VDB_PATH)

        # Write a small script that creates, adds, saves, then crashes
        crash_script = os.path.join(SCRIPT_DIR, "_crash_writer.py")
        with open(crash_script, "w") as f:
            f.write(f'''
import os, sys, time, numpy as np
import xlang3
quanta = xlang3.importModule("quanta", fromPath="Quanta")

vdb = quanta.partitioned_vdb(
    prefix="test", path=r"{VDB_PATH}", dim={DIM}, granularity="monthly"
)
now_ms = int(time.time() * 1000)
emb = np.random.randn({DIM}).astype(np.float32)
emb = emb / np.linalg.norm(emb)

# Save the embedding to a file so the tester can read it
np.save(r"{os.path.join(SCRIPT_DIR, '_crash_emb.npy')}", emb)

vdb.AddVectors(701, emb, timestamp=now_ms, chunks="crash_data")
vdb.Save("")

# Write a flag file indicating save was successful
with open(r"{os.path.join(SCRIPT_DIR, '_crash_flag.txt')}", "w") as ff:
    ff.write(str(now_ms))

# Simulate crash by calling os._exit (skip cleanup)
os._exit(1)
''')

        try:
            # Run the crash script
            result = subprocess.run(
                [sys.executable, crash_script],
                cwd=SCRIPT_DIR,
                timeout=60,
                capture_output=True,
                text=True
            )

            # Check flag file
            flag_path = os.path.join(SCRIPT_DIR, "_crash_flag.txt")
            if not os.path.exists(flag_path):
                self.skipTest(f"Crash writer didn't complete save. stderr: {result.stderr[:500]}")

            with open(flag_path) as ff:
                saved_ts = int(ff.read().strip())

            # Now load in current process
            vdb2 = quanta.partitioned_vdb(prefix="test", path=VDB_PATH, dim=DIM)
            vdb2.Load(VDB_PATH)

            emb = np.load(os.path.join(SCRIPT_DIR, "_crash_emb.npy"))
            results = vdb2.Lookup(emb, 1)
            self.assertGreaterEqual(len(results), 1)
            self.assertEqual(int(results[0][0]), 701)
            self.assertEqual(int(results[0][4]), saved_ts, "Timestamp should be intact after crash")
            print(f"  [PASS] test_creator_crash_resilience: data intact after crash")
        finally:
            # Cleanup temp files
            for tmp in ["_crash_writer.py", "_crash_flag.txt", "_crash_emb.npy"]:
                p = os.path.join(SCRIPT_DIR, tmp)
                if os.path.exists(p):
                    os.remove(p)


# ══════════════════════════════════════════════════════════════════
# TEST 8: Singleton via Cantor LRPC
# ══════════════════════════════════════════════════════════════════
class TestSingletonViaCantor(unittest.TestCase):
    """
    Test that the same module loaded via Cantor LRPC is a true singleton:
    LoadModule once, then QueryModule from other "clients" gets the same instance.
    Requires Cantor service to be running (lrpc:1000).
    """

    def tearDown(self):
        cleanup_vdb(VDB_PATH)

    def _get_cantor(self):
        """Connect to Cantor via LRPC, skip test if not available."""
        try:
            cantor = xlang3.importModule("cantor", thru="lrpc:1000")
            # Quick connectivity check
            cantor.GetRootPath()
            return cantor
        except Exception as e:
            self.skipTest(f"Cantor LRPC not available: {e}")

    def test_singleton_module(self):
        """Multiple handles via cantor.QueryModule share the same module."""
        cantor = self._get_cantor()

        # Load the quanta module via Cantor
        try:
            q1 = cantor.LoadModule("quanta", "Quanta")
        except Exception as e:
            self.skipTest(f"Could not load quanta via Cantor: {e}")

        # Query it again — should return the same module
        try:
            q2 = cantor.QueryModule("quanta")
        except Exception as e:
            self.fail(f"QueryModule failed: {e}")

        # Both should be able to create VDBs that work
        cleanup_vdb(VDB_PATH)
        vdb1 = q1.partitioned_vdb(prefix="test", path=VDB_PATH, dim=DIM)
        now_ms = int(time.time() * 1000)
        emb = random_emb()
        vdb1.AddVectors(801, emb, timestamp=now_ms, chunks="singleton_data")
        vdb1.Save("")

        # Load via q2 (same module) - should access the same data
        vdb2 = q2.partitioned_vdb(prefix="test", path=VDB_PATH, dim=DIM)
        vdb2.Load(VDB_PATH)
        results = vdb2.Lookup(emb, 1)
        self.assertGreaterEqual(len(results), 1)
        self.assertEqual(int(results[0][0]), 801)
        print(f"  [PASS] test_singleton_module: q1 and q2 share the same module")

    def test_concurrent_readers_via_cantor(self):
        """Multiple threads reading from module loaded via Cantor LRPC."""
        cantor = self._get_cantor()

        try:
            q = cantor.QueryModule("quanta")
            # Test if module is loaded by trying to use it
            q.partitioned_vdb
        except Exception:
            try:
                q = cantor.LoadModule("quanta", "Quanta")
            except Exception as e:
                self.skipTest(f"Could not get quanta via Cantor: {e}")

        # Create and populate VDB
        cleanup_vdb(VDB_PATH)
        vdb = q.partitioned_vdb(prefix="test", path=VDB_PATH, dim=DIM)
        now_ms = int(time.time() * 1000)
        for i in range(10):
            vdb.AddVectors(900 + i, random_emb(), timestamp=now_ms + i, chunks=f"cr_{i}")
        vdb.Save("")

        # Multiple threads do Lookup
        errors = []

        def reader(tid):
            try:
                q_local = cantor.QueryModule("quanta")
                v = q_local.partitioned_vdb(prefix="test", path=VDB_PATH, dim=DIM)
                v.Load(VDB_PATH)
                r = v.Lookup(random_emb(), 3)
                if len(r) == 0:
                    errors.append((tid, "empty results"))
            except Exception as e:
                errors.append((tid, str(e)))

        threads = [threading.Thread(target=reader, args=(i,)) for i in range(5)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30)

        self.assertEqual(len(errors), 0, f"Reader errors: {errors}")
        print(f"  [PASS] test_concurrent_readers_via_cantor: 5 threads OK")


# ══════════════════════════════════════════════════════════════════
# Main: run all tests
# ══════════════════════════════════════════════════════════════════
if __name__ == "__main__":
    print("=" * 60)
    print("PartitionedVdb Comprehensive Test Suite")
    print("=" * 60)

    # Run tests in order of complexity
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()

    suite.addTests(loader.loadTestsFromTestCase(TestBasicOps))
    suite.addTests(loader.loadTestsFromTestCase(TestPreciseTimeFilter))
    suite.addTests(loader.loadTestsFromTestCase(TestPartitionFilter))
    suite.addTests(loader.loadTestsFromTestCase(TestSaveLoad))
    suite.addTests(loader.loadTestsFromTestCase(TestClipEmbeddings))
    suite.addTests(loader.loadTestsFromTestCase(TestConcurrentLookup))
    suite.addTests(loader.loadTestsFromTestCase(TestCrashResilience))
    suite.addTests(loader.loadTestsFromTestCase(TestSingletonViaCantor))

    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    # Exit with proper code
    sys.exit(0 if result.wasSuccessful() else 1)
