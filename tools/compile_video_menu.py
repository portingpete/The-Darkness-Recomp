"""Build a private CubeWnd override using the game's original menu classes.

XCR 0x203 stores little-endian node records/hashes and separate LE/BE value
pools. Child lists are shared by their word offset in the chunk allocation.
Keep the shipped pools and shared lists intact; append only the PC page.
"""
import argparse
from dataclasses import dataclass
from pathlib import Path
import struct
import zlib


@dataclass
class Node:
    chunk: int
    flags: int
    descriptor: int = 0
    hashes: bytes = b""
    children: list | None = None


class Registry:
    def __init__(self, data, endian):
        self.endian = endian
        offset = 0

        def word():
            nonlocal offset
            result = struct.unpack_from("<I", data, offset)[0]
            offset += 4
            return result

        def hashes(count):
            nonlocal offset
            size = ((count + 1) & ~1) * 2
            result = data[offset:offset + size]
            offset += size
            return result

        self.headers = [[word(), word(), word()] for _ in range(word())]
        count = word()
        self.root_hashes = hashes(count)
        shared = {}

        def node():
            result = Node(word(), word(), children=[])
            if result.flags & 1:
                result.descriptor = word()
                key = result.chunk, result.descriptor >> 16
                count = (result.descriptor & 0xffff) >> 1
                if key not in shared:
                    hs, children = hashes(count), []
                    shared[key] = hs, children
                    children.extend(node() for _ in range(count))
                result.hashes, result.children = shared[key]
            return result

        self.roots = [node() for _ in range(count)]
        self.pools = []
        for _ in self.headers:
            size = word()
            self.pools.append(bytearray(data[offset:offset + size]))
            offset += size
        assert offset == len(data)
        self.names = {}
        visited = set()

        def names(n):
            self.names[self.decode(n)[0]] = n.flags & 0xfffe
            if id(n.children) not in visited:
                visited.add(id(n.children))
                for child in n.children:
                    names(child)
        for root in self.roots:
            names(root)

    def decode(self, n):
        pool = self.pools[n.chunk]
        half = lambda p: struct.unpack_from(self.endian + "H", pool, p)[0]
        string = lambda p: bytes(pool[p:pool.index(0, p)]).decode("latin1")
        name = string((n.flags & 0xfffe) * 2 + 2)
        value = (n.flags >> 16) * 2
        kind = (half(value) >> 1) & 15
        if kind == 1:
            value = string(half(half(value + 2) * 2) * 4 + 2)
        elif kind == 0:
            value = ""
        else:
            value = (kind, bytes(pool[half(value + 2) * 2:half(value + 2) * 2 + 4]))
        return name, value

    def make(self, name, value, children=()):
        pool = self.pools[0]
        # CStr_static: tagged byte string, a 16-bit string handle, then the
        # registry's type/dimension header and pointer to that handle.
        pool.extend(b"\0" * (-len(pool) % 4))
        raw = len(pool)
        pool.extend(struct.pack(self.endian + "H", 2) + value.encode("latin1") + b"\0")
        pool.extend(b"\0" * (-len(pool) % 2))
        handle = len(pool)
        pool.extend(struct.pack(self.endian + "HHH", raw // 4, 2, handle // 2))
        assert len(pool) < 0x20000
        flags = self.names[name] | ((handle + 2) // 2 << 16)
        n = Node(0, flags, children=list(children))
        if children:
            self.set_children(n, children)
        return n

    def set_children(self, node, children):
        node.children = list(children)
        # Preserve authored child order; the hash table is parallel to the
        # child array (not sorted). Use the original case-insensitive hash.
        def hash_name(name):
            h = 5381
            for c in name.lower().encode("latin1"):
                h = (h * 33 + (c if c < 128 else c - 256)) & 0xffffffff
            return (h - 5381) & 0xffff
        node.hashes = b"".join(struct.pack("<H", hash_name(self.decode(c)[0])) for c in children)
        node.hashes += b"\0" * (-len(node.hashes) % 4)
        a, b, c = self.headers[0]
        offset = a * 4 + b * 3 + c
        stride = int(any(ch.flags & 1 for ch in children))
        self.headers[0][0 if stride else 1] += len(children)
        self.headers[0][2] += (len(children) + 1) // 2
        assert offset < 65536
        node.flags |= 1
        node.descriptor = (offset << 16) | len(children) << 1 | stride

    def encode(self):
        word = lambda n: struct.pack("<I", n)
        data = bytearray(word(len(self.headers)))
        for header in self.headers:
            data.extend(struct.pack("<III", *header))
        data.extend(word(len(self.roots)) + self.root_hashes)
        written = set()

        def node(n):
            data.extend(word(n.chunk) + word(n.flags))
            if n.flags & 1:
                data.extend(word(n.descriptor))
                key = n.chunk, n.descriptor >> 16
                if key not in written:
                    written.add(key)
                    data.extend(n.hashes)
                    for child in n.children:
                        node(child)
        for root in self.roots:
            node(root)
        for pool in self.pools:
            data.extend(word(len(pool)) + pool)
        return data


SETTINGS = ("brightness", "gamma", "fov", "bloom", "vsync", "fps", "resolution", "mode", "motionblur", "antialiasing")
SETTING_NAMES = ("Brightness", "Gamma", "Field of view", "Bloom", "Vertical sync", "Frame limit", "Resolution", "Display", "Motion blur", "Antialiasing")
INITIAL_VALUES = ("100%", "1.00", "Original", "On", "Off", "60", "720p", "Borderless", "Off", "Off")


def replace_video_page(registry):
    r = registry
    page, = [n for n in r.roots if r.decode(n) == ("WINDOW", "options_video")]
    template, = [n for n in r.roots if r.decode(n) == ("WINDOW", "options_video_dev")]
    properties = [c for c in template.children if r.decode(c)[0] not in
                  ("WINDOW", "ACCELLERATOR_3", "ACCELLERATOR_4", "ACCELLERATOR_5", "ACCELLERATOR_6")]
    def window(cls, text, region, script=None):
        children = [r.make("CLASSNAME", cls), r.make("TEXT", text)]
        if script:
            children.append(r.make("SCRIPT_PRESSED", script))
            children.append(r.make("ALWAYSPAINT", "1"))
        # Preserve the original control's property order and native sizing.
        children.append(r.make("RGN", region))
        return r.make("WINDOW", "", children)
    properties.append(window("CubeText", "nc, §LMENU_VIDEO_HEADING", "0,2,20,2"))
    for row, (key, name, initial) in enumerate(zip(SETTINGS, SETTING_NAMES, INITIAL_VALUES)):
        y = 4 + row
        properties.append(window("CubeText", "sc, " + name, f"1,{y},10,1"))
        properties.append(window("CubeButton", "sc, < " + initial.center(12) + " >", f"11,{y},8,1", "darkrecomp." + key))
    properties.append(window("CubeText", "sc, Brightness: 100% is neutral", "0,15,20,1"))
    properties.append(window("CubeText", "sc, Gamma: lower is darker; 1.00 is neutral", "0,16,20,1"))
    properties.append(window("CubeText", "sc, Left/Right: change; restart resolution", "0,18,20,1"))
    r.set_children(page, properties)


def compile_menu(source):
    data = bytearray(source)
    assert data[:15] == b"MOS DATAFILE2.0"
    payloads = []
    for entry, endian in ((0x30, "<"), (0x60, ">")):
        offset, size, version = struct.unpack_from("<III", data, entry + 32)
        assert version == 0x203
        r = Registry(data[offset:offset + size], endian)
        replace_video_page(r)
        payloads.append(r.encode())
    output = data[:0x90]
    for entry, payload in zip((0x30, 0x60), payloads):
        struct.pack_into("<II", output, entry + 32, len(output), len(payload))
        output.extend(payload)
    return output


def compile_menu_archive(source, original_menu, replacement):
    """Replace the three prefetched menu reads in GameContext_Create.XDF.

    XDF 0x101 has 24-byte file records, 20-byte read records and one zlib
    stream. A cached read records a file offset and a decompressed-stream
    offset separately. Preserve links, timestamps and every other cached byte.
    """
    version, string_size = struct.unpack_from("<II", source)
    assert version == 0x101
    strings = source[8:8 + string_size]
    file_count_at = 8 + string_size
    file_count, = struct.unpack_from("<I", source, file_count_at)
    files_at = file_count_at + 4
    block_count_at = files_at + file_count * 24
    block_count, = struct.unpack_from("<I", source, block_count_at)
    blocks_at = block_count_at + 4
    stream_at = blocks_at + block_count * 20
    header = bytearray(source[:stream_at])
    inflate = zlib.decompressobj()
    payload = inflate.decompress(source[stream_at:]) + inflate.flush()
    assert inflate.eof and not inflate.unused_data
    matches = []
    for index in range(file_count):
        record = struct.unpack_from("<6I", header, files_at + index * 24)
        name = strings[record[0]:].split(b"\0", 1)[0]
        if name.lower() == b"gui\\cubewnd.xcr":
            matches.append((index, record))
    (menu_index, menu_record), = matches
    assert menu_record[3] == len(original_menu)
    struct.pack_into("<I", header, files_at + menu_index * 24 + 12, len(replacement))
    original_be, original_be_size = struct.unpack_from("<II", original_menu, 0x80)
    replacement_be, replacement_be_size = struct.unpack_from("<II", replacement, 0x80)
    rewritten = bytearray()
    old_end = 0
    replaced = []
    for index in range(block_count):
        at = blocks_at + index * 20
        next_block, file_index, size, file_offset, stream_offset = struct.unpack_from("<5I", header, at)
        assert stream_offset == old_end
        chunk = payload[stream_offset:stream_offset + size]
        assert len(chunk) == size
        old_end = stream_offset + size
        if file_index == menu_index:
            assert menu_record[1] <= index <= menu_record[2]
            assert chunk == original_menu[file_offset:file_offset + size]
            if file_offset == 0 and size in (0x30, 0x90):
                chunk = replacement[:size]
            else:
                assert (file_offset, size) == (original_be, original_be_size)
                file_offset, size = replacement_be, replacement_be_size
                chunk = replacement[file_offset:file_offset + size]
            replaced.append(index)
        struct.pack_into("<5I", header, at, next_block, file_index, size, file_offset, len(rewritten))
        rewritten.extend(chunk)
    assert old_end == len(payload)
    assert replaced == list(range(menu_record[1], menu_record[2] + 1)) and len(replaced) == 3
    return header + zlib.compress(rewritten, level=9)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive-source", type=Path)
    parser.add_argument("--archive-output", type=Path)
    args = parser.parse_args()
    assert args.source.resolve() != args.output.resolve(), "Never overwrite the shipped registry"
    source = args.source.read_bytes()
    result = compile_menu(source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != result:
        args.output.write_bytes(result)
    if args.archive_source or args.archive_output:
        assert args.archive_source and args.archive_output
        assert args.archive_source.resolve() != args.archive_output.resolve()
        archive = compile_menu_archive(args.archive_source.read_bytes(), source, result)
        args.archive_output.parent.mkdir(parents=True, exist_ok=True)
        if not args.archive_output.exists() or args.archive_output.read_bytes() != archive:
            args.archive_output.write_bytes(archive)
