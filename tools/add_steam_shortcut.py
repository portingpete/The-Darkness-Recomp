"""Add a Windows or Linux Steam shortcut for an installed DarkRecomp release.

Close Steam before writing; it saves shortcuts.vdf on exit. --dry-run prints
the entry without changing Steam files. Existing entries and backups are kept.
On Linux, select a Proton tool in the shortcut's Compatibility properties.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
APP_NAME = "The Darkness (DarkRecomp)"
LAUNCH_OPTIONS = "--sound"


def steam_userdata_roots(platform=None, home=None, environ=None):
    platform = sys.platform if platform is None else platform
    home = Path.home() if home is None else Path(home)
    environ = os.environ if environ is None else environ
    if platform == "win32":
        installations = []
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
                installations.append(Path(winreg.QueryValueEx(key, "SteamPath")[0]))
        except (ImportError, OSError):
            pass
        installations.append(Path(environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Steam")
    else:
        installations = [home / ".local/share/Steam", home / ".steam/steam",
                         home / ".steam/root",
                         home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"]
    return [path / "userdata" for path in installations]


def find_shortcuts(userdata_roots):
    found = {}
    for root in userdata_roots:
        if not root.is_dir():
            continue
        for user in sorted(root.iterdir()):
            if user.name.isdecimal() and (user / "config").is_dir():
                path = user / "config/shortcuts.vdf"
                found.setdefault(path.resolve(), path)
    return list(found.values())


def read_string(data, pos):
    end = data.find(b"\x00", pos)
    if end == -1:
        raise ValueError("Unterminated Steam shortcut string; file was not changed")
    return data[pos:end].decode("utf-8"), end + 1


def parse_dict(data, pos=0):
    result = {}
    while pos < len(data):
        kind = data[pos]
        pos += 1
        if kind == 8:
            return result, pos
        key, pos = read_string(data, pos)
        if key in result:
            raise ValueError("Duplicate Steam shortcut key; file was not changed")
        if kind == 0:
            result[key], pos = parse_dict(data, pos)
        elif kind == 1:
            result[key], pos = read_string(data, pos)
        elif kind == 2:
            if pos + 4 > len(data):
                raise ValueError("Truncated Steam shortcut integer; file was not changed")
            result[key] = struct.unpack_from("<I", data, pos)[0]
            pos += 4
        else:
            raise ValueError(f"Unsupported Steam shortcut field type {kind}; file was not changed")
    raise ValueError("Unterminated Steam shortcut dictionary; file was not changed")


def serialize_dict(values):
    out = bytearray()
    for key, value in values.items():
        if isinstance(value, dict):
            kind, encoded = 0, serialize_dict(value) + b"\x08"
        elif isinstance(value, str):
            kind, encoded = 1, value.encode("utf-8") + b"\x00"
        elif isinstance(value, int):
            kind, encoded = 2, struct.pack("<I", value & 0xFFFFFFFF)
        else:
            raise ValueError(f"Unsupported Steam shortcut value for {key}")
        out.append(kind)
        out.extend(key.encode("utf-8") + b"\x00")
        out.extend(encoded)
    return out


def shortcut_entry(root):
    root = Path(root).resolve()
    executable = root / "build_native/Release/DarkRecompPreview.exe"
    if not executable.is_file():
        raise FileNotFoundError(f"Game launcher not found: {executable}; extract the entire Windows release first")
    target = f'"{executable}"'
    appid = (zlib.crc32((target + APP_NAME).encode("utf-8")) | 0x80000000) & 0xFFFFFFFF
    return {
        "appid": appid,
        "AppName": APP_NAME,
        "Exe": target,
        "StartDir": f'"{root}"',
        "icon": "",
        "ShortcutPath": "",
        "LaunchOptions": LAUNCH_OPTIONS,
        "IsHidden": 0,
        "AllowDesktopConfig": 1,
        "AllowOverlay": 1,
        "OpenVR": 0,
        "Devkit": 0,
        "DevkitGameID": "",
        "DevkitOverrideAppID": 0,
        "LastPlayTime": 0,
        "FlatpakAppID": "",
        "sortas": "",
        "tags": {},
    }


def add_to(shortcuts_path, entry, dry_run=False):
    shortcuts_path = Path(shortcuts_path)
    if shortcuts_path.is_file():
        data = shortcuts_path.read_bytes()
        parsed, end = parse_dict(data)
        if end != len(data):
            raise ValueError("Unexpected trailing Steam shortcut data; file was not changed")
    else:
        parsed = {"shortcuts": {}}
    shortcuts = parsed.get("shortcuts")
    if not isinstance(shortcuts, dict) or any(not isinstance(value, dict) for value in shortcuts.values()):
        raise ValueError("Invalid Steam shortcuts root; file was not changed")
    for existing in shortcuts.values():
        if existing.get("AppName") == APP_NAME or existing.get("Exe") == entry["Exe"]:
            print(f"Already present in {shortcuts_path} as '{existing.get('AppName')}'")
            return False
    if dry_run:
        print(json.dumps({"shortcuts_file": str(shortcuts_path), "entry": entry}, indent=2))
        return False
    next_index = str(max((int(index) for index in shortcuts if index.isdecimal()), default=-1) + 1)
    shortcuts[next_index] = entry
    backup = shortcuts_path.with_name(shortcuts_path.name + ".bak")
    if shortcuts_path.is_file() and not backup.exists():
        shutil.copy2(shortcuts_path, backup)
    out = serialize_dict(parsed) + b"\x08"
    with tempfile.NamedTemporaryFile(dir=shortcuts_path.parent, delete=False) as temporary:
        temporary.write(out)
        temporary_path = Path(temporary.name)
    try:
        os.replace(temporary_path, shortcuts_path)
    finally:
        temporary_path.unlink(missing_ok=True)
    print(f"Added '{APP_NAME}' (appid {entry['appid']}) to {shortcuts_path}")
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shortcuts_file", nargs="?", type=Path, help="One Steam user's config/shortcuts.vdf")
    parser.add_argument("--root", type=Path, default=ROOT, help="Extracted release folder containing Darkness and build_native")
    parser.add_argument("--steam-userdata", type=Path, help="Use this Steam userdata directory instead of autodetection")
    parser.add_argument("--dry-run", action="store_true", help="Print the shortcut without writing Steam files")
    args = parser.parse_args(argv)
    try:
        entry = shortcut_entry(args.root)
        roots = [args.steam_userdata] if args.steam_userdata else steam_userdata_roots()
        targets = [args.shortcuts_file] if args.shortcuts_file else find_shortcuts(roots)
        if not targets:
            parser.error("No Steam user found; open Steam once or pass config/shortcuts.vdf explicitly")
        if len(targets) > 1:
            parser.error("Multiple Steam users found; pass one config/shortcuts.vdf explicitly:\n" +
                         "\n".join(map(str, targets)))
        if not targets[0].parent.is_dir():
            parser.error(f"Steam config directory does not exist: {targets[0].parent}")
        if add_to(targets[0], entry, args.dry_run):
            print("Restart Steam to see the new shortcut.")
        if sys.platform != "win32":
            print("In Steam, open Properties > Compatibility and select a Proton tool. Steam Deck gameplay is unverified.")
        return 0
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
