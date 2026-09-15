import os
import sys
import argparse

def inspect_xtc(path):
    if not os.path.exists(path):
        print(f"Error: {path} not found")
        return 1

    with open(path, "rb") as f:
        data = f.read()

    pos = data.rfind(b"IMAGEDIRECTORY5")
    if pos == -1:
        print(f"Error: Could not find IMAGEDIRECTORY5 footer in {path}")
        return 1

    dir_off = int.from_bytes(data[pos+32:pos+36], "little")
    dir_size = int.from_bytes(data[pos+36:pos+40], "little")
    dir_count = int.from_bytes(data[pos+40:pos+44], "little")

    print(f"XTC Texture Container: {path}")
    print(f"File Size: {len(data):,} bytes")
    print(f"IMAGEDIRECTORY5 Table Offset: 0x{dir_off:08X} ({dir_size:,} bytes)")
    print(f"Reported Texture Count: {dir_count}")
    print("=" * 70)

    marker = b"\xff\xff\xff\xff\x00\x00\x00\x00"
    records = []
    idx = dir_off
    while idx < pos:
        idx = data.find(marker, idx)
        if idx == -1 or idx >= pos:
            break
        str_len = int.from_bytes(data[idx+8:idx+12], "little")
        name = data[idx+12:idx+12+str_len].decode("ascii", errors="ignore").rstrip("\0 ")
        records.append((idx, name))
        idx += 8

    print(f"Successfully enumerated {len(records)} texture records:\n")
    for i, (addr, name) in enumerate(records[:30]):
        print(f"  [{i:3d}] At 0x{addr:08X}: {name}")
    if len(records) > 30:
        print(f"  ... and {len(records) - 30} more textures.")
    return 0

def main():
    parser = argparse.ArgumentParser(description="Starbreeze XTC Texture Container Inspector")
    parser.add_argument("xtc_file", nargs="?", default="Darkness/Content/Textures/GUI.xtc", help="Path to .xtc file")
    args = parser.parse_args()
    return inspect_xtc(args.xtc_file)

if __name__ == "__main__":
    sys.exit(main())
