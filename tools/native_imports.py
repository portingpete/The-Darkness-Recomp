"""Extract import identities from this XEX; generate strong fail-fast defaults."""
import hashlib
import json
from pathlib import Path
import re
import struct
from game_revisions import validate_image
from xex_image import decode_xex


def generate(root: Path, out: Path) -> None:
    xex = (root / "Darkness/default.xex").read_bytes()
    xex_header, image = decode_xex(xex)
    revisions = validate_image(root, xex, xex_header, image)
    header = (out / "ppc_recomp_shared.h").read_text()
    names = set(re.findall(r"PPC_EXTERN_FUNC\((__imp__\w+)\)", header))
    native_sources = "\n".join(p.read_text() for p in sorted((root / "runtime/native").glob("*.cpp")))
    implemented = set(re.findall(r"PPC_FUNC\((__imp__\w+)\)", native_sources))
    missing = sorted(names - implemented)
    (out / "ppc_imports.cpp").write_text('#include "ppc_context.h"\n' + "".join(
        f'PPC_FUNC({name}) {{ PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unimplemented import {name}"); }}\n'
        for name in missing))
    count = struct.unpack_from(">I", xex, 20)[0]
    optional = dict(struct.iter_unpack(">II", xex[24:24 + count * 8]))
    start = optional[0x103FF]
    _, string_size, libraries = struct.unpack_from(">III", xex, start)
    pos = start + 12 + string_size
    exports = {}
    for lib in ("xboxkrnl", "xam"):
        inc = (root / f"refs/UnleashedRecomp/tools/XenonRecomp/XenonUtils/xbox/{lib}_table.inc").read_text()
        exports[lib] = dict((int(a, 16), b) for a, b in re.findall(
            r"XE_EXPORT\([^,]+,\s*0x([0-9A-Fa-f]+),\s*(\w+)", inc))
    data_imports = []
    for i in range(libraries):
        size = struct.unpack_from(">I", xex, pos)[0]
        _, entries = struct.unpack_from(">HH", xex, pos + 36)
        descriptors = []
        for j in range(entries):
            address = struct.unpack_from(">I", xex, pos + 40 + j * 4)[0]
            word = struct.unpack_from(">I", image, address - 0x82000000)[0]
            descriptors.append((address, word & 0xffff, word >> 24))
        callable_ordinals = {ordinal for _, ordinal, kind in descriptors if kind}
        # Library strings are padded to four-byte boundaries.
        strings = xex[start + 12:start + 12 + string_size]
        libnames = [s.decode() for s in strings.split(b'\0') if s]
        library = libnames[i].split('.')[0]
        for address, ordinal, kind in descriptors:
            if kind == 0 and ordinal not in callable_ordinals:
                name = exports[library].get(ordinal)
                if name is None:
                    raise RuntimeError(f"Unknown data import {library}:{ordinal}")
                data_imports.append((address, name))
        pos += size
    text = '#pragma once\n#include <cstdint>\n#include "runtime/native/xex_image.h"\n'
    text += f'inline constexpr char kGameImageSha256[] = "{hashlib.sha256(image).hexdigest()}";\n'
    text += f'inline constexpr char kGameXexSha256[] = "{hashlib.sha256(xex).hexdigest()}";\n'
    text += 'inline constexpr DarkRecomp::Native::GameRevision kGameRevisions[] = {\n'
    for revision in revisions:
        # JSON string escaping also produces valid ASCII C++ string literals.
        fields = (revision['xex_sha256'], revision['image_sha256'], revision['name'])
        text += '  {' + ', '.join(json.dumps(value, ensure_ascii=True) for value in fields) + '},\n'
    text += '};\n'
    text += 'struct GuestDataImport { uint32_t address; const char* name; };\n'
    text += 'inline constexpr GuestDataImport kDataImports[] = {\n'
    text += ''.join(f'  {{0x{a:08X}, "{n}"}},\n' for a, n in data_imports) + '};\n'
    (out / "ppc_image_metadata.h").write_text(text)
