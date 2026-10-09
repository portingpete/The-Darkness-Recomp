"""Catalog encoding, layout limits and shared native-menu dictionary contracts."""
from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_native_menu_text import DEFAULT_CATALOG, compile_header, load_catalog
from compile_video_menu import localized_text


class NativeMenuText(unittest.TestCase):
    def test_catalog_and_header_share_exact_cp1251_glyph_bytes(self):
        translations = load_catalog()
        self.assertEqual(translations["BRIGHTNESS"].encode("latin1"), "ЯРКОСТЬ".encode("cp1251"))
        self.assertEqual(translations["MOVE FORWARD"].encode("latin1"), "ВПЕРЁД".encode("cp1251"))
        header = compile_header(translations)
        self.assertTrue(header.isascii(), "generated C++ must not depend on compiler source encoding")
        entries = re.findall(r'\{"([^"\\]+)", "((?:\\x[0-9a-f]{2})+)"\}', header)
        self.assertEqual(len(entries), len(translations))
        for english, escaped in entries:
            encoded = bytes(int(value, 16) for value in re.findall(r"\\x([0-9a-f]{2})", escaped))
            self.assertEqual(encoded, translations[english].encode("latin1"), english)
        self.assertIn("return english;", header, "technical values must retain an English fallback")

    def test_dynamic_settings_and_keyboard_status_have_compact_translations(self):
        translations = load_catalog()
        for text in ("ON", "OFF", "ORIGINAL", "BORDERLESS", "WINDOWED", "UNLIMITED", "SYSTEM",
                     "ENGLISH", "GERMAN", "FRENCH", "SPANISH", "ITALIAN", "SAVE FAILED"):
            self.assertIn(text, translations)
            self.assertLessEqual(len(translations[text]), 12, text)
        for text in ("UNBOUND", "RELEASE", "PRESSKEY", "PRESS..."):
            self.assertIn(text, translations)
            self.assertLessEqual(len(translations[text]), 8, text)
        for text in ("SAVE APPLIES CHANGES. CANCEL DISCARDS.", "RELEASE HELD KEYS/BUTTONS, THEN PRESS.",
                     "PRESS A KEY/MOUSE; ESC CANCEL, DEL CLEAR", "RESERVED KEY. TRY AGAIN; ESC CANCELS.",
                     "BINDING UPDATED. SAVE TO APPLY.", "DEFAULTS STAGED. SAVE TO APPLY.",
                     "SAVING KEYBOARD/MOUSE BINDINGS...", "SAVE FAILED. TRY AGAIN OR CANCEL.",
                     "KEYBOARD/MOUSE BINDINGS SAVED."):
            self.assertIn(text, translations)
            self.assertLessEqual(len(translations[text]), 40, text)

    def test_catalog_rejects_unencodable_text_and_overflow_instead_of_truncating(self):
        valid = json.loads(DEFAULT_CATALOG.read_text(encoding="utf-8"))
        cases = (("text", "😀"), ("text", "яркость"), ("text", "ТЕСТ\n"),
                 ("text", "Я" * 21), ("max_glyphs", 0), ("max_glyphs", True))
        with tempfile.TemporaryDirectory(prefix="DarkRecomp menu catalog ") as temporary:
            path = Path(temporary) / "catalog.json"
            for field, value in cases:
                source = json.loads(json.dumps(valid))
                source["strings"]["BRIGHTNESS"][field] = value
                path.write_text(json.dumps(source, ensure_ascii=False), encoding="utf-8")
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    load_catalog(path)

    def test_generated_controls_reject_missing_text_and_region_overflow(self):
        translations = load_catalog()
        with self.assertRaisesRegex(ValueError, "Missing Russian"):
            localized_text("sc, NEW SETTING", "1,4,10,1", translations)
        without_initial = dict(translations)
        del without_initial["SYSTEM"]
        with self.assertRaisesRegex(ValueError, "Missing Russian"):
            localized_text("sc, <    SYSTEM    >", "11,4,8,1", without_initial)
        with self.assertRaisesRegex(ValueError, "region"):
            localized_text("nc, KEYBOARD BINDINGS", "0,4,5,1", translations)

    def test_header_cli_is_repeatable_and_never_overwrites_the_catalog(self):
        with tempfile.TemporaryDirectory(prefix="DarkRecomp menu header ") as temporary:
            path = Path(temporary) / "native_menu_text.generated.h"
            command = [sys.executable, str(ROOT / "tools/compile_native_menu_text.py"),
                       "--catalog", str(DEFAULT_CATALOG), "--output", str(path)]
            subprocess.run(command, check=True, capture_output=True)
            before = path.stat().st_mtime_ns
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(path.stat().st_mtime_ns, before, "unchanged catalog rewrote generated header")
            self.assertEqual(path.read_text(encoding="ascii"), compile_header(load_catalog()))
            before_catalog = DEFAULT_CATALOG.read_bytes()
            rejected = subprocess.run(command[:-1] + [str(DEFAULT_CATALOG)], capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual(DEFAULT_CATALOG.read_bytes(), before_catalog)


if __name__ == "__main__":
    unittest.main()
