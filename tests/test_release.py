"""Exercise release contents and Windows/Linux launcher setup checks."""
import hashlib
import importlib.util
import json
import io
import os
from pathlib import Path
import shutil
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
steam_spec = importlib.util.spec_from_file_location('add_steam_shortcut', ROOT / 'tools/add_steam_shortcut.py')
steam = importlib.util.module_from_spec(steam_spec)
steam_spec.loader.exec_module(steam)


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
        (self.root / 'tools/update_release.ps1').write_bytes((ROOT / 'tools/update_release.ps1').read_bytes())
        (self.root / 'tools/add_steam_shortcut.py').write_bytes((ROOT / 'tools/add_steam_shortcut.py').read_bytes())
        for name in release.LINUX_SETUP_TOOLS:
            (self.root / 'tools' / name).write_bytes((ROOT / 'tools' / name).read_bytes())

    def game_files(self):
        game = self.root / 'Darkness'
        game.mkdir(exist_ok=True)
        for name in ('default.xex',):
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
            self.assertIn('build_native/Release/DarkRecompSettings.exe', names)
            self.assertEqual(bundle.read('LaunchWithSettings.cmd'), (ROOT / 'LaunchWithSettings.cmd').read_bytes())
            self.assertEqual(bundle.read('LaunchWithUpdates.cmd'), (ROOT / 'LaunchWithUpdates.cmd').read_bytes())
            self.assertEqual(bundle.read('tools/update_release.ps1'), (ROOT / 'tools/update_release.ps1').read_bytes())
            self.assertEqual(bundle.read('LaunchStallProfiler.cmd'), (ROOT / 'LaunchStallProfiler.cmd').read_bytes())
            self.assertIn('Launch.sh', names)
            self.assertIn('SetupLinux.cmd', names)
            self.assertIn('PlayLinux.cmd', names)
            self.assertIn('STEAM_DECK.md', names)
            for name in release.LINUX_SETUP_TOOLS:
                self.assertEqual(bundle.read(f'tools/{name}'), (ROOT / 'tools' / name).read_bytes())
            shell = bundle.getinfo('Launch.sh')
            self.assertEqual(shell.create_system, 3)
            self.assertEqual(shell.external_attr >> 16, 0o100755)
            self.assertNotIn(b'\r\n', bundle.read('Launch.sh'))
            self.assertEqual(bundle.read('tools/add_steam_shortcut.py'), (ROOT / 'tools/add_steam_shortcut.py').read_bytes())
            self.assertIn('START_HERE.txt', names)
            manifest = json.loads(bundle.read('RELEASE.json'))
            self.assertEqual(manifest['commit'], 'abc123')
            for name, expected in manifest['sha256'].items():
                self.assertEqual(hashlib.sha256(bundle.read(name)).hexdigest(), expected)
        self.assertIn(hashlib.sha256(path.read_bytes()).hexdigest(), path.with_suffix('.zip.sha256').read_text())
        if os.name == 'nt':
            # Exercise the real PowerShell reader against the Python packager,
            # including generated notices and Launch.sh's Unix file mode.
            stage = self.root / 'update-stage'
            stage.mkdir()
            def ps_quote(value):
                return "'" + str(value).replace("'", "''") + "'"
            command = (
                "$ErrorActionPreference = 'Stop'; "
                f". {ps_quote(ROOT / 'tools/update_release.ps1')} -LibraryOnly; "
                f"Expand-VerifiedRelease {ps_quote(path)} "
                f"{ps_quote(path.with_suffix('.zip.sha256'))} "
                f"{ps_quote(stage)} 'v0.1.1' | Out-Null"
            )
            result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy',
                                     'Bypass', '-Command', command], capture_output=True,
                                    text=True, timeout=30,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((stage / 'LaunchWithUpdates.cmd').read_bytes(),
                             (ROOT / 'LaunchWithUpdates.cmd').read_bytes())
            self.assertFalse((stage / 'Darkness').exists())

    def test_incomplete_package_is_rejected(self):
        (self.bin / 'avcodec-darkxma-62.dll').unlink()
        with self.assertRaisesRegex(RuntimeError, 'avcodec-darkxma-62.dll'):
            release.package(self.root, self.crt, self.root / 'out', 'v0.1.1', 'abc123')
        self.assertFalse((self.root / 'out').exists())

    def test_missing_crt_is_rejected(self):
        (self.crt / 'msvcp140.dll').unlink()
        with self.assertRaisesRegex(RuntimeError, 'msvcp140.dll'):
            release.collect_files(self.root, self.crt)

    def test_incomplete_linux_setup_package_is_rejected(self):
        paths = [self.root / name for name in ('Launch.sh', 'SetupLinux.cmd', 'PlayLinux.cmd', 'STEAM_DECK.md')]
        paths += [self.root / 'tools' / name for name in release.LINUX_SETUP_TOOLS]
        for path in paths:
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.unlink()
                try:
                    with self.assertRaisesRegex(RuntimeError, path.name.replace('.', r'\.')):
                        release.package(self.root, self.crt, self.root / 'out', 'v0.1.1', 'abc123')
                    self.assertFalse((self.root / 'out').exists())
                finally:
                    path.write_bytes(original)

    def test_incomplete_settings_launcher_package_is_rejected(self):
        for path in (self.bin / 'DarkRecompSettings.exe', self.root / 'LaunchWithSettings.cmd'):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.unlink()
                try:
                    with self.assertRaisesRegex(RuntimeError, path.name.replace('.', r'\.')):
                        release.package(self.root, self.crt, self.root / 'out', 'v0.1.1', 'abc123')
                    self.assertFalse((self.root / 'out').exists())
                finally:
                    path.write_bytes(original)

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
        for name in ('default.xex', 'Content', 'System', 'START_HERE.txt'):
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
    def test_missing_settings_launcher_points_to_complete_release(self):
        (self.bin / 'DarkRecompSettings.exe').unlink()
        result = subprocess.run('cmd /d /c LaunchWithSettings.cmd', cwd=self.root,
                                input='\n', capture_output=True, text=True, timeout=15,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('Extract the ENTIRE Windows release ZIP', result.stdout)
        self.assertIn('LaunchWithSettings.cmd', result.stdout)
        self.assertFalse((self.root / 'build_native/run').exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher')
    def test_stall_launcher_keeps_normal_play_arguments_and_scopes_environment(self):
        # Capture the wrapper's delegation without running the game. The outer
        # script also checks that opting in does not persist in its caller.
        (self.root / 'Launch.cmd').write_text(
            '@echo off\n>launcher-env.txt echo %DARKRECOMP_STALL_PROFILE%\n'
            '>launcher-args.txt echo %*\nexit /b 37\n', encoding='utf-8')
        (self.root / 'test-launch.cmd').write_text(
            '@echo off\nset "DARKRECOMP_STALL_PROFILE=0"\n'
            'call LaunchStallProfiler.cmd --game-dir "dump & files!" --fps 120\n'
            'set "LAUNCH_RESULT=%ERRORLEVEL%"\n'
            '>outer-env.txt echo %DARKRECOMP_STALL_PROFILE%\n'
            'exit /b %LAUNCH_RESULT%\n', encoding='utf-8')
        result = subprocess.run('cmd /d /c test-launch.cmd', cwd=self.root,
                                capture_output=True, text=True, timeout=15,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assertEqual((self.root / 'launcher-env.txt').read_text().strip(), '1')
        self.assertEqual((self.root / 'outer-env.txt').read_text().strip(), '0')
        self.assertEqual((self.root / 'launcher-args.txt').read_text().strip(),
                         'play --game-dir "dump & files!" --fps 120')

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
        (external / 'default.xex').unlink()
        result = self.launch('check --game-dir "external dump & files!"')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('external dump & files!\\default.xex', result.stdout)


@unittest.skipUnless(os.name == 'posix' and shutil.which('bash'), 'Linux/Bash launcher')
class LinuxLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dark Linux release & test! ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.launcher = self.root / 'Launch.sh'
        self.launcher.write_bytes((ROOT / 'Launch.sh').read_bytes())
        self.bin = self.root / 'build_native/Release'
        self.bin.mkdir(parents=True)
        for name in release.BINARIES + release.CRT_REQUIRED:
            (self.bin / name).write_bytes(b'fixture')
        self.game = self.root / 'Darkness'
        (self.game / 'Content').mkdir(parents=True)
        (self.game / 'System').mkdir()
        (self.game / 'default.xex').write_bytes(b'private fixture')
        self.command_dir = self.root / 'fake commands'
        self.command_dir.mkdir()
        # Restrict PATH to fixtures so even a machine with real UMU installed
        # cannot launch it. The launcher only needs dirname and cat besides Bash.
        for command in ('dirname', 'cat'):
            (self.command_dir / command).symlink_to(shutil.which(command))
        self.runner = self.command_dir / 'umu-run'
        self.runner.write_text(
            '#!/bin/bash\n'
            'printf "%s\\0" "$PWD" "$WINEPREFIX" "$GAMEID" "$PROTONPATH" "$@" > "$UMU_TEST_CAPTURE"\n'
            'exit "${UMU_TEST_EXIT:-0}"\n', encoding='utf-8')
        self.runner.chmod(0o755)
        self.capture = self.root / 'captured argv'
        self.bash = shutil.which('bash')
        self.env = os.environ.copy()
        for name in ('WINEPREFIX', 'GAMEID', 'PROTONPATH', 'XDG_DATA_HOME'):
            self.env.pop(name, None)
        self.env.update(PATH=str(self.command_dir), HOME=str(self.root / 'fake home'),
                        UMU_TEST_CAPTURE=str(self.capture))

    def launch(self, *arguments, **environment):
        return subprocess.run([self.bash, str(self.launcher), *arguments],
                              cwd=self.command_dir, env=self.env | environment,
                              capture_output=True, text=True, timeout=15)

    def captured(self):
        return self.capture.read_bytes().decode('utf-8').split('\0')[:-1]

    def test_default_launch_uses_release_root_and_preserves_arguments(self):
        arguments = ['--fps', '60', '--label', 'spaces & "quotes" $()', '', 'line\nbreak']
        result = self.launch(*arguments)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.captured(), [str(self.root),
            str(self.root / 'fake home/.local/share/darkrecomp/proton'),
            'umu-default', 'UMU-Proton', str(self.bin / 'DarkRecompPreview.exe'),
            '--sound', *arguments])

    def test_mute_respects_prefix_tool_and_game_overrides(self):
        override = str(self.root / 'other prefix with spaces')
        result = self.launch('mute', '--fps', '30', WINEPREFIX=override,
                             GAMEID='umu-custom', PROTONPATH='/tools/custom Proton')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.captured()[1:], [override, 'umu-custom', '/tools/custom Proton',
            str(self.bin / 'DarkRecompPreview.exe'), '--mute', '--fps', '30'])

    def test_xdg_default_and_runner_exit_status(self):
        data = str(self.root / 'Linux user data')
        result = self.launch('play', XDG_DATA_HOME=data, UMU_TEST_EXIT='37')
        self.assertEqual(result.returncode, 37)
        self.assertEqual(self.captured()[1], str(Path(data) / 'darkrecomp/proton'))

    def test_check_does_not_run_umu_or_create_a_prefix(self):
        before = sorted(str(path.relative_to(self.root)) for path in self.root.rglob('*'))
        result = self.launch('check')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Setup looks ready', result.stdout)
        self.assertFalse(self.capture.exists())

        self.assertEqual(before, sorted(str(path.relative_to(self.root)) for path in self.root.rglob('*')))

    def test_missing_game_and_dll_prevent_launch(self):
        (self.game / 'default.xex').unlink()
        (self.bin / 'avcodec-darkxma-62.dll').unlink()
        result = self.launch()
        self.assertEqual(result.returncode, 1)
        self.assertIn('default.xex', result.stderr)
        self.assertIn('avcodec-darkxma-62.dll', result.stderr)
        self.assertIn('entire Windows release ZIP', result.stderr)
        self.assertFalse(self.capture.exists())

    def test_release_dll_names_accept_windows_casing(self):
        for name, destination in (
                ('msvcp140.dll', 'MSVCP140.dll'),
                ('vcruntime140.dll', 'VCRUNTIME140.DLL'),
                ('vcruntime140_1.dll', 'VCRUNTIME140_1.dll'),
                ('avcodec-darkxma-62.dll', 'AvCodec-DarkXma-62.DlL')):
            (self.bin / name).rename(self.bin / destination)
        result = self.launch('check')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Setup looks ready', result.stdout)
        self.assertFalse(self.capture.exists())

        launcher = self.bin / 'DARKRECOMPPREVIEW.EXE'
        (self.bin / 'DarkRecompPreview.exe').rename(launcher)
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.captured()[4], str(launcher))

    def test_missing_umu_guides_supported_install_and_steam_alternative(self):
        self.runner.unlink()
        result = self.launch()
        self.assertEqual(result.returncode, 1)
        self.assertIn('umu-run was not found', result.stderr)
        self.assertIn('Open-Wine-Components/umu-launcher', result.stderr)
        self.assertIn('Steam', result.stderr)

    def test_help_does_not_require_setup_or_umu(self):
        self.runner.unlink()
        (self.game / 'default.xex').unlink()
        result = self.launch('--help')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Usage:', result.stdout)
        self.assertFalse(self.capture.exists())


class SteamShortcutTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dark steam & test! ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        launcher = self.root / 'build_native/Release/DarkRecompPreview.exe'
        launcher.parent.mkdir(parents=True)
        launcher.write_bytes(b'launcher')
        self.entry = steam.shortcut_entry(self.root)
        self.userdata = self.root / 'Steam/userdata'
        self.shortcuts = self.userdata / '123/config/shortcuts.vdf'
        self.shortcuts.parent.mkdir(parents=True)

    def read_shortcuts(self):
        data = self.shortcuts.read_bytes()
        parsed, end = steam.parse_dict(data)
        self.assertEqual(end, len(data))
        return parsed

    def test_shortcut_quotes_installed_folder_and_starts_sound(self):
        self.assertEqual(self.entry['Exe'], f'"{self.root / "build_native/Release/DarkRecompPreview.exe"}"')
        self.assertEqual(self.entry['StartDir'], f'"{self.root}"')
        self.assertEqual(self.entry['LaunchOptions'], '--sound')

    def test_linux_discovers_native_and_flatpak_steam_locations(self):
        candidates = steam.steam_userdata_roots(platform='linux', home=self.root)
        self.assertIn(self.root / '.local/share/Steam/userdata', candidates)
        self.assertIn(self.root / '.steam/root/userdata', candidates)
        self.assertIn(self.root / '.var/app/com.valvesoftware.Steam/.local/share/Steam/userdata', candidates)

    def test_discovery_accepts_first_shortcut_and_deduplicates_users(self):
        (self.userdata / 'anonymous/config').mkdir(parents=True)
        self.assertEqual(steam.find_shortcuts([self.userdata, self.userdata]), [self.shortcuts])
        self.assertTrue(steam.add_to(self.shortcuts, self.entry))
        self.assertEqual(self.read_shortcuts()['shortcuts']['0'], self.entry)

    def test_sparse_entries_and_other_root_fields_survive(self):
        # Independent binary VDF fixture: existing indices 0 and 2.
        original = (b'\x00shortcuts\x00\x000\x00\x01AppName\x00Other game\x00\x08'
                    b'\x002\x00\x01AppName\x00Second game\x00\x08\x08'
                    b'\x01extra\x00keep me\x00\x08')
        self.shortcuts.write_bytes(original)
        self.assertTrue(steam.add_to(self.shortcuts, self.entry))
        parsed = self.read_shortcuts()
        self.assertEqual(parsed['shortcuts']['0']['AppName'], 'Other game')
        self.assertEqual(parsed['shortcuts']['2']['AppName'], 'Second game')
        self.assertEqual(parsed['shortcuts']['3'], self.entry)
        self.assertEqual(parsed['extra'], 'keep me')
        backup = self.shortcuts.with_name('shortcuts.vdf.bak')
        self.assertEqual(backup.read_bytes(), original)
        current = self.shortcuts.read_bytes()
        self.assertFalse(steam.add_to(self.shortcuts, self.entry))
        self.assertEqual(self.shortcuts.read_bytes(), current)
        self.assertEqual(backup.read_bytes(), original)

    def test_existing_backup_is_preserved(self):
        self.shortcuts.write_bytes(b'\x00shortcuts\x00\x08\x08')
        backup = self.shortcuts.with_name('shortcuts.vdf.bak')
        backup.write_bytes(b'previous backup')
        self.assertTrue(steam.add_to(self.shortcuts, self.entry))
        self.assertEqual(backup.read_bytes(), b'previous backup')

    def test_dry_run_does_not_create_or_change_steam_files(self):
        self.assertFalse(steam.add_to(self.shortcuts, self.entry, dry_run=True))
        self.assertFalse(self.shortcuts.exists())
        original = b'\x00shortcuts\x00\x08\x08'
        self.shortcuts.write_bytes(original)
        self.assertFalse(steam.add_to(self.shortcuts, self.entry, dry_run=True))
        self.assertEqual(self.shortcuts.read_bytes(), original)
        self.assertFalse(self.shortcuts.with_name('shortcuts.vdf.bak').exists())

    def test_unknown_or_truncated_vdf_is_preserved(self):
        for original in (b'\x00shortcuts\x00\x03unknown\x00abcd\x08\x08',
                         b'\x00shortcuts\x00\x000\x00\x02appid\x00\x01',
                         b'\x00shortcuts\x00\x08\x08trailing'):
            with self.subTest(original=original):
                self.shortcuts.write_bytes(original)
                with self.assertRaises(ValueError):
                    steam.add_to(self.shortcuts, self.entry)
                self.assertEqual(self.shortcuts.read_bytes(), original)
                self.assertFalse(self.shortcuts.with_name('shortcuts.vdf.bak').exists())

    def test_multiple_users_require_explicit_selection(self):
        second = self.userdata / '456/config/shortcuts.vdf'
        second.parent.mkdir(parents=True)
        with self.assertRaises(SystemExit) as raised:
            steam.main(['--root', str(self.root), '--steam-userdata', str(self.userdata)])
        self.assertEqual(raised.exception.code, 2)
        self.assertFalse(self.shortcuts.exists())
        self.assertFalse(second.exists())
        self.assertEqual(steam.main([str(self.shortcuts), '--root', str(self.root)]), 0)
        self.assertTrue(self.shortcuts.exists())
        self.assertFalse(second.exists())


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
