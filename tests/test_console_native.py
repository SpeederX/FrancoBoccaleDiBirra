import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as folder:
    receipt = pathlib.Path(folder) / "console-result.txt"
    result = subprocess.run([sys.argv[1], str(receipt)], creationflags=subprocess.CREATE_NO_WINDOW,
                            capture_output=True, timeout=10)
    if result.returncode:
        raise RuntimeError(f"Native console exited {result.returncode}: {result.stderr.decode(errors='replace')}")
    print(receipt.read_text())
