"""Non-rendering checks for the original menu registry override."""
from pathlib import Path
import struct
import sys
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_video_menu import (Registry, SETTINGS, KEYBOARD_ACTIONS, KEYBOARD_PRIMARY,
                               KEYBOARD_SECONDARY, compile_menu, compile_menu_archive)


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
            self.assertEqual(len(before.roots) + 5, len(after.roots))
            for original, edited in zip(before.roots, after.roots):
                if before.decode(original)[1] not in ("options_video", "options_controller",
                        "MainMenu_NoCheckpoint", "MainMenu_GotCheckpoint", "GAMEMENU_real"):
                    self.assertEqual(tree(before, original), tree(after, edited))

    def test_native_controls_and_navigation(self):
        for registry, _ in registries(self.output):
            page, = [n for n in registry.roots if registry.decode(n) == ("WINDOW", "options_video")]
            props = dict(registry.decode(c) for c in page.children)
            self.assertEqual(props["CLASSNAME"], "CubeMenu_VideoSettings")
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
            self.assertNotIn("darkrecomp.gamma", buttons)
            self.assertIn("darkrecomp.language", buttons)
            self.assertIn("darkrecomp.anisotropy", buttons)
            self.assertEqual(buttons[:2], ["darkrecomp.brightness", "darkrecomp.fov"])

    def test_exit_confirmation_and_keyboard_entry_preserve_original_controls(self):
        for (before, _), (after, _) in zip(registries(self.source), registries(self.output)):
            self.assertEqual(after.root_hashes[:len(before.roots) * 2], before.root_hashes[:len(before.roots) * 2])
            for index, root in enumerate(after.roots):
                h = 5381
                for c in after.decode(root)[0].lower().encode("latin1"):
                    h = (h * 33 + (c if c < 128 else c - 256)) & 0xffffffff
                self.assertEqual(struct.unpack_from("<H", after.root_hashes, index * 2)[0], (h - 5381) & 0xffff)
            for name in ("MainMenu_NoCheckpoint", "MainMenu_GotCheckpoint", "GAMEMENU_real", "options_controller"):
                original, = [n for n in before.roots if before.decode(n) == ("WINDOW", name)]
                edited, = [n for n in after.roots if after.decode(n) == ("WINDOW", name)]
                previous = [tree(before, n) for n in original.children]
                added = [n for n in edited.children if tree(after, n) not in previous]
                self.assertEqual(len(added), 1)
                self.assertEqual([tree(after, n) for n in edited.children if n not in added], previous)
                props = dict(after.decode(n) for n in added[0].children)
                self.assertEqual(props["CLASSNAME"], "CubeButton")
                self.assertEqual(props["SCRIPT_PRESSED"], "darkrecomp.keybindings" if name == "options_controller"
                                 else "cg_submenu('darkrecomp_exit_confirm')")
                x, y, w, h = map(int, props["RGN"].split(","))
                self.assertLessEqual(x + w, 20)
                self.assertLessEqual(y + h, 20)
            confirm, = [n for n in after.roots if after.decode(n) == ("WINDOW", "darkrecomp_exit_confirm")]
            props = dict(after.decode(n) for n in confirm.children)
            self.assertEqual(props["ACCELLERATOR_1"], "gui_cancel,,cg_prevmenu()")
            self.assertEqual(props["ACCELLERATOR_2"], "gui_back,,cg_prevmenu()")
            self.assertEqual(props["DEFAULTLOOKUP"], "", "CubeMenu requires its retail glyph/cell lookup")
            actions = [dict(after.decode(p) for p in n.children).get("SCRIPT_PRESSED")
                       for n in confirm.children if after.decode(n)[0] == "WINDOW"]
            self.assertEqual([a for a in actions if a], ["cg_prevmenu()", "darkrecomp.exit"])

    def test_generated_visible_menu_text_uses_caps(self):
        for (before, _), (after, _) in zip(registries(self.source), registries(self.output)):
            originals = {before.decode(root)[1]: root for root in before.roots}
            for root in after.roots:
                name = after.decode(root)[1]
                if name not in ("options_video", "options_controller", "MainMenu_NoCheckpoint",
                                "MainMenu_GotCheckpoint", "GAMEMENU_real", "darkrecomp_exit_confirm") and not name.startswith("darkrecomp_keyboard_"):
                    continue
                original_controls = [tree(before, n) for n in originals[name].children
                                     if before.decode(n)[0] == "WINDOW"] if name in originals else []
                generated = [n for n in root.children if after.decode(n)[0] == "WINDOW"
                             and tree(after, n) not in original_controls]
                for window in generated:
                    props = dict(after.decode(c) for c in window.children)
                    if "TEXT" in props:
                        token, text = props["TEXT"].split(",", 1)
                        self.assertIn(token, ("sc", "nc"), "control token must remain unchanged")
                        self.assertEqual(text, text.upper(), name)

    def test_keyboard_pages_cover_both_slots_and_preserve_same_level_navigation(self):
        for registry, _ in registries(self.output):
            covered = []
            for page_number in range(1, 5):
                page, = [n for n in registry.roots if registry.decode(n) ==
                         ("WINDOW", f"darkrecomp_keyboard_{page_number}")]
                props = dict(registry.decode(c) for c in page.children)
                self.assertEqual(props["CLASSNAME"], "CubeMenu")
                self.assertEqual(props["ID"], f"DARKRECOMP_KEYBOARD_{page_number}")
                self.assertEqual(props["DEFAULTLOOKUP"], "")
                self.assertEqual(props["STYLE"], "HIDDENFOCUS")
                self.assertEqual(props["RGN"], "0,0,640,480")
                self.assertEqual(props["ACCELLERATOR_1"], "gui_cancel,,cg_prevmenu()")
                self.assertEqual(props["ACCELLERATOR_2"], "gui_back,,cg_prevmenu()")
                controls = [dict(registry.decode(c) for c in window.children)
                            for window in page.children if registry.decode(window)[0] == "WINDOW"]
                columns = [("sc, ACTION", "1,6,9,1"), ("sc, PRIMARY", "10,6,5,1"),
                           ("sc, SECONDARY", "15,6,5,1")]
                for text, region in columns:
                    header, = [c for c in controls if c.get("TEXT") == text]
                    self.assertEqual((header["CLASSNAME"], header["RGN"]), ("CubeText", region))
                bindings = [c for c in controls if c.get("SCRIPT_PRESSED", "").startswith("darkrecomp.keyboard.bind.")]
                expected = [(page_number - 1) * 6 + row for row in range(6)]
                self.assertEqual([c["SCRIPT_PRESSED"] for c in bindings],
                    [f"darkrecomp.keyboard.bind.{action}.{slot}" for action in expected for slot in range(2)])
                for row, action in enumerate(expected):
                    label, = [c for c in controls if c.get("TEXT") == "sc, " + KEYBOARD_ACTIONS[action]]
                    self.assertEqual(label["RGN"], f"1,{7 + row},9,1")
                    for slot, x, width, values in ((0, 10, 5, KEYBOARD_PRIMARY), (1, 15, 5, KEYBOARD_SECONDARY)):
                        button = bindings[row * 2 + slot]
                        self.assertEqual(button["CLASSNAME"], "CubeButton")
                        self.assertEqual(button["ALWAYSPAINT"], "1")
                        self.assertNotIn("STYLE", button, "binding slots must retain original focusability")
                        self.assertEqual(button["RGN"], f"{x},{7 + row},{width},1")
                        self.assertEqual(button["TEXT"], "sc, [" + values[action].center(8) + "]")
                        self.assertEqual(len(button["TEXT"][4:]), width * 2,
                                         "initial and live key values must retain native hit rectangle width")
                        self.assertEqual(button["TEXT"][4:], button["TEXT"][4:].strip(),
                                         "retail initial TEXT sizing strips leading and trailing whitespace")
                        covered.append((action, slot))
                scripts = {c.get("SCRIPT_PRESSED"): c for c in controls if "SCRIPT_PRESSED" in c}
                previous, following = (page_number - 2) % 4 + 1, page_number % 4 + 1
                self.assertIn(f"cg_switchmenu('darkrecomp_keyboard_{previous}')", scripts)
                self.assertIn(f"cg_switchmenu('darkrecomp_keyboard_{following}')", scripts)
                self.assertFalse(any(script.startswith("cg_submenu") for script in scripts),
                                 "pagination must not stack four nested Back levels")
                for action in ("defaults", "save", "cancel"):
                    self.assertIn("darkrecomp.keyboard." + action, scripts)
                self.assertEqual(scripts["darkrecomp.keyboard.save"]["ALWAYSPAINT"], "1")
                self.assertFalse(any(c.get("ID") == "DARKRECOMP_KEYBOARD_STATUS" for c in controls))
                self.assertNotIn("darkrecomp.keyboard.status", scripts)
                self.assertIn(f"sc, PAGE {page_number}/4", [c.get("TEXT") for c in controls])
            self.assertEqual(covered, [(action, slot) for action in range(24) for slot in range(2)])

    def test_keyboard_columns_and_footer_fit_the_original_cell_grid(self):
        for registry, _ in registries(self.output):
            for page in [n for n in registry.roots if registry.decode(n)[1].startswith("darkrecomp_keyboard_")]:
                rows = {}
                for window in [n for n in page.children if registry.decode(n)[0] == "WINDOW"]:
                    fields = [registry.decode(c) for c in window.children]
                    props = dict(fields)
                    x, y, width, height = map(int, props["RGN"].split(","))
                    self.assertGreaterEqual(x, 0)
                    self.assertGreaterEqual(y, 0)
                    self.assertGreater(width, 0)
                    self.assertGreater(height, 0)
                    self.assertLessEqual(x + width, 20)
                    self.assertLessEqual(y + height, 20)
                    token, text = props["TEXT"].split(", ", 1)
                    self.assertIn(token, ("sc", "nc"))
                    self.assertLessEqual(len(text), width * (2 if token == "sc" else 1))
                    if "SCRIPT_PRESSED" in props:
                        order = [name for name, _ in fields]
                        self.assertLess(order.index("TEXT"), order.index("SCRIPT_PRESSED"))
                        self.assertLess(order.index("TEXT"), order.index("RGN"))
                    for row in range(y, y + height):
                        rows.setdefault(row, []).append((x, x + width))
                for row, spans in rows.items():
                    spans.sort()
                    for left, right in zip(spans, spans[1:]):
                        self.assertLessEqual(left[1], right[0], f"overlapping keyboard controls on row {row}")

    def test_original_gamma_controls_and_page_callbacks_are_preserved(self):
        for (before, _), (after, _) in zip(registries(self.source), registries(self.output)):
            source, = [n for n in before.roots if before.decode(n) == ("WINDOW", "options_video")]
            page, = [n for n in after.roots if after.decode(n) == ("WINDOW", "options_video")]
            original_properties = [before.decode(c) for c in source.children
                                   if before.decode(c)[0] != "WINDOW"]
            self.assertEqual(original_properties, [after.decode(c) for c in page.children
                                                   if after.decode(c)[0] != "WINDOW"])
            def gamma_nodes(registry, parent):
                return [n for n in parent.children if registry.decode(n)[0] == "WINDOW"
                        and dict(registry.decode(c) for c in n.children).get("GROUP") == "gamma"]
            original_gamma, gamma = gamma_nodes(before, source), gamma_nodes(after, page)
            self.assertEqual(len(gamma), 2)
            self.assertEqual([dict(after.decode(c) for c in n.children)["CLASSNAME"] for n in gamma],
                             ["CubeOptionButton", "CubeOptionMeter"])
            for old, new in zip(original_gamma, gamma):
                original_fields = [before.decode(c) for c in old.children if before.decode(c)[0] != "RGN"]
                fields = [after.decode(c) for c in new.children if after.decode(c)[0] != "RGN"]
                self.assertEqual(fields, original_fields)
                props = dict(after.decode(c) for c in new.children)
                self.assertEqual(props["OPTION"], "OPTG\\VIDEO_GAMMA")
                self.assertEqual(props["RGN"], "11,5,8,1")
                self.assertNotIn("SCRIPT_PRESSED", props)
            steps = dict(after.decode(c) for c in gamma[0].children)["STEPS"]
            self.assertEqual(steps[0], 5)
            self.assertEqual(struct.unpack(after.endian + "I", steps[1])[0], 12)
            texts = [after.decode(c)[1] for n in page.children for c in n.children
                     if after.decode(c)[0] == "TEXT"]
            self.assertIn("sc, §LMENU_GAMMA", texts)
            self.assertIn("sc, GAMMA: ORIGINAL GAME CALIBRATION", texts)
            self.assertNotIn("sc, Gamma: lower is darker; 1.00 is neutral", texts)

    def test_language_selection_is_visible_with_restart_help(self):
        for registry, _ in registries(self.output):
            page, = [n for n in registry.roots if registry.decode(n) == ("WINDOW", "options_video")]
            controls = [dict(registry.decode(c) for c in window.children)
                        for window in page.children if registry.decode(window)[0] == "WINDOW"]
            label, = [c for c in controls if c.get("TEXT") == "sc, LANGUAGE"]
            button, = [c for c in controls if c.get("SCRIPT_PRESSED") == "darkrecomp.language"]
            self.assertEqual(label["RGN"], "1,14,10,1")
            self.assertEqual(button["RGN"], "11,14,8,1")
            self.assertEqual(button["TEXT"], "sc, <    SYSTEM    >")
            self.assertEqual(button["CLASSNAME"], "CubeButton")
            self.assertEqual(button["ALWAYSPAINT"], "1")
            self.assertIn("sc, RESOLUTION/LANGUAGE: RESTART TO APPLY", [c.get("TEXT") for c in controls])

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
                if "TEXT" in props:
                    self.assertTrue(props["TEXT"].startswith("sc, "))
                    # The original Gamma label is a localization reference.
                    if not props["TEXT"].startswith("sc, §"):
                        self.assertLessEqual(len(props["TEXT"][4:]), width * 2)
                if props["CLASSNAME"] == "CubeButton":
                    order = [key for key, _ in fields]
                    self.assertLess(order.index("TEXT"), order.index("SCRIPT_PRESSED"))
                    self.assertLess(order.index("TEXT"), order.index("RGN"))
                    self.assertEqual((x, width), (11, 8))
                    self.assertEqual(len(props["TEXT"][4:]), width * 2,
                                     "initial value must reserve the same glyph width as live values")
                    self.assertTrue(props["TEXT"].startswith("sc, < ") and props["TEXT"].endswith(" >"))
                # The original meter paints inside its option button's region.
                if props["CLASSNAME"] != "CubeOptionMeter":
                    rows.setdefault(y, []).append((x, x + width))
            for y, intervals in rows.items():
                intervals.sort()
                for left, right in zip(intervals, intervals[1:]):
                    self.assertLessEqual(left[1], right[0], f"overlapping columns on row {y}")
            for y in range(4, 5 + len(SETTINGS)):
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
        self.assertEqual(dict(registry.decode(c) for c in page.children)["CLASSNAME"], "CubeMenu_VideoSettings")
        scripts = [registry.decode(prop)[1] for child in page.children for prop in child.children
                   if registry.decode(prop)[0] == "SCRIPT_PRESSED"]
        self.assertEqual(scripts, ["darkrecomp." + key for key in SETTINGS])


if __name__ == "__main__":
    unittest.main()
