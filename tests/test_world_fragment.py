"""Contract checks for translation of the original world fragment programs."""
from pathlib import Path
import hashlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from compile_world_fragment import ASSETS, VARIANTS, compile_source, compile_fixed, select_template


class WorldFragmentTests(unittest.TestCase):
    def test_darkness_vision_permutations_and_xenon_depth(self):
        directory = ROOT / 'Darkness/System/Gl/ARB_fragment_program'
        for stage in (0, 1):
            name = f'WClientMod_DV5_{stage}'
            self.assertEqual(VARIANTS[name], [0, 1, 2])
            for flags in (0, 1, 2):
                code, meta = compile_source(select_template((directory / f'{name}.fp').read_text(encoding='latin-1'), flags))
                slots = {0, 1, 2, 4} if stage == 0 else {0}
                if flags:
                    slots.add(5 if stage == 0 else 1)
                self.assertEqual(meta['textures'], {slot: '2D' for slot in slots})
                self.assertNotIn('@', code)
                if stage == 0:
                    self.assertTrue(meta['conditions']['xenon'])
                    self.assertIn('clip((r0.xxxx));', code)
                    self.assertIn('tdepth.w = (((float4)(1.0)) - (tdepth.xxxx)).w;', code)
                    self.assertNotIn('floor(', code) # Console depth is native float, not RGB packed.
                    self.assertNotIn('PixelPosV.z = ((PixelPosV) * ((float4)(0.5))).z;', code)
                    self.assertEqual(code.count('texture1.Sample('), 9)

    def test_kill_operand_and_nested_platform_conditions(self):
        code, _ = compile_source('OUTPUT o = result.color; PARAM c = program.env[0]; KIL c; MOV o, 1; END')
        self.assertIn('clip((c));', code)
        code, _ = compile_source('''OUTPUT o = result.color;
@if xenon
@if platform_pc
MOV o, 2;
@else
MOV o, 1;
@endif
@else
MOV o, 3;
@endif
END''')
        self.assertIn('o = (((float4)(1)));', code)
        self.assertNotIn('((float4)(2))', code)
        self.assertNotIn('((float4)(3))', code)
        with self.assertRaises(ValueError):
            compile_source('OUTPUT o = result.color; KIL missing; END')

    def test_final_composite_without_motion_blur_and_optional_bloom(self):
        source = (ROOT / 'Darkness/System/Gl/ARB_fragment_program/XREngine_Final5.fp').read_text(encoding='latin-1')
        for flags in range(0, 16, 2):
            self.assertIn(flags, VARIANTS['XREngine_Final5'])
            code, metadata = compile_source(select_template(source, flags))
            self.assertNotIn(3, metadata['textures'])
            self.assertNotIn(4, metadata['textures'])
            self.assertEqual(code.count('texture0.Sample('), 1)
            self.assertEqual(1 in metadata['textures'], bool(flags & 8))
            self.assertEqual(2 in metadata['textures'], bool(flags & 4))

    def test_original_programs_and_texture_dimensions(self):
        for name, expected in ASSETS.items():
            self.assertEqual(hashlib.sha256((ROOT / 'Darkness' / name).read_bytes()).hexdigest(), expected)
        directory = ROOT / 'Darkness/System/Gl/ARB_fragment_program'
        for name, count, textures in [('XRShader_FP20_NDSP', 37, {0: '2D', 1: '2D', 2: '2D', 4: 'CUBE'}),
                                      ('XRShader_FP20_NDS', 35, {0: '2D', 1: '2D', 2: '2D'}),
                                      ('XRShader_MotionMap', 9, {}),
                                      ('XREngine_MulFilter', 10, {})]:
            source = (directory / (name + '.fp')).read_text(encoding='latin-1')
            code, metadata = compile_source(source)
            self.assertEqual((code, metadata), compile_source(source))
            self.assertEqual(metadata['instruction_count'], count)
            self.assertEqual(metadata['textures'], textures)
            self.assertNotIn('@', code)

    def test_sparse_constants_and_masked_output(self):
        code, _ = compile_source('OUTPUT o = result.color; PARAM a = program.env[19]; '
                                 'PARAM b = program.env[2]; MOV o.rgb, a; MOV o.a, b.x; END')
        self.assertIn('a = env[19]', code)
        self.assertIn('b = env[2]', code)
        # Updating alpha must retain the RGB value from the preceding instruction.
        self.assertRegex(code, r'o\.xyz = .*\.xyz;')
        self.assertRegex(code, r'o\.w = .*\.w;')
        self.assertNotIn('env[0]', code)

    def test_darkness_voice_effect_keeps_all_masks(self):
        source = (ROOT / 'Darkness/System/Gl/ARB_fragment_program/XREngine_RadialBlurInvert.fp').read_text(encoding='latin-1')
        code, metadata = compile_source(source)
        self.assertEqual(metadata['instruction_count'], 49)
        self.assertEqual(metadata['textures'], {0: '2D', 1: '2D', 2: '2D', 3: '2D'})
        self.assertEqual(code.count('texture0.Sample('), 8)
        for slot in (1, 2, 3):
            self.assertEqual(code.count(f'texture{slot}.Sample('), 1)
        self.assertIn('p7 = env[7]', code)

    def test_lrp_weights_first_operand(self):
        code, _ = compile_source('OUTPUT o = result.color; PARAM a = program.env[0]; '
                                 'PARAM b = program.env[1]; PARAM c = program.env[2]; LRP o, a, b, c; END')
        expression = re.search(r'^o = (.+);$', code, re.M)[1]
        # Evaluate the emitted arithmetic against independently chosen endpoints
        # and an interior point. A reversed interpolation used to pass compilation.
        for a, expected in [(0, .3), (1, .8), (.2, .4)]:
            self.assertAlmostEqual(eval(expression, {'__builtins__': {}}, {'a': a, 'b': .8, 'c': .3}), expected)

    def test_original_hurt_blur_samples_constants_and_cone(self):
        source = (ROOT / 'Darkness/System/Gl/ARB_fragment_program/XREngine_RadialBlurHurt.fp').read_text(encoding='latin-1')
        self.assertEqual(VARIANTS['XREngine_RadialBlurHurt'], [0])
        code, metadata = compile_source(select_template(source, 0))
        self.assertEqual(metadata['textures'], {0: '2D'})
        self.assertEqual(metadata['instruction_count'], 60)
        self.assertEqual(code.count('texture0.Sample('), 8)
        for register in range(9):
            self.assertIn(f'p{register} = env[{register}]', code)
        # Keep the original two normalizations, including their zero-radius
        # arithmetic, and square only RGB before the eight-sample average.
        self.assertEqual(code.count('1.0 / sqrt(abs('), 2)
        self.assertIn('rScale.w = ((r0.zzzz) * (r0.wwww)).w;', code)
        for sample in range(8):
            self.assertIn(f'r{sample}.xyz = ((r{sample}) * (r{sample})).xyz;', code)

        def assignment(dest, occurrence, values):
            expressions = re.findall(r'^' + re.escape(dest) + r' = \((.*)\)(?:\.[xyzw]+)?;$', code, re.M)
            expression = expressions[occurrence]
            for key, value in values.items():
                expression = expression.replace(key, repr(value))
            return eval(expression, {'__builtins__': {}}, {'min': min, 'max': max})

        # Independent clamp/cone endpoints and interior values distinguish this
        # original effect from RadialBlur and the four-texture Invert effect.
        for radius in (.125, .5, 1.0, 2.0):
            raw = assignment('rScale.x', 0, {'p8.yyyy': 1.5, 'p8.xxxx': -.25, 'rScale.wwww': radius})
            zone = assignment('rScale.x', 1, {'rScale.xxxx': raw, 'c0.yyyy': 1})
            zone = assignment('rScale.x', 2, {'rScale.xxxx': zone, 'c0.zzzz': 0})
            expected_zone = max(0, min(1, -.25 + 1.75 * radius))
            self.assertAlmostEqual(zone, expected_zone)
            for direction in (-1.0, .2, .6, 1.0):
                cone = direction
                for index, values in enumerate(({'p7.yyyy': .25}, {'p7.zzzz': 2},
                                                {'c0.zzzz': 0}, {'c0.yyyy': 1},
                                                {'rScale.xxxx': zone}, {'c0.yyyy': 1},
                                                {'p7.wwww': .7}), start=3):
                    cone = assignment('rDir.w', index, {'rDir.wwww': cone, **values})
                self.assertAlmostEqual(cone, .7 * expected_zone * max(0, min(1, (direction - .25) * 2)))
        self.assertIn('oCol = ((r0) * (p5) + (rDir.wzzz));', code)

    def test_original_mul_filter_fade_and_bindings(self):
        name = 'System/Gl/ARB_fragment_program/XREngine_MulFilter.fp'
        source = (ROOT / 'Darkness' / name).read_text(encoding='latin-1')
        self.assertEqual(VARIANTS['XREngine_MulFilter'], [0])
        code, metadata = compile_source(select_template(source, 0))
        self.assertEqual(metadata['textures'], {})
        self.assertEqual(metadata['instruction_count'], 10)
        self.assertIn('tc0 = input.tex[0]', code)
        self.assertIn('p0 = env[0]', code)
        self.assertIn('p1 = env[1]', code)
        # Preserve the original RSQ -> multiply sequence, including its zero
        # radius behavior. Do not replace it with a guessed epsilon or sqrt.
        self.assertIn('r0.z = (((float4)(1.0 / sqrt(abs((r0.wwww).x))))).z;', code)
        self.assertIn('rScale.w = ((r0.zzzz) * (r0.wwww)).w;', code)
        self.assertNotIn('saturate(', code)

        def assignment(dest, occurrence, values):
            expressions = re.findall(r'^' + re.escape(dest) + r' = \((.*)\)(?:\.[xyzw]+)?;$', code, re.M)
            expression = expressions[occurrence]
            for key, value in values.items():
                expression = expression.replace(key, repr(value))
            return eval(expression, {'__builtins__': {}}, {'min': min, 'max': max})

        # Evaluate the emitted interpolation arithmetic with asymmetric colors,
        # both clamp limits, and a partial fade. A reversed LRP produces a
        # different result even though the shader still compiles.
        for radius in (.125, .5, 1.0, 2.0):
            for low, high in ((0, 1), (-.5, 2), (.8, .2)):
                raw = assignment('rScale.x', 0, {'p0.yyyy': high, 'p0.xxxx': low, 'rScale.wwww': radius})
                upper = assignment('rScale.x', 1, {'rScale.xxxx': raw, 'c0.yyyy': 1})
                scale = assignment('rScale.x', 2, {'rScale.xxxx': upper, 'c0.zzzz': 0})
                expected_scale = max(0, min(1, low + radius * (high - low)))
                self.assertAlmostEqual(scale, expected_scale)
                for strength in (0, .25, 1):
                    for tint in (-.2, .4, 1.5, strength):
                        mixed = assignment('r0', 1, {'rScale.xxxx': scale, 'c0.yyyy': 1, 'p1': tint})
                        actual = assignment('oCol', 0, {'p1.wwww': strength, 'r0': mixed, 'c0.yyyy': 1})
                        self.assertAlmostEqual(actual, 1 + strength * ((1 - expected_scale) * (tint - 1)))

    def test_darkness_fixed_composite_keeps_both_texture_stages(self):
        source = (ROOT / 'Darkness/System/Xenon/FragmentProgram/MRenderXenon_Attrib_TexEnvMode02.fp').read_text(encoding='latin-1')
        code, metadata = compile_fixed(source)
        self.assertEqual(metadata['textures'], {0: '2D', 1: '2D'})
        self.assertEqual(metadata['instruction_count'], 12)
        for slot in (0, 1):
            self.assertEqual(code.count(f'texture{slot}.Sample('), 1)
            self.assertIn(f'input.tex[{slot}]', code)
        self.assertIn('c0 = env[0]', code)

    def test_inactive_branches_do_not_leak(self):
        code, _ = compile_source('OUTPUT o = result.color;\n@if dynmip\nBOGUS o, 0;\n'
                                 '@if support_normalize\nBOGUS o, 1;\n@else\nBOGUS o, 2;\n@endif\n'
                                 '@else\nMOV o, 1;\n@endif\nEND')
        self.assertNotIn('BOGUS', code)

    def test_duplicate_else_rejected_in_active_and_inactive_branches(self):
        duplicate = ('@if dynmip\nMOV o, 1;\n@else\nMOV o, 2;\n'
                     '@else\nMOV o, 3;\n@endif\n')
        for wrapper in ('{}', '@if platform_pc\n{}@endif\n',
                        '@if platform_pc\n@else\n{}@endif\n'):
            with self.subTest(wrapper=wrapper), self.assertRaisesRegex(ValueError, 'Duplicate else'):
                compile_source('OUTPUT o = result.color;\n' + wrapper.format(duplicate) + 'END')

    def test_nested_and_sibling_else_branches_remain_independent(self):
        code, metadata = compile_source(
            'OUTPUT o = result.color;\n'
            '@if dynmip\nBOGUS o, 0;\n@else\n'
            '@if support_normalize\nBOGUS o, 1;\n@else\nMOV o, 2;\n@endif\n'
            'ADD o, o, 3;\n@endif\n'
            '@if platform_pc\nBOGUS o, 4;\n@else\nMUL o, o, 5;\n@endif\nEND')
        self.assertNotIn('BOGUS', code)
        self.assertEqual(metadata['instruction_count'], 3)

    def test_original_anisotropic_cross_product_and_dimensions(self):
        source = (ROOT / 'Darkness/System/Gl/ARB_fragment_program/XRShader_FP20_NDSEATP.fp').read_text(encoding='latin-1')
        # All original combinations retain the cross product only with aniso.
        for flags in range(32):
            code, metadata = compile_source(select_template(source, flags))
            self.assertEqual('cross(' in code, bool(flags & 4))
            # The original source issues this fetch unconditionally; D3D's
            # compiled binding reflection removes it when its result is unused.
            self.assertEqual(metadata['textures'][8], '2D')
        # ARB XPD leaves W undefined; no native material may depend on it.
        for mask in ('', '.w', '.xyzw'):
            with self.assertRaisesRegex(ValueError, 'XPD W'):
                compile_source('OUTPUT o = result.color; TEMP t; XPD o' + mask + ', t, t; END')

    def test_unsupported_input_fails_closed(self):
        prefix = 'OUTPUT o = result.color; TEMP t; '
        for tail in ('BAD o, t;', 'ADD o, t;', 'MOV o, missing;', 'PARAM a = program.env[256];',
                     'TEX o, t, texture[16], 2D;', 'TEX o, t, texture[4], CUBE; TEX o, t, texture[4], 2D;',
                     '\n@if unknown\nMOV o, t;\n@endif', '\n@else', '\n@if dynmip'):
            with self.subTest(tail=tail), self.assertRaises(ValueError):
                compile_source(prefix + tail)

    def test_changed_asset_rejected_before_output(self):
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory) / 'game'
            output = Path(directory) / 'out'
            for name in ASSETS:
                path = game / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes((ROOT / 'Darkness' / name).read_bytes())
            changed = game / next(iter(ASSETS))
            changed.write_bytes(changed.read_bytes() + b'\n# altered\n')
            result = subprocess.run([sys.executable, str(ROOT / 'tools/compile_world_fragment.py'),
                                     '--game-dir', str(game), '--output-dir', str(output)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'Original shader asset changed', result.stderr)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
