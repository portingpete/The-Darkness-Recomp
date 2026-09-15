#!/usr/bin/env python3
"""Build a regen-safe function atlas over the generated AOT code.

Reads (never writes) build_native/generated/ppc_*.cpp plus the game image,
and emits tools/code_atlas.json: per-function size, callers/callees, imports,
referenced distinctive strings, class-registry links and hand-written notes.

Generated files are rewritten by tools/recompile.py, so no annotation may live
in them; tools/atlas_notes.json is the persistent human layer. Re-run this
script after regeneration or note edits:
    python tools/build_atlas.py [--output tools/code_atlas.json]

Tiers (see stats): T1 named (registry/seed), T2 direct evidence (distinctive
string or categorized import), T2p propagated (strict call-graph majority,
explicit confidence), T3 mapped-only.
"""
from __future__ import annotations

import bisect
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "build_native" / "generated"
IMG = ROOT / "Darkness" / "basefile.exe"
IMG_BASE = 0x82000000
REGISTRY = ROOT / "tools" / "engine_classes.json"
SEEDS = ROOT / "tools" / "atlas_notes.json"
DEFAULT_OUTPUT = ROOT / "tools" / "code_atlas.json"

# Guest image extent for resolving referenced addresses (loader maps the
# xextool dump 1:1 at 0x82000000; code lives below ~0x82B10000).
CODE_MIN, CODE_MAX = 0x82000000, 0x82B40000

IMPORT_CATEGORIES = (
    ("file-io", ("NtCreateFile", "NtOpenFile", "NtReadFile", "NtWriteFile", "NtQueryDirectoryFile",
                 "NtQueryInformationFile", "NtSetInformationFile", "NtFlushBuffersFile",
                 "NtQueryFullAttributesFile", "NtClose", "Fsc")),
    ("threading", ("ExCreateThread", "ExTerminateThread", "KeTls", "NtWaitForSingleObject",
                   "NtReleaseSemaphore", "KeSetEvent", "NtCreateEvent", "NtCreateSemaphore",
                   "RtlEnterCriticalSection", "RtlLeaveCriticalSection", "RtlInitializeCriticalSection",
                   "ObReferenceObject", "ObDereferenceObject", "KeSetAffinityThread")),
    ("memory", ("MmQueryStatistics", "MmAllocatePhysicalMemory", "MmFreePhysicalMemory",
                "VirtualQuery", "VirtualProtect", "VirtualAlloc")),
    ("audio", ("XAudio", "XamAudio")),
    ("timing", ("KeQueryPerformanceFrequency", "KeQuerySystemTime", "NtDelayExecution")),
    ("xam", ("Xam", "XexCheckExecutablePrivilege", "XGet", "XMsg")),
    ("rtl", ("RtlInit", "RtlAnsi", "RtlUnicode", "RtlCopy", "RtlZero", "RtlEqual")),
    ("debug", ("DbgPrint", "DbgBreakPoint")),
)


def classify_string(s: bytes) -> str | None:
    if re.search(rb"[A-Za-z]:\\\\|System\\\\|Content\\\\|/[^ ]*\.|\\[^ ]*\.|\.(xrg|XDF|xdf)\b", s):
        return "path"
    if re.match(rb"(XR|MRender|WClient|VBOp|XREngine|GUIFade)", s):
        return "shader"
    if b"%" in s and re.search(rb"%[dsxXc%]", s):
        return "format"
    if re.match(rb"[CMWX][A-Z][A-Za-z0-9_]{3,}$", s):
        return "class"
    if len(s) >= 16 and b" " in s:
        return "text"
    return None


def main() -> int:
    output = Path(sys.argv[sys.argv.index("--output") + 1]) if "--output" in sys.argv else DEFAULT_OUTPUT

    data = IMG.read_bytes()
    img_end = IMG_BASE + len(data)
    str_by_addr: dict[int, tuple[str, str]] = {}
    for match in re.finditer(rb"[\x20-\x7e]{5,}", data):
        kind = classify_string(match.group())
        if kind:
            str_by_addr.setdefault(IMG_BASE + match.start(), (match.group().decode("ascii", "replace"), kind))
    print(f"strings: {len(str_by_addr)} distinctive addresses", flush=True)

    registry = {int(e["factory_address"], 16): e["name"]
                for e in json.loads(REGISTRY.read_text())}
    seeds = json.loads(SEEDS.read_text())

    mark_re = re.compile(rb"PPC_FUNC_IMPL\(([^)]+)\)")
    addr_re = re.compile(rb"0x82[0-9A-Fa-f]{6}\b")
    # Guest calls appear as sub_X(ctx,...) within a translation unit and as
    # __imp__sub_X(ctx,...) across units; both are direct static edges.
    call_re = re.compile(rb"(?:__imp__)?sub_([0-9A-Fa-f]+)\(")
    # Real kernel imports start uppercase; __imp__sub_X are game functions and
    # __imp____savegprlr_N are codegen helpers, never imports.
    imp_re = re.compile(rb"__imp__([A-Z][A-Za-z0-9_]*)")
    # NOTE: every recompiled game function is named __imp__sub_<addr> by the
    # generator; that prefix carries no import meaning. Bodies start after the
    # marker line so a function never references itself.

    funcs: dict[int, dict] = {}
    for path in sorted(GEN.glob("ppc_*.cpp")):
        if path.name in ("ppc_func_mapping.cpp", "ppc_imports.cpp"):
            continue
        text = path.read_bytes()
        marks = [(m.start(), m.end(), m.group(1).decode()) for m in mark_re.finditer(text)]
        starts = [m.start() for m in mark_re.finditer(text)]
        line_starts = [0] + [m.start() + 1 for m in re.finditer(rb"\n", text)]

        def line_of(offset: int) -> int:
            return bisect.bisect_right(line_starts, offset)
        for idx, (start, mend, name) in enumerate(marks):
            end = starts[idx + 1] if idx + 1 < len(starts) else len(text)
            m = re.match(r"(?:__imp__)?sub_([0-9A-Fa-f]+)$", name)
            if not m:
                continue
            addr = int(m.group(1), 16)
            body = text[mend:end]
            callees = sorted({int(x, 16) for x in
                              {c.decode() for c in call_re.findall(body)}})
            callees = [c for c in callees if CODE_MIN <= c <= CODE_MAX and c != addr][:48]
            imports = sorted({i.decode() for i in imp_re.findall(body)})[:24]
            seen: dict[str, list] = {}
            for raw in addr_re.findall(body):
                ref = int(raw.decode(), 16)
                if ref == addr or not (IMG_BASE <= ref < img_end):
                    continue
                hit = str_by_addr.get(ref)
                if hit and hit[0] not in seen:
                    seen[hit[0]] = [hit[1], ref]
            strings = sorted(seen.items(), key=lambda kv: ({"path": 0, "shader": 1, "format": 2,
                                                             "class": 3, "text": 4}[kv[1][0]], kv[0]))[:10]
            funcs[addr] = {
                "file": path.name,
                "lines": line_of(end - 1) - line_of(mend) + 1,
                "callees": callees,
                "imports": imports,
                "strings": [{"value": v, "kind": k, "addr": f"0x{a:08X}"} for v, (k, a) in strings],
            }
    print(f"functions: {len(funcs)}", flush=True)

    callers: dict[int, list[int]] = {}
    for addr, info in funcs.items():
        for callee in info["callees"]:
            callers.setdefault(callee, []).append(addr)
    # Functions that construct a registered engine class inherit its name.
    spawners: dict[int, list[str]] = {}
    for factory, cls in registry.items():
        for caller in callers.get(factory, []):
            spawners.setdefault(caller, []).append(cls)
    # Strict label propagation over callee edges only ("what it does").
    # Caller-direction ("who uses it") was tried and removed: the dense boot
    # web outvotes everything, so shared utilities get mislabeled by sheer
    # caller count. A function takes its callees' majority tag when at least 2
    # agree with >=2/3 share. Sources are certain seed tags plus
    # import-category tags; weak observation tags never propagate. Propagated
    # tags carry their own confidence and tier so they cannot be mistaken for
    # direct evidence.
    WEAK_SOURCE = {"hook-observed", "stack-observed", "engine-class-factory", "profile-gated"}
    MIN_VOTES, MIN_SHARE, ROUNDS = 2, 0.67, 3

    def import_tags(imports: list[str]) -> list[str]:
        tags = []
        for tag, needles in IMPORT_CATEGORIES:
            if any(any(n in imp for n in needles) for imp in imports):
                tags.append(tag)
        return tags

    label: dict[int, set[str]] = {}
    for addr, info in funcs.items():
        seed = seeds.get(f"0x{addr:08X}")
        src = set()
        if seed:
            src |= set(seed.get("tags", []))
        src |= set(import_tags(info["imports"]))
        src -= WEAK_SOURCE
        if src:
            label[addr] = src
    propagated: dict[int, tuple[str, str]] = {}
    for _ in range(ROUNDS):
        changed = False
        for addr in sorted(funcs):
            if addr in label or addr in propagated:
                continue
            callee_votes: dict[str, int] = {}
            for callee in funcs[addr]["callees"]:
                for tag in label.get(callee, ()):
                    callee_votes[tag] = callee_votes.get(tag, 0) + 1
            total = sum(callee_votes.values())
            direction, votes = "propagated-callees", callee_votes
            if total < MIN_VOTES:
                continue
            if total >= MIN_VOTES:
                best = max(sorted(votes), key=lambda t: votes[t])
                if votes[best] / total >= MIN_SHARE:
                    propagated[addr] = (best, direction)
                    label[addr] = {best}
                    changed = True
        if not changed:
            break

    tier_counts = {"t1": 0, "t2": 0, "t2p": 0, "t3": 0}
    seeds_matched = 0
    out: dict[str, dict] = {}
    for addr in sorted(funcs):
        info = funcs[addr]
        key = f"0x{addr:08X}"
        entry: dict = {"file": info["file"], "lines": info["lines"]}
        seed = seeds.get(key)
        cls = registry.get(addr)
        if seed:
            seeds_matched += 1
            entry["name"] = seed.get("name", key)
            entry["note"] = seed.get("summary")
            entry["tags"] = seed.get("tags", [])
            entry["confidence"] = seed.get("confidence")
        elif cls:
            entry["name"] = cls
            entry["tags"] = ["engine-class-factory"]
        for spawned in spawners.get(addr, [])[:8]:
            entry.setdefault("tags", []).append(f"spawns-{spawned}")
        if info["callees"]:
            entry["callees"] = [f"0x{c:08X}" for c in info["callees"]]
        if info["imports"]:
            entry["imports"] = info["imports"]
            tags = import_tags(info["imports"])
            if tags:
                entry.setdefault("tags", []).extend(t for t in tags if t not in entry.get("tags", []))
        if info["strings"]:
            entry["strings"] = info["strings"]
        call_list = callers.get(addr, [])
        if call_list:
            entry["caller_count"] = len(call_list)
            entry["callers"] = [f"0x{c:08X}" for c in sorted(call_list)[:24]]
        has_name = "name" in entry
        direct = bool(info["strings"]) or bool(entry.get("tags"))
        prop = propagated.get(addr)
        if prop and not has_name and not direct:
            entry.setdefault("tags", []).append(prop[0])
            entry["confidence"] = prop[1]
            tier = "t2p"
        else:
            tier = "t1" if has_name else ("t2" if direct else "t3")
        entry["tier"] = tier
        tier_counts[tier] += 1
        out[key] = entry

    identified = tier_counts["t1"] + tier_counts["t2"] + tier_counts["t2p"]
    stats = {
        "functions": len(out),
        "tier1_named": tier_counts["t1"],
        "tier2_direct": tier_counts["t2"],
        "tier2_propagated": tier_counts["t2p"],
        "tier3_mapped_only": tier_counts["t3"],
        "identified_share": round(identified / len(out), 4) if out else 0,
        "registry_classes": len(registry),
        "seed_notes": len(seeds),
        "seeds_matched": seeds_matched,
        "distinctive_strings": len(str_by_addr),
    }
    output.write_text(json.dumps({"meta": {"generator": "tools/build_atlas.py"}, "stats": stats,
                                  "functions": out}, indent=1) + "\n")
    print(json.dumps(stats, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
