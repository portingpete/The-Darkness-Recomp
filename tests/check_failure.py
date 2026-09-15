import subprocess
import sys
result = subprocess.run([sys.argv[1], sys.argv[2], "--invalid-dispatch"], capture_output=True, text=True)
assert result.returncode == 2, result
assert "missing or invalid indirect function target" in result.stderr, result.stderr
assert "r3=0x11223344" in result.stderr, result.stderr
print("Missing dispatch aborts with diagnostics and preserves the input register.")
