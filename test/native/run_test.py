"""Run native tests with isolated storage, captured output, and a hard timeout."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

executable, script = sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix="quanta-native-") as root:
    result = subprocess.run([executable, script, root, *sys.argv[3:]],
                            cwd=Path(executable).parent, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60,
                            env={**os.environ, "PYTHONUNBUFFERED": "1"})
    print(result.stdout, end="", flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
    if "quanta-runtime-passed" not in result.stdout:
        raise RuntimeError("Missing completion marker")
