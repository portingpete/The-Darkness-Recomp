"""Non-rendering checks for the original menu registry override."""
from pathlib import Path
import struct
import sys
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_video_menu import Registry, SETTINGS, compile_menu, compile_menu_archive


def registries(data):
    for entry, endian in ((0x30, "<"), (0x60, ">")):
        offset, size = struct.unpack_from("<II", data, entry + 32)
        yield Registry(data[offset:offset + size], endian), bytes(data[offset:offset + size])


def tree(registry, node):
    return registry.decode(node), tuple(tree(registry, c) for c in node.children)


def archive_reads(data):
    """Decode the file/read tables independently of the menu packager."""
    string_bytes = int.from_bytes(data[4:8], "little")
    strings = data[8:8 + string_bytes]
    position = 8 + string_bytes
    count = int.from_bytes(data[position:position + 4], "little")
    position += 4
    files = [struct.unpack_from("<6I", data, position + i * 24) for i in range(count)]
    position += count * 24
    count = int.from_bytes(data[position:position + 4], "little")
    position += 4
    blocks = [struct.unpack_from("<5I", data, position + i * 20) for i in range(count)]
    payload = zlib.decompress(data[position + count * 20:])
    result = {}
    for file_index, record in enumerate(files):
        name = strings[record[0]:].split(b"\0", 1)[0].decode("ascii")
        reads = []
        block_index = record[1]
        while block_index != 0xffffffff:
            assert len(reads) <= len(blocks)
            following, owner, length, file_offset, payload_offset = blocks[block_index]
            assert owner == file_index
            chunk = payload[payload_offset:payload_offset + length]
            assert len(chunk) == length and file_offset + length <= record[3]
            reads.append((file_offset, chunk))
            if following == 0xffffffff:
                assert block_index == record[2]
            block_index = following
        result[name] = (record[3:], reads)
    return result


class VideoMenu(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "Darkness/Content/Gui/CubeWnd.xcr").read_bytes()
        cls.output = compile_menu(cls.source)
        cls.archive = (ROOT / "Darkness/Content/Xdf/GameContext_Create.XDF").read_bytes()

    def test_original_round_trip_and_untouched_pages(self):
        for (before, raw), (after, _) in zip(registries(self.source), registries(self.output)):
            self.assertEqual(before.encode(), raw)
            self.assertEqual(len(before.roots), len(after.roots))
            for original, edited in zip(before.roots, after.roots):
                if before.decode(original) != ("WINDOW", "options_video"):
                    self.assertEqual(tree(before, original), tree(after, edited))

    def test_native_controls_and_navigation(self):
        for registry, _ in registries(self.output):
            page, = [n for n in registry.roots if registry.decode(n) == ("WINDOW", "options_video")]
            props = dict(registry.decode(c) for c in page.children)
            self.assertEqual(props["CLASSNAME"], "CubeMenu")
            self.assertEqual(props["ACCELLERATOR_1"], "gui_cancel,,cg_prevmenu()")
            self.assertEqual(props["ACCELLERATOR_2"], "gui_back,,cg_prevmenu()")
            buttons = []
            for child in page.children:
                if registry.decode(child)[0] != "WINDOW":
                    continue
                props = dict(registry.decode(c) for c in child.children)
                if props["CLASSNAME"] == "CubeButton":
                    self.assertEqual(props["ALWAYSPAINT"], "1")
                    self.assertIn("RGN", props)
                    buttons.append(props["SCRIPT_PRESSED"])
            self.assertEqual(buttons, ["darkrecomp." + key for key in SETTINGS])
            self.assertIn("darkrecomp.antialiasing", buttons)
            self.assertIn("darkrecomp.gamma", buttons)
            self.assertEqual(buttons[:2], ["darkrecomp.brightness", "darkrecomp.gamma"])

    def test_value_columns_and_help_fit_the_original_cell_grid(self):
        for registry, _ in registries(self.output):
            page, = [n for n in registry.roots if registry.decode(n) == ("WINDOW", "options_video")]
            windows = [n for n in page.children if registry.decode(n)[0] == "WINDOW"]
            rows = {}
            for window in windows:
                fields = [registry.decode(c) for c in window.children]
                props = dict(fields)
                x, y, width, height = map(int, props["RGN"].split(","))
                self.assertGreater(width, 0)
                self.assertLessEqual(x + width, 20)
                self.assertLessEqual(y + height, 20)
                if y == 2:
                    continue # Original localized heading.
                self.assertEqual(height, 1)
                self.assertTrue(props["TEXT"].startswith("sc, "))
                self.assertLessEqual(len(props["TEXT"][4:]), width * 2)
                if props["CLASSNAME"] == "CubeButton":
                    order = [key for key, _ in fields]
                    self.assertLess(order.index("TEXT"), order.index("SCRIPT_PRESSED"))
                    self.assertLess(order.index("TEXT"), order.index("RGN"))
                    self.assertEqual((x, width), (11, 8))
                    self.assertEqual(len(props["TEXT"][4:]), width * 2,
                                     "initial value must reserve the same glyph width as live values")
                    self.assertTrue(props["TEXT"].startswith("sc, < ") and props["TEXT"].endswith(" >"))
                rows.setdefault(y, []).append((x, x + width))
            for y, intervals in rows.items():
                intervals.sort()
                for left, right in zip(intervals, intervals[1:]):
                    self.assertLessEqual(left[1], right[0], f"overlapping columns on row {y}")
            for y in range(4, 4 + len(SETTINGS)):
                self.assertEqual(rows[y], [(1, 11), (11, 19)])

    def test_chunk_bounds_and_original_hash_algorithm(self):
        for registry, _ in registries(self.output):
            visited = set()
            def visit(n):
                if not n.flags & 1 or (n.chunk, n.descriptor >> 16) in visited:
                    return
                visited.add((n.chunk, n.descriptor >> 16))
                a, b, c = registry.headers[n.chunk]
                stride = 4 if n.descriptor & 1 else 3
                end = (n.descriptor >> 16) + (len(n.children) + 1) // 2 + len(n.children) * stride
                self.assertLessEqual(end, a * 4 + b * 3 + c)
                for i, child in enumerate(n.children):
                    h = 5381
                    for char in registry.decode(child)[0].lower().encode("latin1"):
                        h = (h * 33 + (char if char < 128 else char - 256)) & 0xffffffff
                    self.assertEqual(struct.unpack_from("<H", n.hashes, i * 2)[0], (h - 5381) & 0xffff)
                    if child.flags & 1:
                        self.assertEqual(stride, 4)
                    visit(child)
            for root in registry.roots:
                visit(root)

    def test_startup_archive_serves_the_native_menu(self):
        original = archive_reads(self.archive)
        packed = archive_reads(compile_menu_archive(self.archive, self.source, self.output))
        self.assertEqual(original.keys(), packed.keys())
        for name in original:
            if name != "gui\\cubewnd.xcr":
                self.assertEqual(original[name], packed[name], name)
        metadata, reads = packed["gui\\cubewnd.xcr"]
        self.assertEqual(metadata[0], len(self.output))
        self.assertEqual(metadata[1:], original["gui\\cubewnd.xcr"][0][1:])

        def read(offset, length):
            for start, chunk in reads:
                if start <= offset and offset + length <= start + len(chunk):
                    return chunk[offset - start:offset - start + length]
            self.fail("startup cache is missing a menu read")
        # The engine reads the MOS header, directory, then XCR_BE entirely
        # from this archive. A loose-file-only edit leaves the old page here.
        header = read(0, 0x90)
        self.assertEqual(read(0, 0x30), header[:0x30])
        offset, size = struct.unpack_from("<II", header, 0x80)
        payload = read(offset, size)
        self.assertEqual(payload, self.output[offset:offset + size])
        registry = Registry(payload, ">")
        page, = [n for n in registry.roots if registry.decode(n) == ("WINDOW", "options_video")]
        self.assertEqual(dict(registry.decode(c) for c in page.children)["CLASSNAME"], "CubeMenu")
        scripts = [registry.decode(prop)[1] for child in page.children for prop in child.children
                   if registry.decode(prop)[0] == "SCRIPT_PRESSED"]
        self.assertEqual(scripts, ["darkrecomp." + key for key in SETTINGS])


if __name__ == "__main__":
    unittest.main()
