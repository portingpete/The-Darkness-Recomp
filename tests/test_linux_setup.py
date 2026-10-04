"""Exercise Linux setup with local archives and fake launchers only."""
from contextlib import redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import package_release
import setup_linux


def write_archive(path, root, entries):
    """Write (relative name, kind, payload) entries beneath a fixture root."""
    with tarfile.open(path, "w:gz") as archive:
        directory = tarfile.TarInfo(root)
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        archive.addfile(directory)
        for name, kind, payload in entries:
            member = tarfile.TarInfo(f"{root}/{name}" if root else name)
            member.mode = 0o755 if kind in ("dir", "executable") else 0o644
            if kind == "dir":
                member.type = tarfile.DIRTYPE
            elif kind in ("symlink", "hardlink"):
                member.type = tarfile.SYMTYPE if kind == "symlink" else tarfile.LNKTYPE
                member.linkname = payload
            elif kind == "fifo":
                member.type = tarfile.FIFOTYPE
            elif kind == "device":
                member.type = tarfile.CHRTYPE
                member.devmajor = 1
                member.devminor = 3
            else:
                member.type = tarfile.REGTYPE
                payload = payload.encode() if isinstance(payload, str) else payload
                member.size = len(payload)
                archive.addfile(member, io.BytesIO(payload))
                continue
            archive.addfile(member)
    return hashlib.sha256(path.read_bytes()).hexdigest()


FAKE_UMU = """#!/usr/bin/env python3
import json, os, pathlib, sys
capture = os.environ.get("SETUP_TEST_CAPTURE")
if capture:
    pathlib.Path(capture).write_text(json.dumps({
        "argv": sys.argv[1:], "cwd": os.getcwd(),
        "environment": {key: value for key, value in os.environ.items()
                        if key in ("WINEPREFIX", "GAMEID", "PROTONPATH", "WINEDLLOVERRIDES",
                                   "LD_LIBRARY_PATH", "LIBGL_DRIVERS_PATH", "MESA_D3D12_DEFAULT_ADAPTER_NAME",
                                   "GALLIUM_DRIVER", "SETUP_TEST_GRAPHICS", "PRESSURE_VESSEL_BWRAP",
                                   "DARKRECOMP_REAL_BWRAP", "DARKRECOMP_RUNTIME_PARENT",
                                   "DARKRECOMP_SETUP_LOG_DIR", "PRESSURE_VESSEL_VARIABLE_DIR",
                                   "PRESSURE_VESSEL_GC_RUNTIMES", "PROTON_USE_WINED3D",
                                   "PROTON_LOG", "WINEDEBUG", "UMU_RUNTIME_UPDATE")}
    }))
sys.exit(int(os.environ.get("SETUP_TEST_EXIT", "0")))
"""


class SetupFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="dark Linux setup & test! ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source checkout"
        self.source.mkdir()
        (self.source / "Launch.sh").write_bytes((ROOT / "Launch.sh").read_bytes())
        self.binary = self.source / "build_native/Release"
        self.binary.mkdir(parents=True)
        for name in package_release.BINARIES + package_release.CRT_REQUIRED:
            (self.binary / name).write_bytes(("fixture " + name).encode())
        self.game = self.root / "private game dump"
        (self.game / "Content").mkdir(parents=True)
        (self.game / "System").mkdir()
        (self.game / "default.xex").write_bytes(b"private game fixture")
        (self.game / "Content/data.bin").write_bytes(b"private content fixture")
        (self.game / "System/data.bin").write_bytes(b"private system fixture")
        self.install = self.root / "Linux installation"
        self.crt = self.root / "mixed case runtime"
        self.crt.mkdir()
        for name in package_release.CRT_REQUIRED:
            (self.crt / name.upper()).write_bytes(("CRT " + name).encode())
        self.umu_archive = self.root / "umu-fixture.tar.gz"
        self.proton_archive = self.root / "proton-fixture.tar.gz"
        umu_hash = write_archive(self.umu_archive, "umu", [
            ("umu-run", "executable", FAKE_UMU),
            ("umu_run.py", "symlink", "umu-run"),
        ])
        proton_hash = write_archive(self.proton_archive, "GE-Proton-fixture", [
            ("proton", "executable", "#!/bin/sh\nexit 99\n"),
            ("version", "file", "fixture\n"),
            ("files", "dir", None),
            ("files/lib", "dir", None),
            ("files/lib/library.so.1", "file", b"library fixture"),
            ("files/lib/library.so", "symlink", "library.so.1"),
        ])
        self.umu = setup_linux.ToolSpec("UMU", "fixture", "https://example.invalid/umu",
                                        umu_hash, "umu", ("umu-run",))
        self.proton = setup_linux.ToolSpec("GE-Proton", "fixture", "https://example.invalid/proton",
                                           proton_hash, "GE-Proton-fixture", ("proton",))
        for name, value in (("UMU", self.umu), ("PROTON", self.proton)):
            mock = patch.object(setup_linux, name, value)
            mock.start()
            self.addCleanup(mock.stop)
        no_network = patch("urllib.request.urlopen", side_effect=AssertionError("Unexpected network access"))
        no_network.start()
        self.addCleanup(no_network.stop)
        tools = self.source / "tools"
        tools.mkdir()
        (tools / "wsl_graphics.py").write_text(
            "#!/usr/bin/env python3\nimport os, sys\n"
            "os.environ['SETUP_TEST_GRAPHICS'] = 'called'\n"
            "os.execvp(sys.argv[1], sys.argv[1:])\n", encoding="utf-8")

    def setup(self, **options):
        defaults = dict(source=self.source, game_dir=self.game, install=self.install,
                        umu_archive=self.umu_archive, proton_archive=self.proton_archive,
                        skip_download=True, wsl=False)
        defaults.update(options)
        with redirect_stdout(io.StringIO()):
            return setup_linux.setup(**defaults)

    def snapshot(self, directory):
        """Capture contents and times without following any directory links."""
        snapshot = {}
        if not directory.exists():
            return snapshot
        for parent, directories, files in os.walk(directory, followlinks=False):
            for name in directories + files:
                path = Path(parent) / name
                stat = path.lstat()
                relative = str(path.relative_to(directory))
                if path.is_symlink():
                    contents = ("link", os.readlink(path))
                elif path.is_file():
                    contents = ("file", path.read_bytes())
                else:
                    contents = ("directory",)
                snapshot[relative] = (contents, stat.st_mtime_ns, stat.st_mode & 0o7777)
        return snapshot

    def seed_existing_tools(self):
        self.install.mkdir(mode=0o700)
        for spec, archive in ((self.umu, self.umu_archive), (self.proton, self.proton_archive)):
            staging = self.root / ("existing-" + spec.root)
            extracted = setup_linux.extract_archive(archive, staging, spec.root)
            extracted.rename(self.install / spec.root)
            staging.rmdir()


class ArchiveSafetyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="dark archive test ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.archive = self.root / "fixture.tar.gz"
        self.staging = self.root / "staging"
        self.attempt = 0

    def extract(self, entries):
        write_archive(self.archive, "tool", entries)
        self.attempt += 1
        self.staging = self.root / f"staging-{self.attempt}"
        return setup_linux.extract_archive(self.archive, self.staging, "tool")

    @unittest.skipUnless(os.name == "posix", "Linux symlink extraction")
    def test_relative_symlinks_and_executable_mode_are_preserved(self):
        result = self.extract([
            ("bin", "dir", None), ("lib", "dir", None),
            ("lib/library.so.1", "file", b"library"),
            ("lib/library.so", "symlink", "library.so.1"),
            ("bin/library.so", "symlink", "../lib/library.so"),
            ("bin/run", "executable", "#!/bin/sh\nexit 0\n"),
        ])
        self.assertEqual(result, self.staging / "tool")
        self.assertTrue((result / "bin/library.so").is_symlink())
        self.assertEqual((result / "bin/library.so").read_bytes(), b"library")
        self.assertEqual((result / "bin/run").stat().st_mode & 0o111, 0o111)

    def test_traversal_and_absolute_member_names_are_rejected(self):
        for name in ("../escaped", "dir/../../escaped"):
            with self.subTest(name=name), self.assertRaises((RuntimeError, ValueError)):
                self.extract([(name, "file", b"unsafe")])
        with tarfile.open(self.archive, "w:gz") as archive:
            member = tarfile.TarInfo(str(self.root / "escaped"))
            member.size = 6
            archive.addfile(member, io.BytesIO(b"unsafe"))
        with self.assertRaises((RuntimeError, ValueError)):
            setup_linux.extract_archive(self.archive, self.staging, "tool")
        self.assertFalse((self.root / "escaped").exists())

    def test_absolute_and_escaping_link_targets_are_rejected(self):
        for target in ("/tmp/outside", "../outside", "dir/../../outside"):
            with self.subTest(target=target), self.assertRaises((RuntimeError, ValueError)):
                self.extract([("link", "symlink", target)])

    def test_hardlinks_and_special_files_are_rejected(self):
        for kind in ("hardlink", "fifo", "device"):
            with self.subTest(kind=kind), self.assertRaises((RuntimeError, ValueError)):
                self.extract([("run", "file", b"safe"), ("unsafe", kind, "tool/run")])

    def test_symlink_ancestors_are_rejected_regardless_of_archive_order(self):
        entries = [("directory", "dir", None), ("alias", "symlink", "directory"),
                   ("alias/payload", "file", b"must not follow the link")]
        for order in (entries, list(reversed(entries))):
            with self.subTest(order=order), self.assertRaises((RuntimeError, ValueError)):
                self.extract(order)

    def test_regular_file_ancestors_are_rejected(self):
        with self.assertRaises((RuntimeError, ValueError)):
            self.extract([("file", "file", b"regular"), ("file/payload", "file", b"invalid")])

    def test_symlink_expansion_before_parent_components_cannot_escape(self):
        with self.assertRaises((RuntimeError, ValueError)):
            self.extract([("alias", "symlink", "."),
                          ("escape", "symlink", "alias/../outside")])

    def test_symlink_cycles_are_rejected(self):
        with self.assertRaises((RuntimeError, ValueError)):
            self.extract([("first", "symlink", "second"), ("second", "symlink", "first")])

    def test_duplicate_member_paths_are_rejected(self):
        with self.assertRaises((RuntimeError, ValueError)):
            self.extract([("run", "file", b"first"), ("./run", "file", b"second")])

    @unittest.skipUnless(os.name == "posix", "Linux symlink extraction")
    def test_existing_staging_symlink_cannot_redirect_extraction(self):
        outside = self.root / "outside"
        outside.mkdir()
        marker = outside / "preserved"
        marker.write_bytes(b"outside data")
        self.staging.symlink_to(outside, target_is_directory=True)
        write_archive(self.archive, "tool", [("run", "file", b"must stay inside staging")])
        with self.assertRaises((RuntimeError, ValueError, FileExistsError)):
            setup_linux.extract_archive(self.archive, self.staging, "tool")
        self.assertEqual(marker.read_bytes(), b"outside data")
        self.assertFalse((outside / "tool").exists())

    @unittest.skipUnless(os.name == "posix", "Linux symlink extraction")
    def test_python310_filter_fallback_preserves_links_and_rejects_escapes(self):
        with patch.dict(tarfile.__dict__):
            tarfile.__dict__.pop("data_filter", None)
            result = self.extract([("run", "executable", b"runner"), ("alias", "symlink", "run")])
            self.assertEqual((result / "alias").read_bytes(), b"runner")
            with self.assertRaises((RuntimeError, ValueError)):
                self.extract([("alias", "symlink", "."), ("escape", "symlink", "alias/../outside")])


@unittest.skipUnless(os.name == "posix", "Linux installation")
class LinuxSetupTests(SetupFixture):
    def block_game_content(self, after=0):
        scandir = os.scandir
        blocked = self.game / "Content"
        visits = 0

        def deny_content(path):
            nonlocal visits
            if Path(path) == blocked:
                visits += 1
                if visits > after:
                    raise PermissionError(13, "Permission denied", os.fspath(path))
            return scandir(path)

        return patch.object(setup_linux.os, "scandir", side_effect=deny_content)

    def test_local_archives_install_without_modifying_source_or_game(self):
        before_source = self.snapshot(self.source)
        before_game = self.snapshot(self.game)
        result = self.setup()
        self.assertIsInstance(result, dict)
        self.assertEqual(self.snapshot(self.source), before_source)
        self.assertEqual(self.snapshot(self.game), before_game)
        runners = list(self.install.rglob("umu-run"))
        self.assertEqual(len(runners), 1)
        self.assertEqual(runners[0].read_text(), FAKE_UMU)
        self.assertEqual(runners[0].stat().st_mode & 0o111, 0o111)
        self.assertTrue(any(path.is_file() for path in self.install.rglob("DarkRecompPreview.exe")))
        binaries = next(path.parent for path in self.install.rglob("DarkRecompPreview.exe"))
        menu_sources = "CubeWnd.pc.xcr.source.sha256"
        self.assertEqual((binaries / menu_sources).read_bytes(), (self.binary / menu_sources).read_bytes())

    def test_missing_release_file_fails_before_installation(self):
        (self.binary / "avcodec-darkxma-62.dll").unlink()
        with self.assertRaisesRegex((RuntimeError, ValueError), "avcodec-darkxma-62.dll"):
            self.setup()
        self.assertFalse(self.install.exists())

    def test_missing_game_root_fails_before_installation(self):
        for name in ("default.xex", "Content", "System"):
            with self.subTest(name=name):
                unavailable = self.game / name
                renamed = self.game / (name + ".hidden")
                unavailable.rename(renamed)
                try:
                    with self.assertRaises((RuntimeError, ValueError)):
                        self.setup()
                    self.assertFalse(self.install.exists())
                finally:
                    renamed.rename(unavailable)

    def test_unreadable_game_subdirectory_fails_before_installation(self):
        with self.block_game_content():
            with self.assertRaisesRegex(setup_linux.SetupError, "Content"):
                self.setup()
        self.assertFalse(self.install.exists())

    def test_unreadable_game_subdirectory_fails_read_only_check(self):
        self.setup()
        before = self.snapshot(self.install)
        with self.block_game_content():
            with self.assertRaisesRegex(setup_linux.SetupError, "Content"):
                self.setup(check=True, umu_archive=None, proton_archive=None)
        self.assertEqual(self.snapshot(self.install), before)

    def test_unreadable_game_subdirectory_during_directory_walk_fails(self):
        with self.block_game_content(after=1):
            with self.assertRaisesRegex(setup_linux.SetupError, "Content"):
                self.setup()
        self.assertFalse(self.install.exists())

    def test_missing_crt_is_rejected(self):
        (self.binary / "msvcp140.dll").unlink()
        with self.assertRaisesRegex((RuntimeError, ValueError), "msvcp140.dll"):
            self.setup()
        self.assertFalse(self.install.exists())

    def test_mixed_case_crt_files_are_accepted(self):
        for name in package_release.CRT_REQUIRED:
            (self.binary / name).unlink()
        self.setup(crt_dir=self.crt)
        binaries = next(path.parent for path in self.install.rglob("DarkRecompPreview.exe"))
        names = {path.name.casefold(): path for path in binaries.iterdir()}
        for name in package_release.CRT_REQUIRED:
            self.assertEqual(names[name].read_bytes(), ("CRT " + name).encode())

    def test_offline_missing_archives_cannot_install(self):
        with self.assertRaises((RuntimeError, ValueError, FileNotFoundError)):
            self.setup(umu_archive=None, proton_archive=None)
        self.assertFalse(any(self.install.rglob("umu-run")))

    def test_archive_hash_mismatch_cannot_publish_a_tool(self):
        self.umu_archive.write_bytes(self.umu_archive.read_bytes() + b"corrupt download")
        with self.assertRaisesRegex((RuntimeError, ValueError), "(?i)(hash|sha256|checksum)"):
            self.setup()
        self.assertFalse(any(self.install.rglob("umu-run")))

    def test_archive_missing_required_tool_file_is_rejected(self):
        incomplete = setup_linux.ToolSpec(self.umu.name, self.umu.version, self.umu.url,
                                          self.umu.sha256, self.umu.root,
                                          ("umu-run", "missing.py"))
        with patch.object(setup_linux, "UMU", incomplete):
            with self.assertRaisesRegex((RuntimeError, ValueError), "missing.py"):
                self.setup()
        self.assertFalse((self.install / self.umu.root).exists())

    def test_rerun_is_idempotent_and_preserves_prefix_saves_and_settings(self):
        first = self.setup()
        personal = {
            "ge-prefix/user.reg": b"existing Wine registry",
            "game/saves/private-save": b"existing save",
            "game/DarkRecomp.settings.ini": b"existing settings",
            "game/Darkness/Content/extra-asset": b"existing extra content",
        }
        for relative, contents in personal.items():
            path = self.install / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        before = self.snapshot(self.install)
        second = self.setup(umu_archive=None, proton_archive=None)
        self.assertEqual(second, first)
        self.assertEqual(self.snapshot(self.install), before)

    def test_check_is_read_only_without_archives(self):
        installed = self.setup()
        before = self.snapshot(self.root)
        checked = self.setup(check=True, umu_archive=None, proton_archive=None)
        self.assertEqual(checked, installed)
        self.assertEqual(self.snapshot(self.root), before)

    def test_check_of_missing_install_does_not_create_files(self):
        before = self.snapshot(self.root)
        with self.assertRaises((RuntimeError, ValueError)):
            self.setup(check=True, umu_archive=None, proton_archive=None)
        self.assertEqual(self.snapshot(self.root), before)

    def test_managed_build_can_update_while_local_edits_are_preserved(self):
        self.setup()
        original = self.binary / "DarkRecompPreview.exe"
        destination = self.install / "game/build_native/Release/DarkRecompPreview.exe"
        original.write_bytes(b"new build")
        self.setup(umu_archive=None, proton_archive=None)
        self.assertEqual(destination.read_bytes(), b"new build")
        destination.write_bytes(b"local edit")
        original.write_bytes(b"another build")
        before = self.snapshot(self.install)
        with self.assertRaisesRegex((RuntimeError, ValueError), "(?i)(modified|preserv)"):
            self.setup(umu_archive=None, proton_archive=None)
        self.assertEqual(self.snapshot(self.install), before)

    def test_existing_original_game_bytes_are_never_replaced(self):
        self.setup()
        original = self.game / "Content/data.bin"
        original.write_bytes(b"different private content")
        before = self.snapshot(self.install)
        with self.assertRaisesRegex((RuntimeError, ValueError), "(?i)(game|asset)"):
            self.setup(umu_archive=None, proton_archive=None)
        self.assertEqual(self.snapshot(self.install), before)

    def test_empty_original_game_directories_are_preserved(self):
        (self.game / "System/data.bin").unlink()
        (self.game / "Content/empty content").mkdir()
        self.setup()
        self.assertTrue((self.install / "game/Darkness/System").is_dir())
        self.assertTrue((self.install / "game/Darkness/Content/empty content").is_dir())

    def test_check_rejects_a_missing_installed_game_root_without_repairing_it(self):
        (self.game / "System/data.bin").unlink()
        self.setup()
        system = self.install / "game/Darkness/System"
        system.rename(self.install / "game/Darkness/hidden System")
        before = self.snapshot(self.install)
        with self.assertRaises((RuntimeError, ValueError)):
            self.setup(check=True, umu_archive=None, proton_archive=None)
        self.assertEqual(self.snapshot(self.install), before)

    def test_matching_existing_tool_trees_are_adopted_without_changing_them(self):
        self.seed_existing_tools()
        cache = self.install / self.proton.root / "__pycache__/existing.pyc"
        cache.parent.mkdir()
        cache.write_bytes(b"existing Python cache")
        before = {spec.name: self.snapshot(self.install / spec.root) for spec in (self.umu, self.proton)}
        result = self.setup()
        for spec in (self.umu, self.proton):
            self.assertEqual(result["tools"][spec.name]["archive_sha256"], spec.sha256)
            self.assertEqual(self.snapshot(self.install / spec.root), before[spec.name])
        self.assertEqual(cache.read_bytes(), b"existing Python cache")

    def test_modified_existing_tool_tree_is_preserved_and_refused(self):
        self.seed_existing_tools()
        runner = self.install / self.umu.root / "umu-run"
        contents = runner.read_bytes()
        runner.write_bytes(b"!" + contents[1:])
        before = self.snapshot(self.install)
        with self.assertRaisesRegex((RuntimeError, ValueError), "(?i)(differ|modified|preserv)"):
            self.setup()
        self.assertEqual(self.snapshot(self.install), before)

    def test_install_root_and_ancestor_aliases_are_rejected_before_resolving(self):
        outside = self.root / "alias target"
        outside.mkdir(mode=0o700)
        (outside / "existing data").write_bytes(b"preserve alias target")
        alias = self.root / "install alias"
        alias.symlink_to(outside, target_is_directory=True)
        for candidate in (alias, alias / "new install"):
            with self.subTest(candidate=candidate):
                before = self.snapshot(self.root)
                with self.assertRaisesRegex((RuntimeError, ValueError), "(?i)(link|alias)"):
                    self.setup(install=candidate)
                self.assertEqual(self.snapshot(self.root), before)

    def test_existing_install_root_is_private_before_copy_and_check(self):
        self.install.mkdir(mode=0o755)
        self.install.chmod(0o755)
        self.setup()
        self.assertEqual(self.install.stat().st_mode & 0o777, 0o700)
        self.assertTrue((self.install / "game/Darkness/Content/data.bin").is_file())

        self.install.chmod(0o755)
        before = self.snapshot(self.install)
        with self.assertRaisesRegex(setup_linux.SetupError, "private"):
            self.setup(check=True, umu_archive=None, proton_archive=None)
        self.assertEqual(self.install.stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.snapshot(self.install), before)

        self.setup(umu_archive=None, proton_archive=None)
        self.assertEqual(self.install.stat().st_mode & 0o777, 0o700)

    def test_private_runtime_permissions_are_checked_without_repair_then_repaired_on_setup(self):
        self.setup(wsl=True)
        sentinels = {}
        for name in ("wsl-pv", "evidence"):
            directory = self.install / name
            sentinel = directory / "existing data"
            sentinel.write_bytes(("preserve " + name).encode())
            sentinels[sentinel] = sentinel.read_bytes()
            directory.chmod(0o755)
        before = self.snapshot(self.install)
        with self.assertRaisesRegex((RuntimeError, ValueError), "(?i)(private|permission)"):
            self.setup(wsl=True, check=True, umu_archive=None, proton_archive=None)
        self.assertEqual(self.snapshot(self.install), before)
        self.setup(wsl=True, umu_archive=None, proton_archive=None)
        for name in ("wsl-pv", "evidence"):
            self.assertEqual((self.install / name).stat().st_mode & 0o777, 0o700)
        for sentinel, contents in sentinels.items():
            self.assertEqual(sentinel.read_bytes(), contents)

    def test_read_only_check_does_not_repair_launcher_or_hook_permissions(self):
        self.setup(wsl=True)
        for relative in ("play-linux.sh", "game/Launch.sh", "wsl_graphics.py"):
            with self.subTest(relative=relative):
                path = self.install / relative
                path.chmod(0o644)
                before = self.snapshot(self.install)
                with self.assertRaisesRegex((RuntimeError, ValueError), "(?i)(executable|permission)"):
                    self.setup(wsl=True, check=True, umu_archive=None, proton_archive=None)
                self.assertEqual(self.snapshot(self.install), before)
                self.assertEqual(path.stat().st_mode & 0o777, 0o644)
                self.setup(wsl=True, umu_archive=None, proton_archive=None)
                self.assertEqual(path.stat().st_mode & 0o777, 0o755)

    def test_writable_runtime_or_evidence_directory_is_refused_without_changing_it(self):
        self.setup(wsl=True)
        for name in ("wsl-pv", "evidence"):
            with self.subTest(name=name):
                directory = self.install / name
                directory.chmod(0o777)
                before = self.snapshot(self.install)
                with self.assertRaisesRegex((RuntimeError, ValueError), "(?i)(writable|belong)"):
                    self.setup(wsl=True, umu_archive=None, proton_archive=None)
                self.assertEqual(self.snapshot(self.install), before)
                directory.chmod(0o700)


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "Linux/Bash launcher")
class InstalledLauncherTests(SetupFixture):
    def launch(self, *arguments, **environment):
        capture = self.root / "captured command.json"
        env = os.environ.copy()
        for name in ("WINEPREFIX", "GAMEID", "PROTONPATH", "PRESSURE_VESSEL_BWRAP",
                     "DARKRECOMP_REAL_BWRAP", "DARKRECOMP_RUNTIME_PARENT", "DARKRECOMP_SETUP_LOG_DIR",
                     "PRESSURE_VESSEL_VARIABLE_DIR", "PRESSURE_VESSEL_GC_RUNTIMES",
                     "GALLIUM_DRIVER", "LD_LIBRARY_PATH", "PROTON_USE_WINED3D", "XDG_DATA_HOME"):
            env.pop(name, None)
        env.update(HOME=str(self.root / "fake home"), SETUP_TEST_CAPTURE=str(capture))
        env.update(environment)
        result = subprocess.run([shutil.which("bash"), str(self.install / "play-linux.sh"), *arguments],
                                cwd=self.root, env=env, capture_output=True, text=True, timeout=15)
        recorded = json.loads(capture.read_text()) if capture.exists() else None
        return result, recorded

    def test_native_wrapper_preserves_arguments_and_uses_pinned_tools(self):
        self.setup(wsl=False)
        arguments = ["--label", 'spaces & "quotes" $()', "", "line\nbreak", "--fps", "61"]
        result, captured = self.launch(*arguments)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(captured["cwd"], str(self.install / "game"))
        self.assertEqual(captured["argv"], [
            str(self.install / "game/build_native/Release/DarkRecompPreview.exe"), "--sound", *arguments])
        self.assertEqual(captured["environment"]["WINEPREFIX"], str(self.install / "ge-prefix"))
        self.assertEqual(captured["environment"]["PROTONPATH"], str(self.install / self.proton.root))
        self.assertEqual(captured["environment"]["GAMEID"], "umu-default")
        self.assertNotIn("PRESSURE_VESSEL_BWRAP", captured["environment"])

    def test_mute_respects_user_overrides_and_propagates_runner_exit_status(self):
        self.setup(wsl=False)
        custom_prefix = str(self.root / "custom prefix")
        custom_tool = str(self.root / "custom Proton")
        result, captured = self.launch("mute", "--fps", "30", WINEPREFIX=custom_prefix,
                                      GAMEID="umu-custom", PROTONPATH=custom_tool, SETUP_TEST_EXIT="37")
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assertEqual(captured["argv"][1:], ["--mute", "--fps", "30"])
        self.assertEqual(captured["environment"]["WINEPREFIX"], custom_prefix)
        self.assertEqual(captured["environment"]["PROTONPATH"], custom_tool)
        self.assertEqual(captured["environment"]["GAMEID"], "umu-custom")

    def test_wsl_wrapper_sets_hook_and_defaults_before_user_overrides(self):
        self.setup(wsl=True)
        result, captured = self.launch("play", "--width", "800", "--height", "600", "--fps", "45")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(captured["argv"][1:], ["--sound", "--width", "640", "--height", "360", "--fps", "30",
                                               "--width", "800", "--height", "600", "--fps", "45"])
        env = captured["environment"]
        self.assertEqual(env["PRESSURE_VESSEL_BWRAP"], str(self.install / "wsl_graphics.py"))
        self.assertEqual(env["DARKRECOMP_RUNTIME_PARENT"], str(self.install / "wsl-pv"))
        self.assertEqual(env["PRESSURE_VESSEL_VARIABLE_DIR"], str(self.install / "wsl-pv"))
        self.assertEqual(env["PRESSURE_VESSEL_GC_RUNTIMES"], "0")
        self.assertEqual(env["GALLIUM_DRIVER"], "d3d12")
        self.assertEqual(env["PROTON_USE_WINED3D"], "1")
        self.assertTrue(env["LD_LIBRARY_PATH"].startswith("/usr/lib/wsl/lib:"))
        self.assertTrue(env["DARKRECOMP_REAL_BWRAP"].endswith("/umu/steamrt4/pressure-vessel/libexec/steam-runtime-tools-0/srt-bwrap"))

    def test_wrapper_help_and_check_do_not_run_umu_or_change_install(self):
        for wsl in (False, True):
            with self.subTest(wsl=wsl):
                self.setup(wsl=wsl)
                for argument in ("check", "help"):
                    with self.subTest(argument=argument):
                        before = self.snapshot(self.install)
                        result, captured = self.launch(argument)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIsNone(captured)
                        self.assertEqual(self.snapshot(self.install), before)


if __name__ == "__main__":
    unittest.main()
