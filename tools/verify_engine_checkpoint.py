"""Independently verify checkpoint hashes and reconstruct every source from its patch."""
import argparse
import hashlib
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return {'bytes': path.stat().st_size, 'sha256': h.hexdigest()}


def workspace_path(name):
    path = (root / name).resolve()
    if not path.is_relative_to(root):
        raise ValueError(f'Path escapes workspace: {name}')
    return path


def verify(manifest_path):
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    for name, expected in manifest['files'].items():
        if digest(workspace_path(name)) != expected:
            raise ValueError(f'Hash/extent differs: {name}')
    patch = manifest_path.with_suffix('').with_suffix('.patch')
    sections = re.split(r'^diff --git a/([^\n]+) b/[^\n]+\n', patch.read_text(encoding='utf-8'), flags=re.M)
    if set(sections[1::2]) != set(manifest['sources']) or len(sections[1::2]) != len(manifest['sources']):
        raise ValueError('Patch source inventory differs')
    for name, body in zip(sections[1::2], sections[2::2]):
        before = manifest['sources'][name]['before']
        old = workspace_path(before['path']).read_text(encoding='utf-8-sig').splitlines(keepends=True) if before else []
        if before and digest(workspace_path(before['path'])) != {k: before[k] for k in ('bytes', 'sha256')}:
            raise ValueError(f'Baseline hash differs: {name}')
        lines = body.splitlines(keepends=True)
        output, cursor, i = [], 0, 0
        while i < len(lines):
            match = re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@', lines[i])
            if not match:
                i += 1
                continue
            start = max(int(match[1]) - 1, 0)
            if start < cursor or start > len(old):
                raise ValueError(f'Invalid hunk position: {name}')
            output.extend(old[cursor:start])
            cursor, i = start, i + 1
            while i < len(lines) and not lines[i].startswith('@@ '):
                line = lines[i]
                i += 1
                if i < len(lines) and lines[i].startswith('\\ No newline at end of file'):
                    line = line.rstrip('\n')
                    i += 1
                if not line or line[0] not in ' +-':
                    raise ValueError(f'Invalid patch line: {name}')
                if line[0] in ' -':
                    if cursor >= len(old) or old[cursor] != line[1:]:
                        raise ValueError(f'Patch context differs: {name}:{cursor + 1}')
                    cursor += 1
                if line[0] in ' +':
                    output.append(line[1:])
        output.extend(old[cursor:])
        if ''.join(output) != workspace_path(name).read_text(encoding='utf-8-sig'):
            raise ValueError(f'Reconstructed source differs: {name}')
    return {'hashed_files_verified': len(manifest['files']), 'source_files_reconstructed': len(manifest['sources']),
            'manifest': digest(manifest_path), 'patch': digest(patch)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest')
    args = parser.parse_args()
    path = workspace_path(args.manifest)
    result = verify(path)
    # Never replace a previous verification report.
    with path.with_suffix('.verification.json').open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps(result, indent=2))
