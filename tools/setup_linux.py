#!/usr/bin/env python3
"""Copy a complete Windows release and owned game dump into a private Linux install.

Pinned UMU and GE-Proton archives come from their upstream GitHub releases.
Setup never runs the game, installs system packages, or changes a Wine prefix.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import uuid


@dataclass(frozen=True)
class ToolSpec:
    name: str
    version: str
    url: str
    sha256: str
    root: str
    required: tuple[str, ...]


# GitHub's published release-asset SHA256 digests, verified October 1, 2026.
UMU = ToolSpec('umu', '1.4.4',
    'https://github.com/Open-Wine-Components/umu-launcher/releases/download/1.4.4/umu-launcher-1.4.4-zipapp.tar',
    'eb590691841f7fad3fc3ad8fd5db4ccb87849fe7948e62b28ece7a4ee48cc851',
    'umu', ('umu-run',))
PROTON = ToolSpec('proton', 'GE-Proton11-7',
    'https://github.com/GloriousEggroll/proton-ge-custom/releases/download/GE-Proton11-7/GE-Proton11-7-x86_64.tar.gz',
    'c5448b76a230384e2d7bc6beb5ccb97bafb7e2c3b6c527cb03a1a546bbcb00a0',
    'GE-Proton11-7-x86_64', ('proton', 'files/bin/wine', 'files/bin/wineserver'))
REQUIRED = ('DarkRecomp.exe', 'DarkRecompPreview.exe', 'DarkRecompSettings.exe',
    'avcodec-darkxma-62.dll', 'avutil-darkxma-60.dll', 'libwinpthread-1.dll',
    'CubeWnd.pc.xcr', 'CubeWnd.pc.xcr.source.sha256', 'GameContext_Create.pc.xdf')
CRT = ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
MANIFEST = 'LINUX_SETUP.json'
VERSION = 1


class SetupError(RuntimeError):
    pass


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def absolute(path: Path, label: str) -> Path:
    if not path.is_absolute():
        raise SetupError(f'{label} must be an absolute Linux path: {path}')
    return path.resolve()


def under(path: Path, parent: Path) -> bool:
    return path == parent or parent in path.parents


def no_links(path: Path) -> None:
    for part in (*reversed(path.parents), path):
        if part.is_symlink():
            raise SetupError(f'Refusing a destination symlink: {part}')


def validate_install(install: Path, source: Path, game: Path) -> None:
    if re.match(r'^/mnt/[a-z](?:/|$)', str(install), re.IGNORECASE):
        raise SetupError('Install on the Linux filesystem, not a Windows /mnt/<drive> folder.')
    if install == Path('/') or install == Path.home():
        raise SetupError('Choose a private installation subdirectory.')
    for origin in (source, game):
        if under(install, origin) or under(origin, install):
            raise SetupError(f'Source and installation must not overlap: {origin}, {install}')
    no_links(install)
    parent = install
    while not parent.exists():
        parent = parent.parent
    if not parent.is_dir():
        raise SetupError(f'Installation parent is not a directory: {parent}')
    info = parent.stat()
    if hasattr(os, 'geteuid') and (info.st_uid != os.geteuid() or info.st_mode & 0o022):
        raise SetupError(f'Installation parent must belong to you and not be writable by others: {parent}')
    # Reject Windows-backed mount types even if mounted at an unusual location.
    mountinfo = Path('/proc/self/mountinfo')
    if mountinfo.exists():
        matches = []
        for line in mountinfo.read_text().splitlines():
            before, separator, after = line.partition(' - ')
            if not separator:
                continue
            fields = before.split()
            mount = Path(fields[4].replace('\\040', ' ').replace('\\134', '\\'))
            if under(parent, mount):
                matches.append((len(str(mount)), after.split()[0]))
        if matches and max(matches)[1] in ('9p', 'drvfs', 'ntfs', 'ntfs3', 'fuseblk'):
            raise SetupError('The installation must use the Linux filesystem, not a Windows-backed mount.')


def names_in(directory: Path) -> dict[str, Path]:
    if not directory.is_dir():
        raise SetupError(f'Missing directory: {directory}')
    result = {}
    for path in directory.iterdir():
        folded = path.name.casefold()
        if folded in result:
            raise SetupError(f'Ambiguous Windows filename casing in {directory}: {path.name}')
        result[folded] = path
    return result


def required_file(directory: Path, name: str) -> Path:
    path = names_in(directory).get(name.casefold())
    if path is None or path.is_symlink() or not path.is_file() or not path.stat().st_size:
        raise SetupError(f'Missing, empty, or linked required file: {directory / name}')
    return path


def fail_game_walk(error: OSError) -> None:
    raise SetupError(f'Cannot read original game directory: {error}') from error


def is_wsl2() -> bool:
    release = Path('/proc/sys/kernel/osrelease')
    text = release.read_text().lower() if release.exists() else ''
    return 'microsoft' in text and ('wsl2' in text or Path('/dev/dxg').exists())


def source_files(source: Path, game: Path, crt: Path | None, wsl: bool) -> dict[Path, tuple[Path, bool]]:
    """Destination-relative file -> (source, immutable original-game bytes)."""
    result = {Path('game/Launch.sh'): (required_file(source, 'Launch.sh'), False)}
    release = source / 'build_native/Release'
    for name in REQUIRED:
        path = required_file(release, name)
        result[Path('game/build_native/Release') / path.name] = (path, False)
    for name in CRT:
        available = names_in(release).get(name.casefold())
        path = required_file(release, name) if available else required_file(crt or release, name)
        result[Path('game/build_native/Release') / path.name] = (path, False)
    # Include the full DLL set from the release and, when supplied, the x64 CRT.
    for directory in (release, crt):
        if directory:
            for path in directory.iterdir():
                if path.suffix.casefold() == '.dll':
                    if path.is_symlink() or not path.is_file() or not path.stat().st_size:
                        raise SetupError(f'Invalid release DLL: {path}')
                    key = Path('game/build_native/Release') / path.name
                    previous = next((key2 for key2 in result if str(key2).casefold() == str(key).casefold()), None)
                    if previous is None:
                        result[key] = (path, False)
    required_file(game, 'default.xex')
    roots = names_in(game)
    for name in ('Content', 'System'):
        item = roots.get(name.casefold())
        if item is None or item.is_symlink() or not item.is_dir():
            raise SetupError(f'Missing original game directory: {game / name}')
    for current, dirs, files in os.walk(game, followlinks=False, onerror=fail_game_walk):
        directory = Path(current)
        for name in dirs:
            if (directory / name).is_symlink():
                raise SetupError(f'Game folders must be real directories: {directory / name}')
        for name in files:
            path = directory / name
            if path.is_symlink() or not path.is_file():
                raise SetupError(f'Game files must be real files: {path}')
            relative = path.relative_to(game)
            parts = list(relative.parts)
            if parts[0].casefold() in ('content', 'system', 'default.xex'):
                parts[0] = {'content': 'Content', 'system': 'System', 'default.xex': 'default.xex'}[parts[0].casefold()]
            result[Path('game/Darkness').joinpath(*parts)] = (path, True)
    if wsl:
        result[Path('wsl_graphics.py')] = (required_file(source / 'tools', 'wsl_graphics.py'), False)
    return result


def archive_members(bundle: tarfile.TarFile, root: str, staging: Path) -> dict[tuple[str, ...], tarfile.TarInfo]:
    members = {}
    for item in bundle.getmembers():
        parts = PurePosixPath(item.name).parts
        if (not parts or parts[0] != root or '..' in parts or item.name.startswith('/') or
                '\\' in item.name or '\0' in item.name or parts in members):
            raise SetupError(f'Unsafe or duplicate archive path: {item.name}')
        if not (item.isfile() or item.isdir() or item.issym()) or item.islnk() or item.sparse:
            raise SetupError(f'Unsupported archive entry: {item.name}')
        if hasattr(tarfile, 'data_filter'):
            try:
                tarfile.data_filter(item, str(staging))
            except tarfile.TarError as error:
                raise SetupError(f'Unsafe archive entry: {item.name}: {error}') from error
        members[parts] = item
    if (root,) in members and not members[(root,)].isdir():
        raise SetupError(f'Archive root must be a directory: {root}')
    for parts in members:
        for length in range(1, len(parts)):
            ancestor = members.get(parts[:length])
            if ancestor and not ancestor.isdir():
                raise SetupError(f'Archive writes through a non-directory ancestor: {"/".join(parts)}')
    # Resolve link components before '..', including other archive symlinks.
    def resolve_link(start: tuple[str, ...], target: str, visiting: set[tuple[str, ...]]) -> tuple[str, ...]:
        if not target or target.startswith('/') or '\\' in target or '\0' in target:
            raise SetupError(f'Unsafe archive symlink target: {target}')
        resolved = list(start)
        for component in target.split('/'):
            if component in ('', '.'):
                continue
            if component == '..':
                if len(resolved) <= 1:
                    raise SetupError(f'Archive symlink escapes its root: {target}')
                resolved.pop()
                continue
            resolved.append(component)
            key = tuple(resolved)
            linked = members.get(key)
            if linked and linked.issym():
                if key in visiting or len(visiting) >= 40:
                    raise SetupError(f'Cyclic archive symlink: {"/".join(key)}')
                resolved = list(resolve_link(key[:-1], linked.linkname, visiting | {key}))
        return tuple(resolved)
    for parts, item in members.items():
        if item.issym():
            resolve_link(parts[:-1], item.linkname, {parts})
    return members


def extract_archive(archive: Path, staging: Path, root: str) -> Path:
    """Extract only validated directories/files, then contained relative links.

    Fresh private staging plus exclusive file creation avoids following archive
    links. No extractall, metadata ownership restoration, or recursive deletion.
    """
    if staging.exists() or staging.is_symlink():
        raise SetupError(f'Archive staging directory already exists: {staging}')
    no_links(staging.parent)
    with tarfile.open(archive, 'r:*') as bundle:
        members = archive_members(bundle, root, staging)
        staging.mkdir(mode=0o700)
        for parts, item in sorted(members.items(), key=lambda pair: len(pair[0])):
            destination = staging.joinpath(*parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if item.isdir():
                destination.mkdir(exist_ok=True)
            elif item.isfile():
                with bundle.extractfile(item) as original, destination.open('xb') as copied:
                    shutil.copyfileobj(original, copied, 1024 * 1024)
                destination.chmod(item.mode & 0o777)
        for parts, item in members.items():
            if item.issym():
                destination = staging.joinpath(*parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.symlink_to(item.linkname)
    return staging / root


def get_archive(spec: ToolSpec, supplied: Path | None, install: Path, skip_download: bool) -> Path:
    archive = supplied or install / 'downloads' / spec.url.rsplit('/', 1)[-1]
    if archive.is_symlink():
        raise SetupError(f'Archive must be a regular file: {archive}')
    if supplied and (archive.is_symlink() or not archive.is_file()):
        raise SetupError(f'Missing regular offline archive: {archive}')
    if not archive.exists():
        if skip_download:
            raise SetupError(f'{spec.name} archive missing; supply its pinned archive or omit --skip-download.')
        archive.parent.mkdir(parents=True, exist_ok=True)
        no_links(archive.parent)
        temporary = archive.with_name(archive.name + '.download-' + uuid.uuid4().hex)
        print(f'Downloading {spec.name} {spec.version} from {spec.url}', flush=True)
        request = urllib.request.Request(spec.url, headers={'User-Agent': 'DarkRecomp-Linux-Setup/1'})
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open('xb') as copied:
            shutil.copyfileobj(response, copied, 1024 * 1024)
        if hash_file(temporary) != spec.sha256:
            temporary.unlink()
            raise SetupError(f'{spec.name} download SHA256 differs from the pinned official release.')
        temporary.replace(archive)
    if hash_file(archive) != spec.sha256:
        raise SetupError(f'{spec.name} archive SHA256 differs from the pinned official release: {archive}')
    return archive


def installed_tool(spec: ToolSpec, install: Path, manifest: dict) -> bool:
    record = manifest.get('tools', {}).get(spec.name, {})
    if record.get('archive_sha256') != spec.sha256 or record.get('version') != spec.version:
        return False
    root = install / spec.root
    if root.is_symlink():
        return False
    for relative in spec.required:
        path = root / relative
        if not path.is_file() or not os.access(path, os.X_OK) or not under(path.resolve(), root.resolve()):
            return False
        if hash_file(path) != record.get('files', {}).get(relative):
            return False
    return True


def install_tool(spec: ToolSpec, archive: Path, install: Path) -> dict:
    destination = install / spec.root
    if destination.exists() or destination.is_symlink():
        raise SetupError(f'Existing {spec.name} installation differs or is unmanaged; preserve it and choose another install directory: {destination}')
    staging = install / ('.extract-' + spec.name + '-' + uuid.uuid4().hex)
    extracted = extract_archive(archive, staging, spec.root)
    for relative in spec.required:
        path = extracted / relative
        if not path.is_file() or not under(path.resolve(), extracted.resolve()):
            raise SetupError(f'Pinned {spec.name} archive is missing {relative}')
        path.chmod(path.stat().st_mode | 0o111)
    extracted.rename(destination)
    staging.rmdir()
    return {'version': spec.version, 'archive_sha256': spec.sha256, 'url': spec.url,
            'files': {relative: hash_file(destination / relative) for relative in spec.required}}


def adopt_tool(spec: ToolSpec, archive: Path, install: Path) -> dict:
    """Prove an existing tree matches every pinned archive member; change nothing."""
    destination = install / spec.root
    no_links(destination)
    if not destination.is_dir():
        raise SetupError(f'Existing tool is not a directory: {destination}')
    print(f'Verifying existing {spec.name} against its pinned archive...', flush=True)
    with tarfile.open(archive, 'r:*') as bundle:
        members = archive_members(bundle, spec.root, install / '.validation-unused')
        for parts, item in members.items():
            path = install.joinpath(*parts)
            if item.issym():
                if not path.is_symlink() or os.readlink(path) != item.linkname:
                    raise SetupError(f'Existing tool link differs from pinned archive: {path}')
            elif item.isdir():
                no_links(path)
                if path.is_symlink() or not path.is_dir():
                    raise SetupError(f'Existing tool directory differs from pinned archive: {path}')
            else:
                no_links(path)
                if path.is_symlink() or not path.is_file() or path.stat().st_size != item.size:
                    raise SetupError(f'Existing tool file differs from pinned archive: {path}')
                digest = hashlib.sha256()
                with bundle.extractfile(item) as original:
                    for chunk in iter(lambda: original.read(1024 * 1024), b''):
                        digest.update(chunk)
                if hash_file(path) != digest.hexdigest():
                    raise SetupError(f'Existing tool bytes differ from pinned archive: {path}')
    for relative in spec.required:
        if not os.access(destination / relative, os.X_OK):
            raise SetupError(f'Existing tool lacks executable permissions: {destination / relative}')
    return {'version': spec.version, 'archive_sha256': spec.sha256, 'url': spec.url,
            'files': {relative: hash_file(destination / relative) for relative in spec.required}}


def launcher_text(install: Path, wsl: bool) -> str:
    root = shlex.quote(str(install))
    text = f'''#!/usr/bin/env bash
set -euo pipefail
root={root}
export PATH="$root/umu:$PATH"
export WINEPREFIX="${{WINEPREFIX:-$root/ge-prefix}}"
export GAMEID="${{GAMEID:-umu-default}}"
export PROTONPATH="${{PROTONPATH:-$root/{PROTON.root}}}"
mode=play
case "${{1:-}}" in
    play|mute|check) mode=$1; shift ;;
    help|--help|-h) exec "$root/game/Launch.sh" help ;;
esac
'''
    if wsl:
        text += '''# Tested WSL defaults; later --width/--height/--fps arguments override them.
export DARKRECOMP_RUNTIME_PARENT="$root/wsl-pv"
export DARKRECOMP_SETUP_LOG_DIR="$root/evidence"
runtime_root="${XDG_DATA_HOME:-$HOME/.local/share}/umu/steamrt4"
export DARKRECOMP_REAL_BWRAP="${DARKRECOMP_REAL_BWRAP:-$runtime_root/pressure-vessel/libexec/steam-runtime-tools-0/srt-bwrap}"
DARKRECOMP_REAL_BWRAP=$(python3 -c 'import pathlib,sys; root=pathlib.Path(sys.argv[1]).resolve(); path=pathlib.Path(sys.argv[2]).resolve(); assert path==root or root in path.parents, "bwrap must remain in the UMU runtime"; print(path)' "$runtime_root" "$DARKRECOMP_REAL_BWRAP")
export DARKRECOMP_REAL_BWRAP
export PRESSURE_VESSEL_BWRAP="$root/wsl_graphics.py"
export PRESSURE_VESSEL_VARIABLE_DIR="$root/wsl-pv"
export PRESSURE_VESSEL_GC_RUNTIMES=0
export GALLIUM_DRIVER="${GALLIUM_DRIVER:-d3d12}"
export LD_LIBRARY_PATH="/usr/lib/wsl/lib:${LD_LIBRARY_PATH-}"
export PROTON_USE_WINED3D="${PROTON_USE_WINED3D:-1}"
if [[ $mode != check ]]; then
    exec "$root/game/Launch.sh" "$mode" --width 640 --height 360 --fps 30 "$@"
fi
'''
    return text + 'exec "$root/game/Launch.sh" "$mode" "$@"\n'


def atomic_bytes(destination: Path, contents: bytes, mode: int = 0o644) -> None:
    no_links(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='.' + destination.name + '-', dir=destination.parent, delete=False) as output:
        temporary = Path(output.name)
        output.write(contents)
    temporary.chmod(mode)
    temporary.replace(destination)


def setup(source: Path, game_dir: Path | None, install: Path, crt_dir: Path | None = None,
          umu_archive: Path | None = None, proton_archive: Path | None = None,
          skip_download: bool = False, check: bool = False, wsl: bool | None = None) -> dict:
    if not install.is_absolute():
        raise SetupError(f'--install-dir must be an absolute Linux path: {install}')
    no_links(install)  # Check the user-supplied spelling before resolve hides aliases.
    source = absolute(source, '--source')
    game = absolute(game_dir or source / 'Darkness', '--game-dir')
    install = absolute(install, '--install-dir')
    crt = absolute(crt_dir, '--crt-dir') if crt_dir else None
    for archive, label in ((umu_archive, '--umu-archive'), (proton_archive, '--proton-archive')):
        if archive is not None:
            absolute(archive, label)
    validate_install(install, source, game)
    wsl = is_wsl2() if wsl is None else wsl
    print('Reading the complete release and original game file list...', flush=True)
    files = source_files(source, game, crt, wsl)
    # Retain existing Windows filename casing instead of creating two DLLs
    # differing only by case on ext4 during a redist/release update.
    release_destination = install / 'game/build_native/Release'
    existing_names = names_in(release_destination) if release_destination.is_dir() else {}
    for relative in list(files):
        if relative.parent == Path('game/build_native/Release'):
            existing = existing_names.get(relative.name.casefold())
            if existing and existing.name != relative.name:
                files[relative.with_name(existing.name)] = files.pop(relative)
    manifest_path = install / MANIFEST
    old = {}
    if manifest_path.exists():
        no_links(manifest_path)
        old = json.loads(manifest_path.read_text())
        if not isinstance(old, dict) or not isinstance(old.get('files'), dict) or not isinstance(old.get('tools'), dict):
            raise SetupError('Existing setup manifest is invalid; preserve this install.')
        if old.get('setup_version') != VERSION:
            raise SetupError('Existing setup manifest has an unsupported version; preserve this install.')
    plan, hashes = [], {}
    print('Comparing release and complete original game files by SHA256; large dumps may take several minutes...', flush=True)
    for relative, (origin, immutable) in sorted(files.items(), key=lambda pair: str(pair[0])):
        destination = install / relative
        no_links(destination)
        digest = hash_file(origin)
        hashes[str(relative)] = digest
        if destination.exists():
            if not destination.is_file():
                raise SetupError(f'Copy destination is not a regular file: {destination}')
            current = hash_file(destination)
            if current == digest:
                continue
            if immutable:
                raise SetupError(f'Original game bytes differ; refusing to replace an existing asset: {destination}')
            if current != old.get('files', {}).get(str(relative)):
                raise SetupError(f'Destination was modified or is unmanaged; preserving it: {destination}')
        plan.append((origin, destination))
    launcher = launcher_text(install, wsl).encode()
    launcher_path = install / 'play-linux.sh'
    no_links(launcher_path)
    tools_ready = {spec.name: installed_tool(spec, install, old) for spec in (UMU, PROTON)}
    directories = [install / name for name in ('ge-prefix', 'wsl-pv', 'evidence')]
    game_directories = [install / 'game/Darkness']
    for current, dirs, _ in os.walk(game, followlinks=False, onerror=fail_game_walk):
        for name in dirs:
            relative = (Path(current) / name).relative_to(game)
            parts = list(relative.parts)
            if parts[0].casefold() in ('content', 'system'):
                parts[0] = {'content': 'Content', 'system': 'System'}[parts[0].casefold()]
            game_directories.append(install / 'game/Darkness' / Path(*parts))
    directories += game_directories
    for directory in directories:
        no_links(directory)
        if directory.exists() and not directory.is_dir():
            raise SetupError(f'Expected private directory: {directory}')
        if directory in (install / 'wsl-pv', install / 'evidence') and directory.exists():
            info = directory.stat()
            if hasattr(os, 'geteuid') and (info.st_uid != os.geteuid() or info.st_mode & 0o022):
                raise SetupError(f'Runtime/evidence directory must belong to you and not be writable by others: {directory}')
    if check:
        if not install.is_dir() or install.stat().st_mode & 0o077:
            raise SetupError('Linux installation directory must be private (0700); rerun setup to repair it.')
        if plan or not all(tools_ready.values()) or not launcher_path.is_file() or launcher_path.read_bytes() != launcher:
            raise SetupError('Linux setup is missing or differs from the source; run setup without --check.')
        if not os.access(launcher_path, os.X_OK) or any(not directory.is_dir() for directory in directories):
            raise SetupError('Linux setup is missing executable permissions or private runtime folders.')
        for relative in (Path('game/Launch.sh'), Path('wsl_graphics.py')):
            if relative in files and not os.access(install / relative, os.X_OK):
                raise SetupError(f'Linux launcher/hook is not executable: {install / relative}')
        if any((install / name).stat().st_mode & 0o077 for name in ('wsl-pv', 'evidence')):
            raise SetupError('WSL runtime/evidence folders must have private 0700 permissions; rerun setup to repair them.')
        print(f'Linux setup ready: {launcher_path}')
        return old
    # Resolve and validate archives before changing game/release files.
    install.mkdir(parents=True, exist_ok=True, mode=0o700)
    if install.stat().st_mode & 0o077:
        install.chmod(0o700)
    archives = {spec.name: get_archive(spec, supplied, install, skip_download)
                for spec, supplied in ((UMU, umu_archive), (PROTON, proton_archive)) if not tools_ready[spec.name]}
    for spec in (UMU, PROTON):
        if spec.name in archives:
            with tarfile.open(archives[spec.name], 'r:*') as bundle:
                archive_members(bundle, spec.root, install / '.validation-unused')
    tools = dict(old.get('tools', {}))
    for spec in (UMU, PROTON):
        if spec.name in archives:
            if (install / spec.root).exists() or (install / spec.root).is_symlink():
                tools[spec.name] = adopt_tool(spec, archives[spec.name], install)
            else:
                tools[spec.name] = install_tool(spec, archives[spec.name], install)
    if plan:
        print(f'Copying {len(plan)} changed/missing files as real Linux files; preserving existing saves/settings/prefix...', flush=True)
    for origin, destination in plan:
        # Stream copies to real files. Never link the Windows dump into Wine.
        no_links(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(prefix='.' + destination.name + '-', dir=destination.parent, delete=False) as output:
            temporary = Path(output.name)
            with origin.open('rb') as original:
                shutil.copyfileobj(original, output, 1024 * 1024)
        temporary.chmod(0o755 if destination.name in ('Launch.sh', 'wsl_graphics.py') else 0o644)
        temporary.replace(destination)
    # Preserve every existing prefix/save/settings file and all extra assets.
    for directory in directories:
        directory.mkdir(mode=0o700, exist_ok=True)
    for name in ('wsl-pv', 'evidence'):
        directory = install / name
        if directory.stat().st_mode & 0o077:
            directory.chmod(0o700)
    if not launcher_path.exists() or launcher_path.read_bytes() != launcher:
        atomic_bytes(launcher_path, launcher, 0o755)
    else:
        if launcher_path.stat().st_mode & 0o777 != 0o755:
            launcher_path.chmod(0o755)
    for relative in (Path('game/Launch.sh'), Path('wsl_graphics.py')):
        if relative in files:
            destination = install / relative
            if destination.stat().st_mode & 0o777 != 0o755:
                destination.chmod(0o755)
    manifest = {'setup_version': VERSION, 'source': str(source), 'game_source': str(game),
                'wsl2': wsl, 'files': hashes, 'tools': tools,
                'launcher_sha256': hashlib.sha256(launcher).hexdigest()}
    contents = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    if not manifest_path.exists() or manifest_path.read_bytes() != contents:
        atomic_bytes(manifest_path, contents)
    print(f'Linux setup ready. Play: {launcher_path}\nRead-only check: {launcher_path} check')
    if wsl:
        print('WSL uses the tested 640x360 / 30 FPS defaults. Override with --width W --height H --fps N.')
    print('UMU may download its Steam runtime on first play; setup does not launch the game.')
    return manifest


def main(argv: list[str] | None = None) -> int:
    if sys.version_info < (3, 10):
        print('Linux setup requires Python 3.10 or newer.', file=sys.stderr)
        return 1
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='Absolute Linux-accessible release/project root')
    parser.add_argument('--game-dir', type=Path, help='Complete owned dump (default: SOURCE/Darkness)')
    parser.add_argument('--install-dir', type=Path, default=Path.home() / '.local/share/darkrecomp')
    parser.add_argument('--crt-dir', type=Path, help='Linux-accessible Visual Studio x64 CRT redist directory')
    parser.add_argument('--umu-archive', type=Path, help='Local pinned UMU archive for offline installation')
    parser.add_argument('--proton-archive', type=Path, help='Local pinned GE-Proton archive for offline installation')
    parser.add_argument('--skip-download', action='store_true', help='Fail instead of downloading missing tool archives')
    parser.add_argument('--check', action='store_true', help='Read-only validation; no downloads, copies or prefix changes')
    args = parser.parse_args(argv)
    if sys.platform != 'linux':
        parser.error('Run this helper inside Linux/WSL; Windows users should use SetupLinux.cmd.')
    try:
        setup(args.source, args.game_dir, args.install_dir, args.crt_dir,
              args.umu_archive, args.proton_archive, args.skip_download, args.check)
    except (SetupError, OSError, ValueError, tarfile.TarError) as error:
        print(f'Linux setup failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
