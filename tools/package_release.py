"""Build, test, and package a Windows release without local game or user data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BINARIES = (
    'DarkRecomp.exe', 'DarkRecompPreview.exe',
    'avcodec-darkxma-62.dll', 'avutil-darkxma-60.dll', 'libwinpthread-1.dll',
    'CubeWnd.pc.xcr', 'GameContext_Create.pc.xdf',
)
DOCUMENTS = ('Launch.cmd', 'START_HERE.txt', 'README.md', 'CONTROLS.md', 'RENDERING.md', 'COPYING')
CRT_REQUIRED = ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')


def find_crt(root: Path) -> Path:
    # Use the toolchain that built these executables, not an unrelated VS install.
    cache = (root / 'build_native/CMakeCache.txt').read_text(encoding='utf-8')
    match = re.search(r'^CMAKE_GENERATOR_INSTANCE:INTERNAL=(.+)$', cache, re.MULTILINE)
    if not match:
        raise RuntimeError('Cannot locate Visual Studio from CMakeCache.txt; pass --crt-dir.')
    redist = Path(match[1].strip()) / 'VC/Redist/MSVC'
    candidates = list(redist.glob('*/x64/Microsoft.VC*.CRT'))
    candidates.sort(key=lambda path: tuple(int(part) for part in path.parents[1].name.split('.')))
    for path in reversed(candidates):
        if all((path / name).is_file() for name in CRT_REQUIRED):
            return path
    raise RuntimeError('Visual C++ x64 redistributable files not found; pass --crt-dir.')


def collect_files(root: Path, crt: Path) -> dict[str, Path]:
    files = {name: root / name for name in DOCUMENTS}
    for name in BINARIES:
        files[f'build_native/Release/{name}'] = root / 'build_native/Release' / name
    for name in CRT_REQUIRED:
        if not (crt / name).is_file():
            raise RuntimeError(f'Missing Visual C++ runtime: {crt / name}')
    for path in crt.glob('*.dll'):
        files[f'build_native/Release/{path.name}'] = path
    deps = root / 'build_native/deps'
    for name in ('COPYING.LGPLv2.1', 'COPYING.winpthreads', 'LICENSE.md', 'PROVENANCE.json'):
        files[f'ThirdParty/audio/{name}'] = deps / 'ffmpeg-darkxma' / name
    files['ThirdParty/audio/ffmpeg-darkxma-upstream.tar.gz'] = deps / 'ffmpeg-darkxma-upstream.tar.gz'
    files['ThirdParty/audio/ffmpeg-darkxma.patch'] = deps / 'ffmpeg-darkxma.patch'
    files['ThirdParty/audio/build_xma_codec.py'] = root / 'tools/build_xma_codec.py'
    for name, path in files.items():
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f'Missing or empty release file: {name} ({path})')
    return files


def package(root: Path, crt: Path, output: Path, version: str, revision: str) -> Path:
    if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?', version):
        raise ValueError('Use a release version such as v0.1.1.')
    files = collect_files(root, crt)
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f'The-Darkness-Recomp-{version}-windows-x64.zip'
    checksum = archive.with_suffix('.zip.sha256')
    if archive.exists() or checksum.exists():
        raise FileExistsError(f'Refusing to overwrite an existing release: {archive}')
    manifest = {
        'version': version, 'commit': revision,
        'source': f'https://github.com/portingpete/The-Darkness-Recomp/tree/{revision}',
        'sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest()
                   for name, path in sorted(files.items())},
    }
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for name, path in sorted(files.items()):
            bundle.write(path, name)
        bundle.writestr('Darkness/PUT_GAME_FILES_HERE.txt',
                        'Copy ALL files and folders from your own extracted Xbox 360 dump here.\r\n'
                        'See START_HERE.txt beside Launch.cmd for the required folder layout.\r\n')
        bundle.writestr('RELEASE.json', json.dumps(manifest, indent=2) + '\n')
        bundle.writestr('ThirdParty/README.txt',
                        'Audio: FFmpeg (LGPL-2.1-or-later), with source, patch, build script,\r\n'
                        'provenance and license notices in audio/. The build script is also\r\n'
                        'available under tools/ at the source revision in RELEASE.json.\r\n'
                        'libwinpthread-1.dll: license in audio/COPYING.winpthreads.\r\n'
                        'Microsoft Visual C++ runtime DLLs: redistributed unmodified from\r\n'
                        'the Visual Studio x64 CRT redistributable directory.\r\n'
                        'The Darkness Recomp: GPLv3; see COPYING and the source in RELEASE.json.\r\n')
    with zipfile.ZipFile(archive) as bundle:
        if bundle.testzip() is not None:
            raise RuntimeError('Release ZIP verification failed.')
    checksum.write_text(f'{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n', encoding='utf-8')
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build_native/releases')
    parser.add_argument('--crt-dir', type=Path)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Windows is required to build this release.')
    if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?', args.version):
        parser.error('Use a release version such as v0.1.1.')
    status = subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)
    if status.strip():
        raise RuntimeError('Commit your changes before packaging a release.')
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                    str(ROOT / 'tools/build.ps1'), '-ReleasePackage'], cwd=ROOT, check=True)
    if subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip():
        raise RuntimeError('Source checkout changed during the build; refusing to package.')
    current = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if current != revision:
        raise RuntimeError('Source revision changed during the build; refusing to package.')
    print(package(ROOT, args.crt_dir or find_crt(ROOT), args.output_dir, args.version, revision))


if __name__ == '__main__':
    main()
