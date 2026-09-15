#!/usr/bin/env python3
"""Query tools/code_atlas.json (see tools/build_atlas.py).

    python tools/atlas.py stats
    python tools/atlas.py describe 0x821F1548
    python tools/atlas.py search CubeMenu
    python tools/atlas.py top --by callers|lines|strings --tier t1 --limit 20
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ATLAS = Path(__file__).resolve().parent / "code_atlas.json"


def load() -> dict:
    return json.loads(ATLAS.read_text())


def norm(addr: str) -> str:
    addr = addr.strip().lower()
    if addr.startswith("sub_"):
        addr = addr[4:]
    if not addr.startswith("0x"):
        addr = "0x" + addr
    return "0x" + addr[2:].upper().zfill(8)


def show(key: str, entry: dict) -> None:
    print(f"{key}  [{entry.get('tier', '?')}] {entry.get('name', '(anonymous)')}")
    if entry.get("note"):
        print(f"  note: {entry['note']} [{entry.get('confidence', '?')}]")
    print(f"  file: {entry['file']}:{entry.get('lines', '?')} lines")
    if entry.get("tags"):
        print(f"  tags: {', '.join(entry['tags'])}")
    if entry.get("imports"):
        print(f"  imports: {', '.join(entry['imports'][:12])}")
    if entry.get("strings"):
        for s in entry["strings"][:8]:
            print(f"  str[{s['kind']}] {s['addr']}: {s['value'][:100]}")
    if entry.get("callees"):
        print(f"  callees: {len(entry['callees'])} e.g. {', '.join(entry['callees'][:8])}")
    if entry.get("caller_count"):
        print(f"  callers: {entry['caller_count']} e.g. {', '.join(entry.get('callers', [])[:8])}")


def main() -> int:
    if not ATLAS.is_file():
        print("Run python tools/build_atlas.py first.", file=sys.stderr)
        return 2
    atlas = load()
    args = sys.argv[1:]
    if not args or args[0] == "stats":
        print(json.dumps(atlas["stats"], indent=2))
        return 0
    if args[0] == "describe" and len(args) == 2:
        key = norm(args[1])
        entry = atlas["functions"].get(key)
        if not entry:
            print(f"unknown function {key}", file=sys.stderr)
            return 1
        show(key, entry)
        return 0
    if args[0] == "search" and len(args) == 2:
        needle = args[1].lower()
        hits = 0
        for key, entry in atlas["functions"].items():
            hay = [entry.get("name", "")] + [s["value"] for s in entry.get("strings", [])]
            if any(needle in h.lower() for h in hay):
                show(key, entry)
                hits += 1
                if hits >= 25:
                    print("... (truncated at 25 hits)")
                    break
        print(f"{hits} hit(s)")
        return 0
    if args[0] == "top":
        by = args[args.index("--by") + 1] if "--by" in args else "callers"
        tier = args[args.index("--tier") + 1] if "--tier" in args else None
        limit = int(args[args.index("--limit") + 1]) if "--limit" in args else 20
        rows = []
        for key, entry in atlas["functions"].items():
            if tier and entry.get("tier") != tier:
                continue
            if by == "callers":
                score = entry.get("caller_count", 0)
            elif by == "lines":
                score = entry.get("lines", 0)
            elif by == "strings":
                score = len(entry.get("strings", []))
            else:
                print("unknown --by (callers|lines|strings)", file=sys.stderr)
                return 2
            rows.append((score, key, entry))
        for score, key, entry in sorted(rows, reverse=True)[:limit]:
            print(f"{score:6}  {key}  [{entry.get('tier')}] {entry.get('name', '(anon)')}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
