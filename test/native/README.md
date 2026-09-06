# Quanta Native Tests

These tests load the real Quanta shared library through XLang3's public SDK.
They use temporary directories, subprocess timeouts, and no external services.

- `quanta_sdk`: concurrent C++ insertion, separate runtimes, automatic shutdown,
  reopening, and native DFS serialization.
- `quanta_runtime`: Python syntax executed by XLang3; vector search, cosine,
  save/reload, partition routing, grouping, trackers, DFS, CPU tensors, strided
  inputs, queue draining, multi-bucket batches, and error reporting.
- `quanta_cpython_bridge`: the same database operations through CPython's
  `xlang3` bridge, including float32/float64 buffers.

From the CantorAI root, use the existing workspace build script:

```powershell
.\CantorAIWorkspace\dev\tools\Build\build_project.ps1 -Root $PWD -BuildType Release -CantorOnly -WithGalaxy -WithVega -WithQuanta -Target QuantaSdkTests
ctest --test-dir out/build/x64-Release/components/Quanta -C Release --output-on-failure
```

XLang3 import:

```python
from Quanta import quanta
db = quanta.partitioned_vdb(path="data", prefix="documents", dimension=3)
db.AddVectors(1, [1., 0., 0.], chunks="document")
db.Close()
```

`AddVectors` on a partitioned database queues an owned batch. Queries see it
after WAL merging; `Close` drains pending insertion before returning. Check
`GetHealth()["error"]` for background failures. Failed WAL merges keep their
files for recovery. Stream flushing is not a power-loss durability guarantee.

CPython import (with the bridge on `sys.path` and native libraries discoverable):

```python
import xlang3
quanta = xlang3.importModule("quanta", fromPath="Quanta")
```

Historical `.x` scripts are unchanged. Windows Release is the tested platform;
Linux/macOS builds and performance benchmarking are separate verification work.
