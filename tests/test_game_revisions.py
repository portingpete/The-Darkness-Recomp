"""Check approved revision pairs and shared AOT contracts using synthetic XEXs."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import game_revisions
import native_imports
from xex_image import decode_xex


BASE = 0x82000000


def digest(data):
    return hashlib.sha256(data).hexdigest()


def fixture():
    header, image = bytearray(0x400), bytearray(0x1000)
    def be(offset, value):
        struct.pack_into('>I', header, offset, value)
    be(0, 0x58455832); be(8, len(header)); be(16, 0x80); be(20, 5)
    for index, (key, value) in enumerate(((0x3ff, 0x70), (0x10100, BASE + 0x400),
                                         (0x10201, BASE), (0x103ff, 0x240), (0x20104, 0x200))):
        be(24 + index * 8, key); be(28 + index * 8, value)
    be(0x70, 8); be(0x74, 0)
    be(0x84, len(image)); be(0x190, BASE)
    struct.pack_into('>IIII', header, 0x200, 64, BASE + 0x700, 8, 4)
    strings = b'xboxkrnl.exe\0\0\0\0'
    library_size = 48
    struct.pack_into('>III', header, 0x240, 12 + len(strings) + library_size, len(strings), 1)
    header[0x24c:0x24c + len(strings)] = strings
    library = 0x24c + len(strings)
    be(library, library_size)
    struct.pack_into('>HHII', header, library + 36, 0, 2, BASE + 0x800, BASE + 0x820)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 0x3c, 0x80)
    image[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', image, 0x84, 0x1f2, 3, 0, 0, 0, 0xe0, 0x102)
    struct.pack_into('<H', image, 0x98, 0x10b)
    for index, (name, rva, size, flags) in enumerate(((b'.text', 0x400, 0x40, 0x60000020),
                                                    (b'.pdata', 0x500, 0x10, 0x40000040),
                                                    (b'.rdata', 0x600, 0x80, 0x40000040))):
        offset = 0x178 + index * 40
        image[offset:offset + len(name)] = name
        struct.pack_into('<II', image, offset + 8, size, rva)
        struct.pack_into('<I', image, offset + 36, flags)
    image[0x400:0x404] = bytes.fromhex('4e800020')
    struct.pack_into('>I', image, 0x800, 0x42)
    struct.pack_into('>I', image, 0x820, 0x01000043)
    return bytes(header), bytes(image)


class GameRevisionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='darkrecomp-revisions-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'config').mkdir()
        self.header, self.image = fixture()
        self.catalog = self.root / 'config/game_revisions.toml'

    def write_catalog(self, revisions, fingerprint=None):
        if fingerprint is None:
            fingerprint = game_revisions.compatibility_sha256(self.header, self.image)
        text = f'format = 1\ncompatibility_sha256 = "{fingerprint}"\n'
        for name, xex, image in revisions:
            text += ('\n[[revision]]\n' + f'name = {json.dumps(name)}\n'
                     + f'xex_sha256 = "{digest(xex)}"\nimage_sha256 = "{digest(image)}"\n')
        self.catalog.write_text(text)

    def test_localization_changes_preserve_shared_profile(self):
        localized = bytearray(self.image)
        localized[0x607] = 0xff
        stock_xex = self.header + self.image
        localized_xex = self.header + localized
        self.write_catalog([('Original', stock_xex, self.image), ('Russian', localized_xex, localized)])
        for xex in (stock_xex, localized_xex):
            header, image = decode_xex(xex)
            self.assertEqual(len(game_revisions.validate_image(self.root, xex, header, image)), 2)

    def test_unknown_whole_file_is_rejected_despite_matching_code(self):
        approved = self.header + self.image
        self.write_catalog([('Original', approved, self.image)])
        localized = bytearray(self.image)
        localized[0x607] = 0xff
        with self.assertRaisesRegex(RuntimeError, 'Unsupported default.xex revision'):
            game_revisions.validate_image(self.root, self.header + localized, self.header, localized)

    def test_moving_xex_payload_offsets_preserves_contracts(self):
        moved = bytearray(self.header)
        moved[0x300:0x310] = self.header[0x200:0x210]
        struct.pack_into('>I', moved, 28 + 4 * 8, 0x300)
        self.assertEqual(game_revisions.compatibility_sha256(moved, self.image),
                         game_revisions.compatibility_sha256(self.header, self.image))

    def test_raw_and_decoded_hashes_must_match_the_same_record(self):
        xex = self.header + self.image
        wrong_image = bytearray(self.image)
        wrong_image[0x607] = 0xff
        self.write_catalog([('Original', xex, wrong_image)])
        with self.assertRaisesRegex(RuntimeError, 'differs from its approved revision'):
            game_revisions.validate_image(self.root, xex, self.header, self.image)

    def test_new_hash_pair_cannot_bypass_code_or_loader_contracts(self):
        baseline = game_revisions.compatibility_sha256(self.header, self.image)
        mutations = {
            'code': ('image', 0x403),
            'unwind metadata': ('image', 0x500),
            'section layout': ('image', 0x178 + 8),
            'entry point': ('header', 28 + 8),
            'TLS slots': ('header', 0x203),
            'TLS template': ('image', 0x700),
            'import ordinal': ('image', 0x803),
            'import thunk': ('image', 0x82f),
            'import library metadata': ('header', 0x25c + 12),
        }
        for name, (target, offset) in mutations.items():
            with self.subTest(contract=name):
                header, image = bytearray(self.header), bytearray(self.image)
                (header if target == 'header' else image)[offset] ^= 1
                xex = header + image
                self.write_catalog([('Changed', xex, image)], baseline)
                with self.assertRaisesRegex(RuntimeError, 'contracts differ'):
                    game_revisions.validate_image(self.root, xex, header, image)

    def test_malformed_layout_metadata_is_bounded(self):
        mutations = (
            ('header', 20, 0xffffffff, '>I'),  # optional-header count
            ('header', 28 + 3 * 8, 0xfffffff0, '>I'),  # imports outside header
            ('header', 0x244, 0xffffffff, '>I'),  # import string-table size
            ('header', 0x248, 0xffffffff, '>I'),  # library count
            ('header', 0x25c, 0xffffffff, '>I'),  # library size
            ('header', 0x25c + 38, 0xffff, '>H'),  # descriptor count
            ('header', 0x204, BASE - 4, '>I'),  # TLS template below image
            ('image', 0x3c, 0xffffffff, '<I'),  # PE header outside image
            ('image', 0x178 + 8, 0xffffffff, '<I'),  # code section size
        )
        for target, offset, value, form in mutations:
            with self.subTest(target=target, offset=offset):
                header, image = bytearray(self.header), bytearray(self.image)
                struct.pack_into(form, header if target == 'header' else image, offset, value)
                with self.assertRaises(ValueError):
                    game_revisions.compatibility_sha256(header, image)

    def test_duplicate_revision_identity_is_rejected(self):
        xex = self.header + self.image
        self.write_catalog([('Original', xex, self.image), ('Duplicate', xex, self.image)])
        with self.assertRaisesRegex(ValueError, 'Duplicate game revision'):
            game_revisions.load_catalog(self.catalog)

    def test_duplicate_optional_header_is_rejected(self):
        header = bytearray(self.header)
        struct.pack_into('>I', header, 24 + 4 * 8, 0x103ff)
        with self.assertRaisesRegex(ValueError, 'Duplicate XEX optional header'):
            game_revisions.compatibility_sha256(header, self.image)

    def test_generation_emits_all_approved_records_for_either_source(self):
        localized = bytearray(self.image)
        localized[0x607] = 0xff
        stock_xex, localized_xex = self.header + self.image, self.header + localized
        self.write_catalog([('Original', stock_xex, self.image), ('Russian', localized_xex, localized)])
        out = self.root / 'generated'
        out.mkdir()
        (out / 'ppc_recomp_shared.h').write_text('PPC_EXTERN_FUNC(__imp__FixtureCall)\n')
        (self.root / 'runtime/native').mkdir(parents=True)
        exports = self.root / 'refs/UnleashedRecomp/tools/XenonRecomp/XenonUtils/xbox'
        exports.mkdir(parents=True)
        for library in ('xam', 'xboxkrnl'):
            (exports / f'{library}_table.inc').write_text('XE_EXPORT(module, 0x42, FixtureData, type)\n')
        (self.root / 'Darkness').mkdir()
        for xex, image in ((stock_xex, self.image), (localized_xex, localized)):
            with self.subTest(source=digest(xex)):
                (self.root / 'Darkness/default.xex').write_bytes(xex)
                native_imports.generate(self.root, out)
                metadata = (out / 'ppc_image_metadata.h').read_text()
                self.assertIn('DarkRecomp::Native::GameRevision kGameRevisions[]', metadata)
                self.assertIn(digest(stock_xex), metadata)
                self.assertIn(digest(localized_xex), metadata)
                self.assertIn(f'kGameImageSha256[] = "{digest(image)}"', metadata)
                self.assertIn('{0x82000800, "FixtureData"}', metadata)

    def test_unknown_source_is_rejected_before_decode(self):
        self.write_catalog([('Original', self.header + self.image, self.image)])
        (self.root / 'Darkness').mkdir()
        (self.root / 'Darkness/default.xex').write_bytes(b'unapproved executable')
        with patch.object(game_revisions, 'decode_xex') as decode:
            with self.assertRaisesRegex(RuntimeError, 'Unsupported default.xex revision'):
                game_revisions.validate_source(self.root)
            decode.assert_not_called()


if __name__ == '__main__':
    unittest.main()
