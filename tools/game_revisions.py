"""Pin approved XEX revisions to the shared AOT code and loader contracts."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import struct
import tomllib

from xex_image import decode_xex


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _slice(data: bytes, offset: int, size: int) -> bytes:
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError('Truncated AOT compatibility metadata')
    return data[offset:offset + size]


def compatibility_sha256(header: bytes, image: bytes) -> str:
    """Hash translation inputs while allowing the approved localization data.

    All PE headers, code sections, unwind metadata, TLS and import identities
    must agree. XEX payload offsets may move; decoded metadata must agree.
    This fingerprint is an additional generation gate, never an alternative to
    matching an approved whole-XEX and whole-image hash pair.
    """
    def word(data, offset):
        return struct.unpack('>I', _slice(data, offset, 4))[0]

    count = word(header, 20)
    pairs = list(struct.iter_unpack('>II', _slice(header, 24, count * 8)))
    fields = dict(pairs)
    if len(fields) != len(pairs):
        raise ValueError('Duplicate XEX optional header')

    def payload(key, minimum):
        if key not in fields:
            raise ValueError(f'Missing AOT compatibility header 0x{key:X}')
        size = word(header, fields[key]) if key & 0xff == 0xff else (key & 0xff) * 4
        if size < minimum:
            raise ValueError('Invalid AOT compatibility header size')
        return _slice(header, fields[key], size)

    if image[:2] != b'MZ':
        raise ValueError('Invalid PE image for AOT compatibility')
    pe = struct.unpack('<I', _slice(image, 0x3c, 4))[0]
    if _slice(image, pe, 4) != b'PE\0\0':
        raise ValueError('Invalid PE signature for AOT compatibility')
    section_count = struct.unpack('<H', _slice(image, pe + 6, 2))[0]
    optional_size = struct.unpack('<H', _slice(image, pe + 20, 2))[0]
    if not 1 <= section_count <= 96 or optional_size < 64:
        raise ValueError('Invalid PE layout for AOT compatibility')
    if struct.unpack('<H', _slice(image, pe + 24, 2))[0] != 0x10b:
        raise ValueError('Unsupported PE format for AOT compatibility')
    sections = pe + 24 + optional_size
    table = _slice(image, sections, section_count * 40)
    translated = []
    has_code = False
    for offset in range(0, len(table), 40):
        entry = table[offset:offset + 40]
        size, rva = struct.unpack_from('<II', entry, 8)
        flags = struct.unpack_from('<I', entry, 36)[0]
        if flags & 0x20 or entry[:8].rstrip(b'\0') == b'.pdata':
            contents = _slice(image, rva, size)
            has_code |= bool(flags & 0x20)
            translated.append([entry[:8].hex(), rva, size, _digest(contents)])
    if not has_code or not any(bytes.fromhex(s[0]).rstrip(b'\0') == b'.pdata' for s in translated):
        raise ValueError('Missing AOT code or unwind metadata')

    tls = payload(0x20104, 16)
    _, tls_address, tls_size, tls_raw_size = struct.unpack('>IIII', tls)
    if tls_raw_size > tls_size:
        raise ValueError('Invalid AOT TLS template size')
    if 0x10100 not in fields or 0x10201 not in fields:
        raise ValueError('Missing AOT entry point or image base')
    image_base = fields[0x10201]
    tls_template = _slice(image, tls_address - image_base, tls_raw_size)

    imports = payload(0x103ff, 12)
    _, string_size, libraries = struct.unpack_from('>III', imports)
    _slice(imports, 12, string_size)
    position = 12 + string_size
    thunks = []
    if libraries > (len(imports) - position) // 40:
        raise ValueError('Invalid AOT import library count')
    for _ in range(libraries):
        library_size = word(imports, position)
        library = _slice(imports, position, library_size)
        entries = struct.unpack('>H', _slice(library, 38, 2))[0]
        descriptors = _slice(library, 40, entries * 4)
        for (address,) in struct.iter_unpack('>I', descriptors):
            descriptor = _slice(image, address - image_base, 4)
            size = 16 if descriptor[0] else 4
            thunks.append([address, _slice(image, address - image_base, size).hex()])
        position += library_size
    if position != len(imports):
        raise ValueError('Unexpected AOT import metadata')

    contracts = {
        'format': 1, 'image_size': len(image), 'image_base': image_base,
        'entry_point': fields[0x10100],
        'pe_headers_sha256': _digest(_slice(image, 0, sections + len(table))),
        'translated_sections': translated,
        'tls_sha256': _digest(tls), 'tls_template_sha256': _digest(tls_template),
        'imports_sha256': _digest(imports), 'import_thunks': thunks,
    }
    return _digest(json.dumps(contracts, sort_keys=True, separators=(',', ':')).encode())


def load_catalog(path: Path) -> tuple[str, list[dict[str, str]]]:
    catalog = tomllib.loads(path.read_text(encoding='utf-8'))
    fingerprint = catalog.get('compatibility_sha256')
    revisions = catalog.get('revision')
    is_hash = lambda value: isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value)
    if catalog.get('format') != 1 or not is_hash(fingerprint) or not isinstance(revisions, list) or not revisions:
        raise ValueError('Invalid game revision catalog')
    seen = set()
    for revision in revisions:
        if (not isinstance(revision, dict) or
                set(revision) != {'name', 'xex_sha256', 'image_sha256'} or
                not isinstance(revision['name'], str) or
                not re.fullmatch('[ -~]{1,128}', revision['name']) or not revision['name'].strip() or
                not is_hash(revision['xex_sha256']) or not is_hash(revision['image_sha256'])):
            raise ValueError('Invalid game revision record')
        if revision['xex_sha256'] in seen:
            raise ValueError('Duplicate game revision XEX hash')
        seen.add(revision['xex_sha256'])
    return fingerprint, revisions


def validate_image(root: Path, xex: bytes, header: bytes, image: bytes) -> list[dict[str, str]]:
    fingerprint, revisions = load_catalog(root / 'config/game_revisions.toml')
    xex_hash = _digest(xex)
    revision = next((r for r in revisions if r['xex_sha256'] == xex_hash), None)
    if revision is None:
        raise RuntimeError('Unsupported default.xex revision; AOT generation requires an approved game dump')
    if revision['image_sha256'] != _digest(image):
        raise RuntimeError('Decoded game image differs from its approved revision')
    if compatibility_sha256(header, image) != fingerprint:
        raise RuntimeError('Game revision code or loader contracts differ from the shared AOT profile')
    return revisions


def validate_source(root: Path) -> list[dict[str, str]]:
    xex = (root / 'Darkness/default.xex').read_bytes()
    _, revisions = load_catalog(root / 'config/game_revisions.toml')
    xex_hash = _digest(xex)
    if not any(r['xex_sha256'] == xex_hash for r in revisions):
        raise RuntimeError('Unsupported default.xex revision; AOT generation requires an approved game dump')
    header, image = decode_xex(xex)
    return validate_image(root, xex, header, image)
