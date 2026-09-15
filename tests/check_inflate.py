"""Compare original AOT game decompression with independent Python zlib output."""
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import zlib

rng = random.Random(39172)
payload = (b"Native Windows game decompression regression. " * 1500
           + bytes(rng.randrange(256) for _ in range(8000)) + bytes(range(256)) * 100)
with tempfile.TemporaryDirectory(prefix="darkrecomp-inflate-") as directory:
    fixture = Path(directory) / "streams.bin"
    with fixture.open("wb") as output:
        for level, strategy in ((0, zlib.Z_DEFAULT_STRATEGY), (6, zlib.Z_FIXED), (6, zlib.Z_DEFAULT_STRATEGY)):
            compressor = zlib.compressobj(level, zlib.DEFLATED, 15, 8, strategy)
            compressed = compressor.compress(payload) + compressor.flush()
            output.write(struct.pack("<II", len(payload), len(compressed)))
            output.write(payload)
            output.write(compressed)
    result = subprocess.run([sys.argv[1], sys.argv[2], "--inflate", str(fixture)], timeout=30)
    sys.exit(result.returncode)
