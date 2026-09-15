"""Add The Darkness (DarkRecomp) to the Steam library as a non-Steam game.

Edits the local shortcuts.vdf for the detected Steam user. Steam must be
CLOSED first or it will overwrite this file on exit. A .bak backup is made
before writing. Safe to re-run: exits quietly if the entry already exists.
"""
import os
import struct
import sys
import zlib
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREVIEW_EXE = os.path.join(ROOT, "build_native", "Release", "DarkRecompPreview.exe")
APP_NAME = "The Darkness (DarkRecomp)"
LAUNCH_OPTIONS = "--sound"

STEAM_USERDATA = r"C:\Program Files (x86)\Steam\userdata"


def find_shortcuts():
    if len(sys.argv) > 1:
        return [sys.argv[1]]
    if not os.path.isdir(STEAM_USERDATA):
        return []
    found = []
    for user in sorted(os.listdir(STEAM_USERDATA)):
        path = os.path.join(STEAM_USERDATA, user, "config", "shortcuts.vdf")
        if os.path.isfile(path):
            found.append(path)
    return found


def read_string(data, pos):
    end = data.find(b"\x00", pos)
    if end == -1:
        return data[pos:].decode("utf-8", errors="replace"), len(data)
    return data[pos:end].decode("utf-8", errors="replace"), end + 1


def parse_dict(data, pos):
    d = {}
    while pos < len(data):
        t = data[pos]
        pos += 1
        if t == 8:
            break
        key, pos = read_string(data, pos)
        if t == 0:
            d[key], pos = parse_dict(data, pos)
        elif t == 1:
            d[key], pos = read_string(data, pos)
        elif t == 2:
            d[key] = struct.unpack("<I", data[pos:pos + 4])[0]
            pos += 4
    return d, pos


def serialize_dict(d):
    out = bytearray()
    for k, v in d.items():
        if isinstance(v, dict):
            out.append(0)
            out.extend(k.encode("utf-8") + b"\x00")
            out.extend(serialize_dict(v))
            out.append(8)
        elif isinstance(v, str):
            out.append(1)
            out.extend(k.encode("utf-8") + b"\x00")
            out.extend(v.encode("utf-8") + b"\x00")
        elif isinstance(v, int):
            out.append(2)
            out.extend(k.encode("utf-8") + b"\x00")
            out.extend(struct.pack("<I", v & 0xFFFFFFFF))
    return out


def add_to(shortcuts_path):
    with open(shortcuts_path, "rb") as f:
        data = f.read()
    parsed, _ = parse_dict(data, 0)
    shortcuts = parsed.get("shortcuts", {})

    for idx, entry in shortcuts.items():
        if "DarkRecomp" in entry.get("AppName", ""):
            print(f"Already present in {shortcuts_path} as '{entry.get('AppName')}'")
            return False

    appid = (zlib.crc32(f'"{PREVIEW_EXE}"{APP_NAME}'.encode("utf-8")) | 0x80000000) & 0xFFFFFFFF
    next_idx = str(len(shortcuts))
    shortcuts[next_idx] = {
        "appid": appid,
        "AppName": APP_NAME,
        "Exe": f'"{PREVIEW_EXE}"',
        "StartDir": ROOT + "\\",
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
    parsed["shortcuts"] = shortcuts

    shutil.copy2(shortcuts_path, shortcuts_path + ".bak")
    out = bytearray(b"\x00shortcuts\x00")
    out.extend(serialize_dict(shortcuts))
    out.extend(b"\x08\x08")
    with open(shortcuts_path, "wb") as f:
        f.write(out)
    print(f"Added '{APP_NAME}' (appid {appid}) to {shortcuts_path}")
    return True


def main():
    if not os.path.isfile(PREVIEW_EXE):
        print(f"Built game not found: {PREVIEW_EXE}")
        print("Run tools\\build.ps1 first.")
        return 1
    targets = find_shortcuts()
    if not targets:
        print(f"No shortcuts.vdf found under {STEAM_USERDATA}")
        print("Pass the path explicitly: python tools\\add_steam_shortcut.py <path>")
        return 1
    changed = False
    for target in targets:
        changed |= add_to(target)
    if changed:
        print("Restart Steam to see the new shortcut.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
