"""Exercise AOT generation and verification without running native tools."""
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import recompile


class OriginalSwitchMetadataTests(unittest.TestCase):
    """Pin the gameplay dispatch to original PPC bytes, without generating AOT."""

    @classmethod
    def setUpClass(cls):
        cls.image = (ROOT / 'Darkness/basefile.exe').read_bytes()
        config_path = ROOT / 'config/darkness.toml'
        config = tomllib.loads(config_path.read_text())['main']
        tables_path = config_path.parent / config['switch_table_file_path']
        cls.tables = tomllib.loads(tables_path.read_text())['switch']

    def word(self, address):
        return struct.unpack_from('>I', self.image, address - 0x82000000)[0]

    def original_dispatch(self):
        # lis/addi materialize the inline table, then rlwinm r0,r24,2,0,29
        # and lwzx/mtctr/bctr select an absolute target. The nearby r11 range
        # check concerns r29-1 and must not be used as the switch index.
        words = tuple(self.word(a) for a in range(0x821C495C, 0x821C4974, 4))
        self.assertEqual(words, (0x3D80821C, 0x398C4974, 0x5700103A,
                                 0x7C0C002E, 0x7C0903A6, 0x4E800420))
        register = (words[2] >> 21) & 31
        table_address = ((words[0] & 0xFFFF) << 16) + (words[1] & 0xFFFF)
        labels = [self.word(table_address + 4 * i) for i in range(5)]
        self.assertEqual(labels, [0x821C4988, 0x821C4A94, 0x821C4988,
                                  0x821C49B0, 0x821C4988])
        self.assertEqual(table_address + 4 * len(labels), min(labels))
        return register, table_address, labels

    def supplied_dispatch(self):
        # XenonRecomp accepts metadata anywhere in the six-instruction
        # dispatch. Ensure no duplicate entry can shadow the corrected one.
        entries = [t for t in self.tables if 0x821C495C <= t['base'] <= 0x821C4970]
        self.assertEqual(len(entries), 1)
        return entries[0]

    def test_gameplay_switch_uses_original_index_register(self):
        register, _, _ = self.original_dispatch()
        self.assertEqual(self.supplied_dispatch()['r'], register)

    def test_gameplay_switch_preserves_all_original_targets(self):
        _, _, labels = self.original_dispatch()
        # Includes repeated labels, the join at index 1, and the previously
        # unreachable body at index 3. No invented default target is needed.
        self.assertEqual(self.supplied_dispatch()['labels'], labels)

    def test_gameplay_switch_routes_all_indices_with_logged_other_registers(self):
        register, table_address, labels = self.original_dispatch()
        supplied = self.supplied_dispatch()
        for index in range(len(labels)):
            with self.subTest(index=index):
                # r24 was not logged, so cover every original table index;
                # r3=2, r11=4, r28=r29=5 are from overlay-before-01.log.
                registers = {3: 2, 11: 4, 28: 5, 29: 5, register: index}
                offset = (registers[register] << 2) & 0xFFFFFFFC
                original_target = self.word(table_address + offset)
                recovered_index = registers[supplied['r']] & 0xFFFFFFFF
                self.assertLess(recovered_index, len(supplied['labels']))
                self.assertEqual(supplied['labels'][recovered_index], original_target)


class RecompileTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='darkrecomp-recompile-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.out = self.root / 'build/generated'
        self.xenon = self.root / 'refs/UnleashedRecomp/tools/XenonRecomp'
        self.generator = self.root / 'generator.exe'
        for name in (
            'config/darkness.toml', 'runtime/guest/ppc_context.template.h',
            'Darkness/_uncrypted.xex', 'Darkness/darkness_switch_tables.toml',
            'Darkness/basefile.exe', 'tools/native_imports.py', 'tools/recompile.py',
            'runtime/native/kernel.cpp', 'generator.exe',
        ):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture input\n')
        for name in (
            'XenonRecomp/recompiler.cpp', 'XenonRecomp/recompiler.h',
            'XenonAnalyse/analyse.cpp', 'XenonAnalyse/analyse.h',
            'XenonUtils/xbox/xam_table.inc', 'XenonUtils/xbox/xboxkrnl_table.inc',
        ):
            path = self.xenon / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture input\n')
        (self.root / 'config/darkness.toml').write_text(
            '[main]\nfile_path = "../Darkness/_uncrypted.xex"\n'
            'switch_table_file_path = "../Darkness/darkness_switch_tables.toml"\n'
        )
        self.chunk_count = 1
        self.diagnostics = 0
        self.report_chunk_count = True
        self.omit_chunk = None
        for patcher in (
            patch.object(recompile, 'ROOT', self.root),
            patch.object(recompile, 'XENON', self.xenon),
            patch.object(recompile, '__file__', str(self.root / 'tools/recompile.py')),
            patch.object(recompile.subprocess, 'run', side_effect=self.generate),
            patch.object(recompile, 'generate_imports', side_effect=self.generate_imports),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    def generate(self, command, **kwargs):
        config_path = Path(command[1])
        config = tomllib.loads(config_path.read_text())['main']
        out = config_path.parent / config['out_directory_path']
        for name in ('ppc_config.h', 'ppc_context.h', 'ppc_recomp_shared.h'):
            (out / name).write_text('#pragma once\n')
        (out / 'ppc_func_mapping.cpp').write_text('// current function mapping\n')
        for index in range(self.chunk_count):
            if index != self.omit_chunk:
                (out / f'ppc_recomp.{index}.cpp').write_text(f'void guest_{index}() {{}}\n')
        if self.report_chunk_count:
            kwargs['stdout'].write(f'Translation units: {self.chunk_count}\n')
        kwargs['stdout'].write(f'Semantic diagnostics: {self.diagnostics}\n')
        return subprocess.CompletedProcess(command, int(bool(self.diagnostics)))

    def generate_imports(self, root, out):
        (out / 'ppc_imports.cpp').write_text('// current import stubs\n')
        (out / 'ppc_image_metadata.h').write_text('#pragma once\n')

    def regenerate(self, *flags):
        arguments = [
            'recompile.py', '--output', str(self.out),
            '--generator', str(self.generator), *flags,
        ]
        with patch.object(sys, 'argv', arguments), redirect_stdout(io.StringIO()):
            return recompile.main()

    def verify(self, allow_incomplete=False):
        with redirect_stdout(io.StringIO()):
            return recompile.verify(self.out, allow_incomplete)

    def test_shrinking_generation_excludes_obsolete_chunks(self):
        self.chunk_count = 2
        self.assertEqual(self.regenerate(), 0)
        obsolete = self.out / 'ppc_recomp.1.cpp'
        old_contents = obsolete.read_bytes()
        self.chunk_count = 1
        self.assertEqual(self.regenerate(), 0)
        manifest = json.loads((self.out / 'manifest.json').read_text())
        self.assertNotIn(obsolete.name, manifest['outputs'])
        source_list = (self.out / 'sources.cmake').read_text()
        self.assertNotIn(obsolete.name, source_list)
        for name in ('ppc_recomp.0.cpp', 'ppc_func_mapping.cpp', 'ppc_imports.cpp'):
            self.assertIn(name, manifest['outputs'])
            self.assertIn(name, source_list)
        self.assertEqual(obsolete.read_bytes(), old_contents)
        self.assertEqual(self.verify(), 0)

    def test_obsolete_chunk_errors_do_not_block_current_generation(self):
        self.chunk_count = 2
        self.regenerate()
        (self.out / 'ppc_recomp.1.cpp').write_text('// ERROR: obsolete translation\n')
        self.chunk_count = 1
        self.assertEqual(self.regenerate(), 0)

    def test_stale_source_list_is_rejected_after_shrinking_generation(self):
        self.chunk_count = 2
        self.regenerate()
        source_list = self.out / 'sources.cmake'
        stale_list = source_list.read_bytes()
        self.chunk_count = 1
        self.regenerate()
        self.assertEqual(self.verify(), 0)
        # A restored build list can select a leftover chunk that is no longer
        # covered by the current manifest's C++ output hashes.
        self.assertTrue((self.out / 'ppc_recomp.1.cpp').is_file())
        source_list.write_bytes(stale_list)
        for allow_incomplete in (False, True):
            with self.subTest(allow_incomplete=allow_incomplete):
                with self.assertRaisesRegex(RuntimeError, r'Changed outputs: .*sources\.cmake'):
                    self.verify(allow_incomplete=allow_incomplete)

    def test_missing_source_list_is_rejected(self):
        self.regenerate()
        (self.out / 'sources.cmake').unlink()
        with self.assertRaisesRegex(RuntimeError, r'Changed outputs: .*sources\.cmake'):
            self.verify()

    def test_missing_translation_unit_count_is_rejected(self):
        self.report_chunk_count = False
        with self.assertRaisesRegex(RuntimeError, '[Tt]ranslation unit'):
            self.regenerate()
        self.assertFalse((self.out / 'manifest.json').exists())

    def test_missing_reported_chunk_is_rejected(self):
        self.omit_chunk = 0
        with self.assertRaisesRegex(OSError, r'ppc_recomp\.0\.cpp'):
            self.regenerate()
        self.assertFalse((self.out / 'manifest.json').exists())

    def test_export_table_changes_invalidate_manifest(self):
        for library in ('xam', 'xboxkrnl'):
            with self.subTest(library=library):
                self.regenerate()
                self.assertEqual(self.verify(), 0)
                table = self.xenon / f'XenonUtils/xbox/{library}_table.inc'
                table.write_text('changed export mapping\n')
                with self.assertRaisesRegex(RuntimeError, rf'Changed inputs: .*{library}_table\.inc'):
                    self.verify()

    def test_export_table_deletion_invalidates_manifest(self):
        self.regenerate()
        (self.xenon / 'XenonUtils/xbox/xam_table.inc').unlink()
        with self.assertRaisesRegex(RuntimeError, r'Changed inputs: .*xam_table\.inc'):
            self.verify()

    def test_identical_outputs_keep_timestamps_and_unrelated_files(self):
        self.regenerate()
        chunk = self.out / 'ppc_recomp.0.cpp'
        old_time = 1_000_000_000_000_000_000
        os.utime(chunk, ns=(old_time, old_time))
        saved_time = chunk.stat().st_mtime_ns
        unrelated = self.out / 'notes.txt'
        unrelated.write_text('keep this file\n')
        self.assertEqual(self.regenerate(), 0)
        self.assertEqual(chunk.stat().st_mtime_ns, saved_time)
        self.assertEqual(unrelated.read_text(), 'keep this file\n')

    def test_semantic_diagnostics_still_require_explicit_opt_in(self):
        self.diagnostics = 2
        with self.assertRaisesRegex(RuntimeError, '2 unresolved semantic diagnostics'):
            self.regenerate()
        self.assertEqual(self.regenerate('--allow-incomplete'), 0)
        with self.assertRaisesRegex(RuntimeError, '2 unresolved semantic diagnostics'):
            self.verify()
        self.assertEqual(self.verify(allow_incomplete=True), 0)


if __name__ == '__main__':
    unittest.main()
