import subprocess
import sys
result = subprocess.run([sys.argv[1], sys.argv[2], sys.argv[3]], capture_output=True, text=True)
assert result.returncode == 2, result
assert "native XMA decode failed" in result.stderr, result.stderr
assert "invalid PCM ring or subframe quota" in result.stderr, result.stderr
print("Malformed active XMA submission is rejected explicitly.")
