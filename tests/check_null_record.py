import subprocess
import sys
try:
    result = subprocess.run(
        [sys.argv[1], sys.argv[2], "--null-record"],
        capture_output=True, text=True, timeout=120)
except subprocess.TimeoutExpired as exc:
    raise AssertionError("null-record lookup hung: %r" % (exc,))
assert result.returncode == 2, result
assert "DbgBreakPoint" in result.stderr, result.stderr
assert "0x8238A1EC" in result.stderr, result.stderr
assert "NULL RECORD SWALLOWED" not in (result.stderr + result.stdout), result
print("Empty record lookup aborts with diagnostics instead of a null record.")
