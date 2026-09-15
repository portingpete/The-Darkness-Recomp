"""Extract import identities from this XEX; generate strong fail-fast defaults."""
import hashlib
from pathlib import Path
import re
import struct


def generate(root: Path, out: Path) -> None:
    header = (out / "ppc_recomp_shared.h").read_text()
    names = set(re.findall(r"PPC_EXTERN_FUNC\((__imp__\w+)\)", header))
    native_sources = "\n".join(p.read_text() for p in sorted((root / "runtime/native").glob("*.cpp")))
    implemented = set(re.findall(r"PPC_FUNC\((__imp__\w+)\)", native_sources))
    missing = sorted(names - implemented)
    (out / "ppc_imports.cpp").write_text('#include "ppc_context.h"\n' + "".join(
        f'PPC_FUNC({name}) {{ PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unimplemented import {name}"); }}\n'
        for name in missing))
    image = (root / "Darkness/basefile.exe").read_bytes()
    xex = (root / "Darkness/_uncrypted.xex").read_bytes()
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
    text = '#pragma once\n#include <cstdint>\n'
    text += f'inline constexpr char kGameImageSha256[] = "{hashlib.sha256(image).hexdigest()}";\n'
    text += f'inline constexpr char kGameXexSha256[] = "{hashlib.sha256(xex).hexdigest()}";\n'
    text += 'struct GuestDataImport { uint32_t address; const char* name; };\n'
    text += 'inline constexpr GuestDataImport kDataImports[] = {\n'
    text += ''.join(f'  {{0x{a:08X}, "{n}"}},\n' for a, n in data_imports) + '};\n'
    (out / "ppc_image_metadata.h").write_text(text)
