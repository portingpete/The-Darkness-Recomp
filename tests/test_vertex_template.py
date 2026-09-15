"""Source-integrity and malformed-input checks for the offline translator."""
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from compile_vertex_template import ASSETS, parse, translate
from compile_world_template import translate as translate_world


class VertexTemplateTests(unittest.TestCase):
    def test_original_assets_translate_deterministically(self):
        first = translate(ROOT / 'Darkness')
        self.assertEqual(first, translate(ROOT / 'Darkness'))
        self.assertEqual(len(first[1]['instructions']), 246)
        self.assertNotIn('$', first[0])
        self.assertNotIn('@', first[0])

    def test_changed_original_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)
            for name in ASSETS:
                target = game / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / 'Darkness' / name, target)
            changed = game / 'System/Gl/VP.xrg'
            changed.write_bytes(changed.read_bytes().replace(b'DP4 @O_HPOS.x', b'DP3 @O_HPOS.x'))
            with self.assertRaisesRegex(ValueError, 'Original asset changed'):
                translate(game)

    def test_original_world_assets_translate_deterministically(self):
        first = translate_world(ROOT / 'Darkness')
        self.assertEqual(first, translate_world(ROOT / 'Darkness'))
        self.assertEqual(len(first[1]['instructions']), 603)
        self.assertNotIn('$', first[0])
        self.assertNotIn('@', first[0])

    def test_world_lighting_branch_selection(self):
        from compile_world_template import condition, MODES
        self.assertEqual(MODES['Lighting_Nonormal'], 16)
        for stage in ('3', '4', '5'):
            self.assertEqual(condition(f'texgen{stage}_Lighting_Nonormal'), f'MODE_{stage} == 16')
        for stage in ('0', '1', '2', '6', '7'):
            self.assertFalse(condition(f'texgen{stage}_Lighting_Nonormal'))
        self.assertEqual(condition('texgen5_constant'), '(MODE_5 == 7 || MODE_5 == 9)')
        manifest = translate_world(ROOT / 'Darkness')[1]
        self.assertEqual(manifest['constant_mode9_stages'], [3, 4, 5])
        for rec in (323, 324, 325, 326, 327, 328):
            self.assertIn(rec, manifest['cache_evidence_records'])

    def test_world_position_branch_uses_post_skin_position(self):
        from compile_world_template import condition, MODES
        self.assertEqual(MODES['wspos'], 10)
        for stage in range(8):
            self.assertEqual(condition(f'texgen{stage}_wspos'), 'MODE_1 == 10' if stage == 1 else False)
        code, manifest = translate_world(ROOT / 'Darkness')
        self.assertEqual(manifest['wspos_mode10_stages'], [1])
        block = code.split('#if MODE_1 == 10\n', 1)[1].split('#endif', 1)[0]
        self.assertEqual(block.splitlines(), [
            'R3.x = DP4(R8, c[refs[2].z+0]).x;',
            'R3.y = DP4(R8, c[refs[2].z+1]).y;',
            'R3.z = DP4(R8, c[refs[2].z+2]).z;',
            'R3.w = MOV(c[0+8].y).w;',
        ])

    def test_world_position_mapping_in_original_cache(self):
        # Offline evidence only. Verify the actual original ALU words rather
        # than treating an enum name as proof. No bytecode enters the runtime.
        # Xenos ALU layout/opcodes: xenia-project/xenia src/xenia/gpu/ucode.h,
        # blob 83719b7a90e85d989974cf56552ce2edec3d561b. DP4=15, MAX=2.
        cache = (ROOT / 'Darkness/System/Xenon/ProgramCache.xpc').read_bytes()
        big = lambda at: struct.unpack_from('>I', cache, at)[0]
        self.assertEqual(big(0x60), 442)
        wanted = {18: (0, 1, 2, 0x00A7A700), 250: (0, 1, 2, 0x00A7A700),
                  253: (4, 3, 4, 0x00003E00), 423: (8, 5, 6, 0x00003E00)}
        manifest = translate_world(ROOT / 'Darkness')[1]
        offset = 0x64
        for record in range(big(0x60)):
            length = big(offset + 37)
            self.assertLessEqual(offset + 41 + length, len(cache))
            if record in wanted:
                weights, dest, position, swizzle = wanted[record]
                self.assertIn(record, manifest['cache_evidence_records'])
                self.assertEqual((big(offset + 21) >> 16) & 15, weights)
                self.assertEqual(cache[offset + 25:offset + 33], bytes((4, 10, 4, 4, 4, 4, 4, 4)))
                blob = cache[offset + 41:offset + 41 + length]
                # Consecutive DP4 xyz from the position register and c12..14.
                dots = b''.join(struct.pack('>III', 0xC8000000 | ((1 << lane) << 16) | dest,
                                           swizzle, 0x8F000000 | (position << 16) | ((12 + lane) << 8))
                                for lane in range(3))
                self.assertEqual(blob.count(dots), 1)
                # Scalar MAX c8.y,c8.y sets W; MAX rN,rN exports the completed
                # vector to the sole live interpolator (the stage1 output).
                one = struct.pack('>III', 0x14800000 | (dest << 8), 0xB1, 0xC2000008)
                export = struct.pack('>III', 0xC80F8000, 0, 0xC2000000 | (dest << 16) | (dest << 8))
                self.assertEqual(blob.count(one), 1)
                self.assertEqual(blob.count(export), 1)
                self.assertLess(blob.index(dots), blob.index(one))
                self.assertLess(blob.index(one), blob.index(export))
            offset += 41 + length

    def test_quoted_comments_are_code_and_outer_comments_are_ignored(self):
        tree = parse('/* *bogus { */ *Root { // outside\n *Code "// inside { }" }')
        self.assertEqual(tree[0][1][0][1], '// inside { }')

    def test_comments_can_follow_unquoted_values(self):
        for comment in ('/* outside */', '// outside', '/* outside\n *Ignored node */'):
            with self.subTest(comment=comment):
                tree = parse(f'*Root {{ *Value token{comment}\n *Path System/Gl/VP.xrg }}')
                self.assertEqual(tree, [('Root', [
                    ('Value', 'token', 1),
                    ('Path', 'System/Gl/VP.xrg', 2 + comment.count('\n')),
                ], 1)])

    def test_malformed_template_rejected(self):
        for source in ('*Root {', '}', '*Root', '*Root { *Child }', '*Root "unterminated'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                parse(source)


if __name__ == '__main__':
    unittest.main()
