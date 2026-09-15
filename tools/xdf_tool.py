import os
import sys
import zlib
import argparse

def inspect_xdf(path):
    with open(path, "rb") as f:
        data = f.read()

    ver = int.from_bytes(data[0:4], "little")
    str_len = int.from_bytes(data[4:8], "little")
    str_table = data[8:8+str_len]
    strings = [s.decode("ascii", errors="replace") for s in str_table.split(b"\0") if s]

    pos = 8 + str_len
    count = int.from_bytes(data[pos:pos+4], "little")
    pos += 4

    print(f"XDF Package: {path}")
    print(f"Format Version: {hex(ver)}")
    print(f"Dependencies/Entries: {count}")
    print(f"String Table Size: {str_len} bytes")
    print("-" * 60)

    entries = []
    for i in range(count):
        entry_raw = data[pos:pos+32]
        size = int.from_bytes(entry_raw[12:16], "little")
        name = strings[i] if i < len(strings) else f"entry_{i}"
        entries.append((i, name, size))
        pos += 32

    for idx, name, size in entries:
        print(f"  [{idx:3d}] {name:<45} {size:>10} bytes")

    # Search for zlib compressed payloads
    zlib_streams = []
    for i in range(pos, len(data) - 4):
        if data[i:i+2] in [b"\x78\x9c", b"\x78\x01", b"\x78\xda"]:
            try:
                decomp = zlib.decompress(data[i:])
                magic = decomp[:16].decode("ascii", errors="ignore").replace("\0", " ")
                zlib_streams.append((i, len(data) - i, len(decomp), magic))
                break
            except:
                pass

    if zlib_streams:
        print("\nPayload Streams:")
        for off, comp_sz, decomp_sz, magic in zlib_streams:
            print(f"  Stream at offset 0x{off:X}: compressed {comp_sz} bytes -> decompressed {decomp_sz} bytes [Header: {magic.strip()}]")

def main():
    parser = argparse.ArgumentParser(description="Starbreeze XDF package inspector")
    parser.add_argument("xdf_file", help="Path to .XDF file")
    args = parser.parse_args()

    if not os.path.exists(args.xdf_file):
        print(f"Error: {args.xdf_file} not found")
        return 1

    inspect_xdf(args.xdf_file)
    return 0

if __name__ == "__main__":
    sys.exit(main())
