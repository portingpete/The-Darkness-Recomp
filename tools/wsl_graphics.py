#!/usr/bin/python3
"""Expose WSL GPU libraries in a private pressure-vessel runtime copy.

Set DARKRECOMP_RUNTIME_PARENT, DARKRECOMP_REAL_BWRAP and optionally
DARKRECOMP_SETUP_LOG_DIR, then use this executable as PRESSURE_VESSEL_BWRAP.
The source runtime stays untouched: /usr receives an invisible tmpfs overlay,
WSL GPU files are bound read-only, and pv-adverb retains their library path.
"""
from dataclasses import dataclass
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import sys
import time

MAX_ARGS_BYTES = 4 * 1024 * 1024
WSL_PATH = '/usr/lib/wsl'
WSL_LIB = WSL_PATH + '/lib'
ADVERBS = {
    '/usr/lib/pressure-vessel/from-host/libexec/steam-runtime-tools-0/pv-adverb',
    '/run/pressure-vessel/pv-from-host/libexec/steam-runtime-tools-0/pv-adverb',
}
PROBES = {'true', '/bin/true', '/usr/bin/true', 'readlink', '/bin/readlink', '/usr/bin/readlink'}
ARITIES = {name: 0 for name in (
    '--help', '--version', '--level-prefix', '--unshare-all', '--share-net',
    '--unshare-user', '--unshare-user-try', '--unshare-ipc', '--unshare-pid',
    '--unshare-net', '--unshare-uts', '--unshare-cgroup', '--unshare-cgroup-try',
    '--disable-userns', '--assert-userns-disabled', '--clearenv', '--new-session',
    '--die-with-parent', '--as-pid-1', '--not-a-security-boundary')}
ARITIES.update({name: 1 for name in (
    '--argv0', '--userns', '--userns2', '--pidns', '--uid', '--gid', '--hostname',
    '--chdir', '--unsetenv', '--lock-file', '--sync-fd', '--remount-ro',
    '--overlay-src', '--tmp-overlay', '--ro-overlay', '--exec-label', '--file-label',
    '--proc', '--dev', '--tmpfs', '--mqueue', '--dir', '--seccomp', '--add-seccomp-fd',
    '--block-fd', '--userns-block-fd', '--info-fd', '--json-status-fd',
    '--cap-add', '--cap-drop', '--perms', '--size')})
ARITIES.update({name: 2 for name in (
    '--setenv', '--bind', '--bind-try', '--dev-bind', '--dev-bind-try',
    '--ro-bind', '--ro-bind-try', '--bind-fd', '--ro-bind-fd', '--file',
    '--bind-data', '--ro-bind-data', '--symlink', '--chmod')})
ARITIES['--overlay'] = 3
BIND_OPTIONS = {'--bind', '--ro-bind'}


class HookError(ValueError):
    """Unsupported or unsafe runtime invocation."""


@dataclass(frozen=True)
class Settings:
    runtime_parent: str
    real_bwrap: str
    log_dir: str | None = None


@dataclass(frozen=True)
class Rewrite:
    argv: list[str]
    options: list[str] | None
    changed: bool
    source: str | None = None


def canonical_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (not value or '\0' in value or value.startswith('//') or not path.is_absolute()
            or str(path) != value or '..' in path.parts):
        raise HookError(f'Expected an absolute canonical Linux path: {value!r}')
    return path


def path_without_links(value: str, *, directory: bool, lstat=None):
    """Inspect each ancestor without following symlinks or Windows reparse points."""
    path = canonical_path(value)
    lstat = lstat or (lambda item: Path(item).lstat())
    current = PurePosixPath('/')
    info = None
    for part in ('/', *path.parts[1:]):
        if part != '/':
            current /= part
        info = lstat(str(current))
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise HookError(f'Link or reparse point in path: {current}')
        if current != path or directory:
            if not stat.S_ISDIR(info.st_mode):
                raise HookError(f'Expected directory: {current}')
    return info


def filesystem_type(value: str, mountinfo: str) -> str:
    """Choose the containing mount without confusing adjacent path prefixes."""
    path = canonical_path(value)
    best = (-1, '')
    for line in mountinfo.splitlines():
        fields = line.split()
        if len(fields) < 7 or '-' not in fields:
            continue
        separator = fields.index('-')
        if separator + 1 >= len(fields):
            continue
        mount = re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), fields[4])
        mount_path = PurePosixPath(mount)
        if path == mount_path or mount_path in path.parents:
            if len(mount_path.parts) > best[0]:
                best = (len(mount_path.parts), fields[separator + 1])
    return best[1]


def validate_settings(settings: Settings, *, lstat=None, mountinfo=None, uid=None):
    uid = os.geteuid() if uid is None else uid
    parent = canonical_path(settings.runtime_parent)
    if parent == PurePosixPath('/'):
        raise HookError('Runtime parent must be a private directory')
    info = path_without_links(str(parent), directory=True, lstat=lstat)
    if info.st_uid != uid or info.st_mode & 0o022:
        raise HookError('Runtime parent must be owned by this user and not writable by other users')
    if mountinfo is None:
        mountinfo = Path('/proc/self/mountinfo').read_text()
    if filesystem_type(str(parent), mountinfo) != 'ext4':
        raise HookError('Private runtime copies must be on ext4')
    binary = canonical_path(settings.real_bwrap)
    info = path_without_links(str(binary), directory=False, lstat=lstat)
    if binary.name != 'srt-bwrap' or not stat.S_ISREG(info.st_mode) or not info.st_mode & 0o111:
        raise HookError('DARKRECOMP_REAL_BWRAP must be the executable bundled srt-bwrap')
    if settings.log_dir:
        info = path_without_links(settings.log_dir, directory=True, lstat=lstat)
        if info.st_uid != uid or info.st_mode & 0o077:
            raise HookError('Diagnostics directory must be owned by this user with private permissions')


def runtime_source(value: str, parent: str) -> str:
    source = canonical_path(value)
    root = canonical_path(parent)
    if (source.name != 'usr' or source.parent.parent != root
            or not re.fullmatch(r'tmp-[A-Za-z0-9]{6}', source.parent.name)):
        raise HookError('Unexpected runtime /usr source outside the private copy parent')
    return str(source)


def decode_args(data: bytes) -> list[str]:
    if not data or len(data) > MAX_ARGS_BYTES or not data.endswith(b'\0'):
        raise HookError('Malformed NUL-separated --args payload')
    # Unix argv bytes use surrogateescape; keep the helper portable to Windows
    # hosts that run the contract tests before copying this hook into WSL.
    return [part.decode('utf-8', 'surrogateescape') for part in data[:-1].split(b'\0')]


def args_fd(argv: list[str]) -> int | None:
    if not argv or argv[0] != '--args':
        return None
    if len(argv) < 3 or not re.fullmatch(r'[0-9]+', argv[1]):
        raise HookError('Malformed --args descriptor or missing runtime entry')
    fd = int(argv[1])
    if not 3 <= fd <= 2**31 - 1:
        raise HookError('Invalid --args descriptor')
    return fd


def scan_options(options: list[str], *, bundled: bool):
    entries = []
    index = 0
    while index < len(options):
        option = options[index]
        if option == '--':
            if bundled:
                raise HookError('Program separator inside bundled runtime options')
            return entries, options[index + 1:]
        if not option.startswith('--'):
            if bundled:
                raise HookError('Unexpected program inside bundled runtime options')
            return entries, options[index:]
        if option not in ARITIES:
            raise HookError(f'Unsupported bubblewrap option: {option}')
        count = ARITIES[option]
        if index + count >= len(options):
            raise HookError(f'Missing argument to {option}')
        entries.append((index, option, options[index + 1:index + count + 1]))
        index += count + 1
    return entries, []


def rewrite(argv: list[str], options: list[str] | None, settings: Settings) -> Rewrite:
    """Pure argv transformation; no shell, mounts, file writes or process actions."""
    if any('\0' in item for item in [*argv, *(options or [])]):
        raise HookError('NUL in argument')
    bundled = args_fd(argv) is not None
    if bundled != (options is not None):
        raise HookError('Missing or unexpected --args payload')
    entries, command = scan_options(options if bundled else argv, bundled=bundled)
    mounts = []
    for i, option, values in entries:
        if option not in {'--bind', '--ro-bind', '--bind-try', '--ro-bind-try',
                          '--dev-bind', '--dev-bind-try', '--bind-fd', '--ro-bind-fd',
                          '--tmpfs', '--tmp-overlay', '--ro-overlay', '--overlay', '--symlink'}:
            continue
        if posixpath.normpath(values[-1]).lstrip('/') == 'usr':
            if values[-1] != '/usr':
                raise HookError('Noncanonical runtime /usr destination')
            mounts.append((i, option, values))
    if not mounts:
        return Rewrite(argv.copy(), options.copy() if options is not None else None, False)
    if not bundled:
        if len(mounts) != 1 or not command or command[0] not in PROBES:
            raise HookError('Unbundled runtime /usr launch is not a recognized setup probe')
        _, option, values = mounts[0]
        if option not in BIND_OPTIONS:
            raise HookError('Unexpected setup probe /usr mount')
        source = None if values[0] == '/usr' else runtime_source(values[0], settings.runtime_parent)
        return Rewrite(argv.copy(), None, False, source)
    if len(mounts) != 1 or mounts[0][1] not in BIND_OPTIONS:
        raise HookError('Expected one ordinary runtime /usr bind')
    at, _, values = mounts[0]
    source = runtime_source(values[0], settings.runtime_parent)
    if argv[2] not in ADVERBS:
        raise HookError('Unexpected pv-adverb runtime entry')
    try:
        end = argv.index('--', 3)
    except ValueError as error:
        raise HookError('Missing pv-adverb program separator') from error
    setters = [i for i in range(3, end) if argv[i] == '--set-ld-library-path']
    if len(setters) != 1 or setters[0] + 1 >= end:
        raise HookError('Expected one complete pv-adverb library-path setter')
    setter = setters[0] + 1
    if argv[setter].startswith('--'):
        raise HookError('Missing pv-adverb library-path value')
    forwarded = argv.copy()
    paths = forwarded[setter].split(':')
    forwarded[setter] = ':'.join([WSL_LIB, *(path for path in paths if path != WSL_LIB)])
    modified = options[:at] + ['--overlay-src', source, '--tmp-overlay', '/usr'] + options[at + 3:]
    modified += ['--ro-bind', WSL_PATH, WSL_PATH, '--remount-ro', '/usr']
    return Rewrite(forwarded, modified, True, source)


def main(argv=None, environ=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    environ = os.environ if environ is None else environ
    try:
        if sys.platform != 'linux':
            raise HookError('The WSL graphics hook requires Linux')
        settings = Settings(environ.get('DARKRECOMP_RUNTIME_PARENT', ''),
                            environ.get('DARKRECOMP_REAL_BWRAP', ''),
                            environ.get('DARKRECOMP_SETUP_LOG_DIR') or None)
        validate_settings(settings)
        fd = args_fd(argv)
        options = None
        if fd is not None:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                raise HookError('--args must reference a seekable regular file or memfd')
            options = decode_args(os.pread(fd, MAX_ARGS_BYTES + 1, 0))
        plan = rewrite(argv, options, settings)
        if plan.source:
            path_without_links(plan.source, directory=True)
        if not plan.changed:
            os.execv(settings.real_bwrap, [settings.real_bwrap, *argv])
        if not Path(WSL_LIB).is_dir() or not Path(WSL_PATH + '/drivers').is_dir():
            raise HookError('WSL GPU library or driver directory is unavailable')
        if settings.log_dir:
            log = Path(settings.log_dir) / f'wsl-bwrap-{time.time_ns()}-{os.getpid()}.json'
            log_fd = os.open(log, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
            with os.fdopen(log_fd, 'w') as stream:
                json.dump({'original': argv, 'forwarded_modified': plan.argv,
                           'bundled_original': options, 'bundled_modified': plan.options}, stream, indent=2)
                stream.write('\n')
        replacement = os.memfd_create('darkrecomp-wsl-bwrap-args', 0)
        try:
            with os.fdopen(os.dup(replacement), 'wb') as stream:
                stream.write(b'\0'.join(os.fsencode(item) for item in plan.options) + b'\0')
            os.lseek(replacement, 0, os.SEEK_SET)
            os.set_inheritable(replacement, True)
            forwarded = plan.argv.copy()
            forwarded[1] = str(replacement)
            os.execv(settings.real_bwrap, [settings.real_bwrap, *forwarded])
        finally:
            os.close(replacement)
    except (HookError, OSError) as error:
        print(f'[DarkRecomp WSL graphics] {error}', file=sys.stderr)
        return 125


if __name__ == '__main__':
    raise SystemExit(main())
