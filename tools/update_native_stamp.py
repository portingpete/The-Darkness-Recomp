"""Invalidate native objects when source contents change, regardless of timestamps."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import tempfile

EXTENSIONS = {'.cpp', '.h', '.hpp', '.inl', '.hlsl'}


def update(root: Path, output: Path) -> bool:
    root = root.resolve()
    tool = Path(__file__).resolve()
    sources = {'CMakeLists.txt': root / 'CMakeLists.txt',
               '@stamp-tool': tool,
               '@stamp-module': tool.parent.parent / 'cmake/NativeCompilationStamp.cmake'}
    sources['runtime/movement/src/lib.rs'] = root / 'runtime/movement/src/lib.rs'
    sources['runtime/movement/src/commands.rs'] = root / 'runtime/movement/src/commands.rs'
    sources['runtime/movement/src/camera.rs'] = root / 'runtime/movement/src/camera.rs'
    sources['@movement-build-module'] = tool.parent.parent / 'cmake/RustMovement.cmake'
    for directory in ('app', 'renderer', 'runtime/native', 'tests'):
        for path in (root / directory).rglob('*'):
            if path.is_file() and path.suffix.lower() in EXTENSIONS:
                sources[path.relative_to(root).as_posix()] = path
    crate = root / 'runtime/dualsense'
    if (crate / 'Cargo.toml').is_file():
        for path in [crate / 'Cargo.toml', crate / 'Cargo.lock', *sorted((crate / 'src').rglob('*.rs'))]:
            sources[path.relative_to(root).as_posix()] = path
        sources['@dualsense-build-tool'] = tool.parent / 'build_dualsense.py'
        sources['@dualsense-build-module'] = tool.parent.parent / 'cmake/RustDualSense.cmake'
        sources['@resource-wait-build-module'] = tool.parent.parent / 'cmake/RustResourceWait.cmake'
        sources['renderer/rust_resource_wait/src/lib.rs'] = root / 'renderer/rust_resource_wait/src/lib.rs'
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest()
              for name, path in sorted(sources.items())}
    fingerprint = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()
    header = ('#pragma once\n'
              f'#define DARK_NATIVE_INPUT_FINGERPRINT "{fingerprint}"\n'
              f'#pragma detect_mismatch("DarkNativeInputs", "{fingerprint}")\n').encode()
    manifest = (json.dumps({'fingerprint': fingerprint, 'inputs': hashes}, indent=2) + '\n').encode()
    output.parent.mkdir(parents=True, exist_ok=True)

    def publish(path: Path, contents: bytes) -> bool:
        if path.is_file() and path.read_bytes() == contents:
            return False
        # Atomic replacement leaves readers with either complete revision.
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
            temporary.write(contents)
            temporary_path = Path(temporary.name)
        try:
            os.replace(temporary_path, path)
        finally:
            temporary_path.unlink(missing_ok=True)
        return True

    changed = publish(output, header)
    publish(output.with_suffix('.json'), manifest)
    print(f'Native inputs {fingerprint}: {len(hashes)} files, '
          f'{"recompile required" if changed else "unchanged"}.')
    return changed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    update(args.root, args.output)
