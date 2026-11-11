#!/usr/bin/env python3
"""Wrap a verified Windows release and the Steam Deck installer in an AppImage.

Run on Linux. appimagetool and its x86_64 runtime must be supplied locally;
this script never downloads packaging tools or includes an owned game dump.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
TARGET = 'x86_64-unknown-linux-musl'
INSTALLER = 'darkrecomp-deck-installer'

# Use the Windows packager's release contract. Keep optional CRT DLLs explicit:
# a self-consistent manifest alone must not allow private game files into a bundle.
_spec = importlib.util.spec_from_file_location('darkrecomp_package_release', ROOT / 'tools/package_release.py')
_release = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_release)
# Older published ZIPs predate frame-drop capture. New ZIPs include these
# files, but their absence must not invalidate an otherwise complete old ZIP.
OPTIONAL_DIAGNOSTICS = frozenset(('LaunchFrameDrops.cmd', 'tools/analyze_frame_drops.py'))
REQUIRED_FILES = frozenset((*(name for name in _release.DOCUMENTS if name not in OPTIONAL_DIAGNOSTICS),
    *(f'build_native/Release/{name}' for name in (*_release.BINARIES, *_release.CRT_REQUIRED)),
    'tools/update_release.ps1', 'tools/update_commits.ps1', 'tools/add_steam_shortcut.py',
    *(f'tools/{name}' for name in _release.LINUX_SETUP_TOOLS),
    'ThirdParty/README.txt', 'ThirdParty/audio/COPYING.LGPLv2.1',
    'ThirdParty/audio/COPYING.winpthreads', 'ThirdParty/audio/LICENSE.md',
    'ThirdParty/audio/PROVENANCE.json', 'ThirdParty/audio/ffmpeg-darkxma-upstream.tar.gz',
    'ThirdParty/audio/ffmpeg-darkxma.patch', 'ThirdParty/audio/build_xma_codec.py'))
ALLOWED_FILES = REQUIRED_FILES | OPTIONAL_DIAGNOSTICS | frozenset(f'build_native/Release/{name}' for name in (
    'concrt140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
    'msvcp140_codecvt_ids.dll', 'vccorlib140.dll', 'vcruntime140_threads.dll'))
PLACEHOLDER = 'Darkness/PUT_GAME_FILES_HERE.txt'
ALLOWED_DIRS = {str(parent) for name in (*ALLOWED_FILES, PLACEHOLDER)
                for parent in PurePosixPath(name).parents if str(parent) != '.'}


class PackageError(RuntimeError):
    pass


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def version_name(value: object) -> str:
    if not isinstance(value, str) or not re.fullmatch(
            r'v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
            r'(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?'
            r'(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?', value):
        raise PackageError('RELEASE.json must contain a semantic version such as v0.1.1.')
    prerelease = value.split('+', 1)[0].partition('-')[2]
    if any(part.isdigit() and len(part) > 1 and part.startswith('0')
           for part in prerelease.split('.')):
        raise PackageError('Release prerelease numbers must not have leading zeroes.')
    return value


def safe_name(name: str, directory: bool = False) -> str:
    clean = name[:-1] if directory and name.endswith('/') else name
    if (not clean or any(character in clean for character in ('\\', ':', '\0'))
            or any(part in ('', '.', '..') for part in clean.split('/'))):
        raise PackageError(f'Unsafe ZIP path: {name!r}')
    return clean


def unique_json(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise PackageError(f'Duplicate RELEASE.json key: {key}')
        result[key] = value
    return result


def validate_release(bundle: zipfile.ZipFile) -> dict:
    files = {}
    seen = set()
    for entry in bundle.infolist():
        if entry.orig_filename != entry.filename:
            raise PackageError(f'Unsafe ZIP filename: {entry.orig_filename!r}')
        name = safe_name(entry.filename, entry.is_dir())
        if name.casefold() in seen:
            raise PackageError(f'Duplicate ZIP path: {name}')
        seen.add(name.casefold())
        kind = stat.S_IFMT(entry.external_attr >> 16)
        expected = stat.S_IFDIR if entry.is_dir() else stat.S_IFREG
        if kind not in (0, expected) or entry.external_attr & 0x400:
            raise PackageError(f'ZIP links and special files are forbidden: {name}')
        if entry.flag_bits & 1:
            raise PackageError(f'Encrypted ZIP entries are forbidden: {name}')
        if entry.is_dir():
            if name not in ALLOWED_DIRS:
                raise PackageError(f'Unexpected release directory: {name}')
            continue
        if name not in ALLOWED_FILES and name not in ('RELEASE.json', PLACEHOLDER):
            raise PackageError(f'Unexpected release file (game files and settings are forbidden): {name}')
        files[name] = entry
    if 'RELEASE.json' not in files:
        raise PackageError('The ZIP is missing RELEASE.json.')
    if files['RELEASE.json'].file_size > 4 * 1024 * 1024:
        raise PackageError('RELEASE.json is unexpectedly large.')
    try:
        manifest = json.loads(bundle.read(files['RELEASE.json']), object_pairs_hook=unique_json)
    except (ValueError, UnicodeError) as error:
        raise PackageError(f'Invalid RELEASE.json: {error}') from error
    if not isinstance(manifest, dict):
        raise PackageError('RELEASE.json must be an object.')
    version_name(manifest.get('version'))
    hashes = manifest.get('sha256')
    if not isinstance(hashes, dict):
        raise PackageError('RELEASE.json is missing its sha256 file manifest.')
    missing = REQUIRED_FILES - hashes.keys()
    if missing:
        raise PackageError(f'Incomplete Windows release; missing manifest files: {", ".join(sorted(missing))}')
    for name, expected in hashes.items():
        safe_name(name)
        if name not in ALLOWED_FILES:
            raise PackageError(f'Unexpected manifest file: {name}')
        if not isinstance(expected, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', expected):
            raise PackageError(f'Invalid SHA256 for {name}')
        if name not in files or files[name].file_size == 0:
            raise PackageError(f'Missing or empty manifest file: {name}')
        digest = hashlib.sha256()
        with bundle.open(files[name]) as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(chunk)
        if digest.hexdigest() != expected.lower():
            raise PackageError(f'SHA256 mismatch: {name}')
    extra = files.keys() - hashes.keys() - {'RELEASE.json', PLACEHOLDER}
    if extra:
        raise PackageError(f'Unmanifested release files: {", ".join(sorted(extra))}')
    return manifest


def extract_release(archive: Path, destination: Path) -> dict:
    # Validate every entry before creating any output. Do not use extractall:
    # release ZIPs come from Windows, but the AppDir is a Linux filesystem.
    with zipfile.ZipFile(archive) as bundle:
        manifest = validate_release(bundle)
        destination.mkdir(parents=True, exist_ok=False)
        for entry in bundle.infolist():
            target = destination.joinpath(*PurePosixPath(entry.filename).parts)
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with bundle.open(entry) as source, target.open('xb') as output:
                shutil.copyfileobj(source, output)
            target.chmod(0o755 if entry.filename == 'Launch.sh' else 0o644)
    return manifest


def validate_elf(path: Path, label: str) -> None:
    with path.open('rb') as stream:
        header = stream.read(20)
    if (len(header) != 20 or header[:7] != b'\x7fELF\x02\x01\x01'
            or int.from_bytes(header[16:18], 'little') not in (2, 3)
            or int.from_bytes(header[18:20], 'little') != 62):
        raise PackageError(f'{label} must be a Linux x86_64 ELF executable: {path}')


def build_installer(root: Path = ROOT) -> Path:
    manifest = root / 'tools/deck_installer/Cargo.toml'
    subprocess.run(['cargo', 'build', '--manifest-path', str(manifest),
                    '--release', '--locked', '--target', TARGET], cwd=root, check=True)
    return manifest.parent / 'target' / TARGET / 'release' / INSTALLER


def prepare_appdir(archive: Path, installer: Path, appdir: Path, root: Path = ROOT) -> dict:
    validate_elf(installer, 'Installer binary')
    if appdir.exists() or appdir.is_symlink():
        raise FileExistsError(f'Refusing to overwrite an AppDir: {appdir}')
    payload = appdir / 'usr/share/darkrecomp/release'
    manifest = extract_release(archive, payload)
    binary = appdir / 'usr/bin' / INSTALLER
    binary.parent.mkdir(parents=True)
    shutil.copyfile(installer, binary)
    binary.chmod(0o755)
    # The installer engine evolves independently of the Windows payload. Keep
    # the release's validated scripts intact and record the engine we actually
    # ship alongside that payload, so older releases receive installer fixes.
    engine = payload.parent / 'setup_linux.py'
    shutil.copyfile(root / 'tools/setup_linux.py', engine)
    # The bundled wizard can be newer than RELEASE.json's Windows revision.
    # Ship its complete, dependency-free Rust project and license separately;
    # copying explicit sources keeps target/ and local build files out.
    source_dir = payload.parent / 'installer-source'
    source_dir.mkdir()
    source_files = {
        'Cargo.toml': root / 'tools/deck_installer/Cargo.toml',
        'Cargo.lock': root / 'tools/deck_installer/Cargo.lock',
        'src/main.rs': root / 'tools/deck_installer/src/main.rs',
        'COPYING': root / 'COPYING',
    }
    for name, origin in source_files.items():
        destination = source_dir / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origin, destination)
    (source_dir / 'BUILD.txt').write_text(
        'The Darkness Recomp Steam Deck installer - corresponding source\n\n'
        'This project is the source of usr/bin/darkrecomp-deck-installer in this\n'
        'AppImage, independent of the bundled Windows release source revision.\n'
        'License: GPL-3.0-or-later; see COPYING. No third-party Rust crates are used.\n\n'
        'Build on Linux with Rust 1.74 or newer, Cargo, and a musl C linker.\n'
        'Copy this installer-source directory to writable storage, then run from it:\n\n'
        f'  rustup target add {TARGET}\n'
        f'  cargo build --release --locked --target {TARGET}\n\n'
        f'The result is target/{TARGET}/release/{INSTALLER}.\n'
        'Configure Cargo\'s target linker for your toolchain if needed.\n',
        encoding='utf-8', newline='\n')
    hashes = {'setup_linux.py': hash_file(engine)}
    hashes.update({f'installer-source/{name}': hash_file(source_dir / name)
                   for name in (*source_files, 'BUILD.txt')})
    metadata = {
        'format_version': 1,
        'release_version': manifest['version'],
        'release_zip_sha256': hash_file(archive),
        'installer_sha256': hash_file(binary),
        'sha256': hashes,
    }
    (payload.parent / 'INSTALLER.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    resources = root / 'tools/appimage'
    (appdir / 'AppRun').write_bytes((resources / 'AppRun').read_bytes().replace(b'\r\n', b'\n'))
    (appdir / 'AppRun').chmod(0o755)
    for name in ('darkrecomp-installer.desktop', 'darkrecomp-installer.svg'):
        shutil.copyfile(resources / name, appdir / name)
    # AppDir thumbnailers require a raster PNG for .DirIcon.
    shutil.copyfile(resources / 'darkrecomp-installer.png', appdir / '.DirIcon')
    return manifest


def _is_executable_by_current_user(path: Path, *, posix: bool | None = None) -> bool:
    """Check whether the package user can launch an existing POSIX AppImage."""
    if posix is None:
        posix = os.name == 'posix'
    return not posix or os.access(path, os.X_OK)


def existing_output_matches(staged: Path, destination: Path,
                            *, require_executable: bool = False) -> bool | None:
    """Return whether an existing ordinary file exactly matches staged.

    None means the destination disappeared or changed identity during the read.
    Symlinks and non-regular paths are rejected instead of being followed.
    For the AppImage, optionally require execute access for the package user.
    """
    try:
        before = destination.lstat()
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(before.st_mode):
        raise PackageError(f'Refusing non-regular existing output: {destination}')
    if before.st_size != staged.stat().st_size:
        return False

    try:
        with staged.open('rb') as expected, destination.open('rb') as actual:
            opened = os.fstat(actual.fileno())
            if not stat.S_ISREG(opened.st_mode) or not os.path.samestat(before, opened):
                return None
            while True:
                expected_chunk = expected.read(1024 * 1024)
                actual_chunk = actual.read(1024 * 1024)
                if expected_chunk != actual_chunk:
                    return False
                if not expected_chunk:
                    break
            after_read = os.fstat(actual.fileno())
            if (after_read.st_size != opened.st_size
                    or after_read.st_mtime_ns != opened.st_mtime_ns
                    or after_read.st_ctime_ns != opened.st_ctime_ns):
                return None
            after_path = destination.lstat()
            if not stat.S_ISREG(after_path.st_mode):
                raise PackageError(f'Refusing non-regular existing output: {destination}')
            if not os.path.samestat(after_read, after_path):
                return None
            if require_executable:
                executable = _is_executable_by_current_user(destination)
                try:
                    after_access = destination.lstat()
                except FileNotFoundError:
                    return None
                if not stat.S_ISREG(after_access.st_mode):
                    raise PackageError(f'Refusing non-regular existing output: {destination}')
                if (not os.path.samestat(after_read, after_access)
                        or after_access.st_mode != after_path.st_mode
                        or after_access.st_ctime_ns != after_path.st_ctime_ns):
                    return None
                if not executable:
                    return False
            return True
    except FileNotFoundError:
        return None


def check_existing_output(staged: Path, destination: Path,
                          *, require_executable: bool = False) -> None:
    """Allow a missing destination or an exact regular match meeting its mode contract."""
    if existing_output_matches(staged, destination,
                               require_executable=require_executable) is False:
        raise FileExistsError(f'Refusing to overwrite conflicting existing output: {destination}')


def publish_no_replace(staged: Path, destination: Path,
                       *, require_executable: bool = False) -> None:
    """Publish without replacement, or reuse an exact match meeting its mode contract."""
    for _ in range(2):
        try:
            os.link(staged, destination)
            return
        except FileExistsError:
            if existing_output_matches(staged, destination,
                                       require_executable=require_executable) is True:
                return
        except OSError as error:
            raise PackageError(
                f'Cannot publish {destination} with atomic no-overwrite hard-link '
                f'semantics; verify hard-link support and output-directory access: {error}'
            ) from error
    raise FileExistsError(f'Output changed during publication; refusing to overwrite: {destination}')


def package(archive: Path, output_dir: Path, appimagetool: Path, runtime_file: Path,
            installer: Path | None = None, root: Path = ROOT) -> Path:
    # Fail before invoking build tools if the input release is unsafe/incomplete.
    with zipfile.ZipFile(archive) as bundle:
        manifest = validate_release(bundle)
    version = version_name(manifest['version'])
    appimagetool = appimagetool.resolve(strict=True)
    runtime_file = runtime_file.resolve(strict=True)
    if not appimagetool.is_file():
        raise PackageError(f'appimagetool is not a file: {appimagetool}')
    validate_elf(runtime_file, 'AppImage runtime')
    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / f'The-Darkness-Recomp-{version}-steam-deck-x86_64.AppImage'
    checksum = output.with_suffix('.AppImage.sha256')
    installer = installer or build_installer(root)
    with tempfile.TemporaryDirectory(prefix='darkrecomp-appimage-') as temporary:
        stage = Path(temporary)
        prepare_appdir(archive, installer, stage / 'AppDir', root)
        generated = stage / output.name
        environment = os.environ | {'ARCH': 'x86_64', 'APPIMAGE_EXTRACT_AND_RUN': '1', 'VERSION': version}
        subprocess.run([str(appimagetool), '--runtime-file', str(runtime_file),
                        '--no-appstream', str(stage / 'AppDir'), str(generated)],
                       cwd=root, env=environment, check=True)
        validate_elf(generated, 'Generated AppImage')
        with tempfile.TemporaryDirectory(prefix='.darkrecomp-appimage-', dir=output_dir) as publication:
            publication = Path(publication)
            staged_output = publication / output.name
            staged_checksum = publication / checksum.name
            with generated.open('rb') as source, staged_output.open('xb') as target:
                shutil.copyfileobj(source, target)
            staged_output.chmod(0o755)
            with staged_checksum.open('x', encoding='utf-8', newline='\n') as stream:
                stream.write(f'{hash_file(staged_output)}  {output.name}\n')

            # Refuse known conflicts before publishing either name. A crash
            # between these two links can leave a complete image without its
            # checksum; a later run may reuse only byte-identical regular files
            # and requires the AppImage to remain executable on POSIX.
            check_existing_output(staged_output, output, require_executable=True)
            check_existing_output(staged_checksum, checksum)
            # Each link is atomic and no-replace on its own; the pair is not a
            # cross-file transaction. Publish the image before its checksum.
            publish_no_replace(staged_output, output, require_executable=True)
            publish_no_replace(staged_checksum, checksum)
    return output


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release-zip', required=True, type=Path,
                        help='complete Windows release ZIP, including Linux setup tools')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build_native/releases')
    parser.add_argument('--installer-binary', type=Path,
                        help='prebuilt Linux x86_64 installer; otherwise build with Cargo and musl')
    parser.add_argument('--appimagetool', type=Path, help='locally verified appimagetool executable')
    parser.add_argument('--runtime-file', type=Path, help='locally verified x86_64 type-2 AppImage runtime')
    parser.add_argument('--appdir-only', type=Path, metavar='DIRECTORY',
                        help='assemble a reviewable AppDir without running appimagetool')
    args = parser.parse_args(argv)
    if not args.appdir_only and (not args.appimagetool or not args.runtime_file):
        parser.error('--appimagetool and --runtime-file are required to create an AppImage.')
    try:
        if args.appdir_only:
            # Validate before spending time compiling the GUI.
            with zipfile.ZipFile(args.release_zip) as bundle:
                validate_release(bundle)
            prepare_appdir(args.release_zip, args.installer_binary or build_installer(), args.appdir_only)
            print(args.appdir_only)
        else:
            print(package(args.release_zip, args.output_dir, args.appimagetool,
                          args.runtime_file, args.installer_binary))
    except (OSError, PackageError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        parser.exit(1, f'AppImage packaging failed: {error}\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
