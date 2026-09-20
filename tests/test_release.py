"""Exercise release contents and the actual Windows launcher setup checks."""
import hashlib
import importlib.util
import json
import io
import os
from pathlib import Path
import subprocess
import tempfile
import tarfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_release', ROOT / 'tools/package_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
codec_spec = importlib.util.spec_from_file_location('build_xma_codec', ROOT / 'tools/build_xma_codec.py')
codec = importlib.util.module_from_spec(codec_spec)
codec_spec.loader.exec_module(codec)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dark release & test! ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.crt = self.root / 'crt'
        self.crt.mkdir()
        for name in release.CRT_REQUIRED:
            (self.crt / name).write_bytes(b'runtime')
        self.bin = self.root / 'build_native/Release'
        self.bin.mkdir(parents=True)
        for name in release.BINARIES:
            (self.bin / name).write_bytes(b'build output')
        for name in release.DOCUMENTS:
            (self.root / name).write_bytes((ROOT / name).read_bytes())
        deps = self.root / 'build_native/deps'
        audio = deps / 'ffmpeg-darkxma'
        audio.mkdir(parents=True)
        for name in ('COPYING.LGPLv2.1', 'COPYING.winpthreads', 'LICENSE.md', 'PROVENANCE.json'):
            (audio / name).write_bytes(b'notice')
        for name in ('ffmpeg-darkxma-upstream.tar.gz', 'ffmpeg-darkxma.patch'):
            (deps / name).write_bytes(b'codec source')
        (self.root / 'tools').mkdir()
        (self.root / 'tools/build_xma_codec.py').write_bytes(b'build script')

    def game_files(self):
        game = self.root / 'Darkness'
        game.mkdir(exist_ok=True)
        for name in ('basefile.exe', '_uncrypted.xex', 'default.xex'):
            (game / name).write_bytes(b'private game data')
        for name in ('Content', 'System'):
            (game / name).mkdir(exist_ok=True)

    def launch(self, argument='check'):
        # cmd.exe consumes a shell command, not CRT-escaped argv quoting.
        return subprocess.run(f'cmd /d /c Launch.cmd {argument}', cwd=self.root,
                              input='\n', capture_output=True, text=True, timeout=15,
                              creationflags=subprocess.CREATE_NO_WINDOW)

    def test_package_excludes_game_saves_logs_and_unlisted_binaries(self):
        self.game_files()
        (self.root / 'saves').mkdir()
        (self.root / 'saves/private-save').write_bytes(b'private')
        (self.root / 'DarkRecomp.settings.ini').write_bytes(b'private')
        (self.bin / 'private.log').write_bytes(b'private')
        (self.bin / 'Unrelated.dll').write_bytes(b'excluded')
        path = release.package(self.root, self.crt, self.root / 'out', 'v0.1.1', 'abc123')
        with zipfile.ZipFile(path) as bundle:
            names = bundle.namelist()
            self.assertEqual([n for n in names if n.startswith('Darkness/')], ['Darkness/PUT_GAME_FILES_HERE.txt'])
            self.assertFalse(any('private' in n or n.endswith('.ini') or n.startswith('saves/') for n in names))
            self.assertNotIn('build_native/Release/Unrelated.dll', names)
            self.assertIn('build_native/Release/vcruntime140_1.dll', names)
            self.assertIn('START_HERE.txt', names)
            manifest = json.loads(bundle.read('RELEASE.json'))
            self.assertEqual(manifest['commit'], 'abc123')
            for name, expected in manifest['sha256'].items():
                self.assertEqual(hashlib.sha256(bundle.read(name)).hexdigest(), expected)
        self.assertIn(hashlib.sha256(path.read_bytes()).hexdigest(), path.with_suffix('.zip.sha256').read_text())

    def test_incomplete_package_is_rejected(self):
        (self.bin / 'avcodec-darkxma-62.dll').unlink()
        with self.assertRaisesRegex(RuntimeError, 'avcodec-darkxma-62.dll'):
            release.package(self.root, self.crt, self.root / 'out', 'v0.1.1', 'abc123')
        self.assertFalse((self.root / 'out').exists())

    def test_missing_crt_is_rejected(self):
        (self.crt / 'msvcp140.dll').unlink()
        with self.assertRaisesRegex(RuntimeError, 'msvcp140.dll'):
            release.collect_files(self.root, self.crt)

    def test_existing_release_is_preserved(self):
        path = release.package(self.root, self.crt, self.root / 'out', 'v0.1.1', 'abc123')
        original = path.read_bytes()
        with self.assertRaises(FileExistsError):
            release.package(self.root, self.crt, self.root / 'out', 'v0.1.1', 'abc123')
        self.assertEqual(path.read_bytes(), original)

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher')
    def test_launcher_lists_missing_files(self):
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        for name in ('basefile.exe', '_uncrypted.xex', 'default.xex', 'Content', 'System', 'START_HERE.txt'):
            self.assertIn(name, result.stdout)

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher')
    def test_launcher_rejects_nested_game_folder(self):
        (self.root / 'Darkness/Darkness/Content').mkdir(parents=True)
        result = self.launch()
        self.assertEqual(result.returncode, 1)
        self.assertIn('without another folder', result.stdout)

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher')
    def test_launcher_accepts_complete_folder_with_spaces_and_symbols(self):
        self.game_files()
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Setup looks ready', result.stdout)

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher')
    def test_incomplete_extraction_points_to_download(self):
        (self.bin / 'DarkRecomp.exe').unlink()
        result = self.launch()
        self.assertEqual(result.returncode, 1)
        self.assertIn('Extract the ENTIRE Windows release ZIP', result.stdout)
        self.assertNotIn('Build it first', result.stdout)

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher')
    def test_help_does_not_require_game_files(self):
        result = self.launch('help')
        self.assertEqual(result.returncode, 0)
        self.assertIn('Usage:', result.stdout)

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher')
    def test_launcher_checks_external_game_directory(self):
        self.game_files()
        external = self.root / 'external dump & files!'
        (self.root / 'Darkness').rename(external)
        result = self.launch('check --game-dir "external dump & files!"')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Setup looks ready', result.stdout)
        (external / '_uncrypted.xex').unlink()
        result = self.launch('check --game-dir "external dump & files!"')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('external dump & files!\\_uncrypted.xex', result.stdout)


class CodecSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.source = root / 'FFmpeg-fixture'
        self.source.mkdir()
        self.archive = root / 'source.tar.gz'
        with tarfile.open(self.archive, 'w:gz') as tar:
            for name, contents in {'decoder.c': b'original', 'version.c': b'version'}.items():
                entry = tarfile.TarInfo(f'{self.source.name}/{name}')
                entry.size = len(contents)
                tar.addfile(entry, io.BytesIO(contents))
                (self.source / name).write_bytes(contents)
        (self.source / 'decoder.c').write_bytes(b'patched')

    def verify(self):
        codec.verify_source_tree(self.source, self.archive, {'decoder.c': b'patched'})

    def test_exact_source_with_shipped_patch_is_accepted(self):
        self.verify()

    def test_edit_outside_patch_is_rejected(self):
        (self.source / 'version.c').write_bytes(b'local change')
        with self.assertRaisesRegex(RuntimeError, 'version.c'):
            self.verify()

    def test_unshipped_source_file_is_rejected(self):
        (self.source / 'config.h').write_bytes(b'local configuration')
        with self.assertRaisesRegex(RuntimeError, 'config.h'):
            self.verify()


if __name__ == '__main__':
    unittest.main()
