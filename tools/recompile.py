"""Reproducible native AOT generation. Incomplete output is diagnostic-only."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tomllib
from native_imports import generate as generate_imports

ROOT = Path(__file__).resolve().parents[1]
XENON = ROOT / "refs/UnleashedRecomp/tools/XenonRecomp"


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def sources() -> list[Path]:
    return [ROOT / "config/darkness.toml", ROOT / "runtime/guest/ppc_context.template.h",
            ROOT / "Darkness/_uncrypted.xex", ROOT / "Darkness/darkness_switch_tables.toml",
            ROOT / "Darkness/basefile.exe", ROOT / "tools/native_imports.py",
            *sorted((ROOT / "runtime/native").glob("*.cpp")), Path(__file__).resolve(), *sorted((XENON / "XenonRecomp").glob("*.cpp")),
            *sorted((XENON / "XenonRecomp").glob("*.h")),
            *sorted((XENON / "XenonAnalyse").glob("*.cpp")),
            *sorted((XENON / "XenonAnalyse").glob("*.h")),
            XENON / "XenonUtils/xbox/xboxkrnl_table.inc",
            XENON / "XenonUtils/xbox/xam_table.inc"]


def verify(out: Path, allow_incomplete: bool) -> int:
    manifest = json.loads((out / "manifest.json").read_text())
    if set(manifest['inputs']) != {p.relative_to(ROOT).as_posix() for p in sources()}:
        raise RuntimeError('AOT source inventory changed. Regenerate before building.')
    for group in ("inputs", "outputs"):
        for name, expected in manifest[group].items():
            path = (ROOT if group == "inputs" else out) / name
            if not path.is_file() or sha256(path) != expected:
                raise RuntimeError(f"Changed {group}: {path}. Run tools/build.ps1 to regenerate.")
    if manifest["semantic_diagnostics"] and not allow_incomplete:
        raise RuntimeError(f"{manifest['semantic_diagnostics']} unresolved semantic diagnostics. "
                           "Only an explicitly requested diagnostic build may compile this output.")
    print(f"AOT manifest verified: {len(manifest['outputs'])} files; "
          f"{manifest['semantic_diagnostics']} semantic diagnostics.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generator", type=Path)
    parser.add_argument("--allow-incomplete", action="store_true")
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    if args.verify:
        return verify(out, args.allow_incomplete)
    if not args.generator:
        parser.error("--generator is required for regeneration")
    out.mkdir(parents=True, exist_ok=True)
    # The generator rewrites every file. Preserve timestamps of byte-identical
    # outputs so a native observer edit need not recompile unchanged AOT chunks.
    previous = {p.name: (p.read_bytes(), p.stat().st_atime_ns, p.stat().st_mtime_ns)
                for p in out.glob('ppc_*') if p.is_file()}
    config_path = ROOT / "config/darkness.toml"
    config_source = tomllib.loads(config_path.read_text())
    config = config_source["main"]
    midasm_hooks = config_source.get("midasm_hook", [])
    for key in ("file_path", "switch_table_file_path"):
        config[key] = os.path.relpath((config_path.parent / config[key]).resolve(), out).replace("\\", "/")
    config["out_directory_path"] = "."
    config_text = "[main]\n" + "\n".join(
        f"{key} = {json.dumps(value)}" for key, value in config.items()) + "\n"
    for hook in midasm_hooks:
        config_text += "\n[[midasm_hook]]\n"
        config_text += "\n".join(f"{key} = {json.dumps(value)}" for key, value in hook.items()) + "\n"
    run_config = out / "generation.toml"
    run_config.write_text(config_text)
    executable = args.generator.resolve()
    log_path = out / "generation.log"
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run([str(executable), str(run_config),
                                 str(ROOT / "runtime/guest/ppc_context.template.h")],
                                stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    log = log_path.read_text(encoding="utf-8")
    matches = re.findall(r"Semantic diagnostics: (\d+)", log)
    if result.returncode not in (0, 1):
        raise RuntimeError(f"Generator failed ({result.returncode}); see {log_path}")
    if not matches:
        raise RuntimeError("Generator did not report semantic diagnostics. "
                           "Run tools/build.ps1 to install and build the patched XenonRecomp; "
                           f"see {log_path}")
    diagnostic_count = int(matches[-1])
    if result.returncode != bool(diagnostic_count):
        raise RuntimeError("Generator exit status disagrees with semantic diagnostics")
    # The generator leaves obsolete chunks behind when the function count shrinks.
    unit_counts = re.findall(r"^Translation units: (\d+)\s*$", log, re.MULTILINE)
    if not unit_counts:
        raise RuntimeError(f"Generator did not report its translation unit count; see {log_path}")
    chunks = [f"ppc_recomp.{index}.cpp" for index in range(int(unit_counts[-1]))]
    generate_imports(ROOT, out)
    generated = ([out / name for name in chunks + ["ppc_func_mapping.cpp", "ppc_imports.cpp"]]
                 + sorted(out.glob("ppc_*.h")))
    # Old comment-only errors must never enter either build profile.
    for path in generated:
        if re.search(r"// ERROR(?:\s|:)", path.read_text()):
            raise RuntimeError(f"Silent code-generation error in {path}")
    counts = {"invalid_instructions": log.count("Unable to decode instruction"),
              "unsupported_instructions": log.count("Unrecognized instruction"),
              "switch_errors": log.count("ERROR: Switch case"),
              "unresolved_direct_calls": log.count("ERROR: Unresolved direct call"),
              "record_form_warnings": log.count("RC bit enabled")}
    # The build consumes this list, so verify it along with the selected files.
    # Explicit list avoids stale chunk files after the function count shrinks.
    source_list = out / "sources.cmake"
    source_list.write_text("set(DARK_PPC_SOURCES\n" + "".join(
        f'  "${{DARK_GENERATED_DIR}}/{name}"\n' for name in chunks + ["ppc_func_mapping.cpp", "ppc_imports.cpp"]) + ")\n")
    manifest = {"format": 1, "profile": "diagnostic" if diagnostic_count else "complete-translation",
                "semantic_diagnostics": diagnostic_count, "diagnostic_counts": counts,
                "generator_sha256": sha256(executable),
                "inputs": {p.relative_to(ROOT).as_posix(): sha256(p) for p in sources()},
                "outputs": {p.name: sha256(p) for p in [*generated, source_list]}}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for path in generated:
        old = previous.get(path.name)
        if old and old[0] == path.read_bytes():
            os.utime(path, ns=(old[1], old[2]))
    print(json.dumps(counts, indent=2))
    print(f"Generation log: {log_path}")
    return verify(out, args.allow_incomplete)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"AOT gate: {exc}", file=sys.stderr)
        sys.exit(1)
