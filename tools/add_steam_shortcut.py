import os
import struct
import zlib
import shutil

shortcuts_path = r'C:\Program Files (x86)\Steam\userdata\76505701\config\shortcuts.vdf'
backup_path = shortcuts_path + '.bak'

if not os.path.exists(shortcuts_path):
    print(f'File not found: {shortcuts_path}')
    exit(1)

with open(shortcuts_path, 'rb') as f:
    data = f.read()

# Parse binary VDF
pos = 0
def read_string():
    global pos
    end = data.find(b'\x00', pos)
    if end == -1:
        s = data[pos:].decode('utf-8', errors='replace')
        pos = len(data)
        return s
    s = data[pos:end].decode('utf-8', errors='replace')
    pos = end + 1
    return s

def parse_dict():
    global pos
    d = {}
    while pos < len(data):
        t = data[pos]
        pos += 1
        if t == 8:
            break
        key = read_string()
        if t == 0:
            d[key] = parse_dict()
        elif t == 1:
            d[key] = read_string()
        elif t == 2:
            val = struct.unpack('<I', data[pos:pos+4])[0]
            pos += 4
            d[key] = val
    return d

parsed = parse_dict()
shortcuts = parsed.get('shortcuts', {})

# Check if already present
for idx, entry in shortcuts.items():
    if 'The Darkness' in entry.get('AppName', '') or 'DarkRecomp' in entry.get('AppName', ''):
        print(f"Already exists in Steam shortcuts as '{entry.get('AppName')}'")
        exit(0)

# Create new shortcut entry
exe = r'K:\DarkRecomp\xenia\canary\xenia_canary.exe'
launch_opts = r'"K:\DarkRecomp\Darkness\default.xex"'
start_dir = 'K:\\DarkRecomp\\xenia\\canary\\'
app_name = 'The Darkness (Xenia Canary)'

# Calculate 32-bit AppID
crc_input = f'"{exe}"{app_name}'.encode('utf-8')
appid = (zlib.crc32(crc_input) | 0x80000000) & 0xFFFFFFFF

next_idx = str(len(shortcuts))
new_entry = {
    'appid': appid,
    'AppName': app_name,
    'Exe': f'"{exe}"',
    'StartDir': start_dir,
    'icon': '',
    'ShortcutPath': '',
    'LaunchOptions': launch_opts,
    'IsHidden': 0,
    'AllowDesktopConfig': 1,
    'AllowOverlay': 1,
    'OpenVR': 0,
    'Devkit': 0,
    'DevkitGameID': '',
    'DevkitOverrideAppID': 0,
    'LastPlayTime': 0,
    'FlatpakAppID': '',
    'sortas': '',
    'tags': {}
}

shortcuts[next_idx] = new_entry
parsed['shortcuts'] = shortcuts

# Backup original
shutil.copy2(shortcuts_path, backup_path)
print(f'Backup created at {backup_path}')

# Serialize binary VDF
def serialize_dict(d):
    out = bytearray()
    for k, v in d.items():
        if isinstance(v, dict):
            out.append(0)
            out.extend(k.encode('utf-8') + b'\x00')
            out.extend(serialize_dict(v))
            out.append(8)
        elif isinstance(v, str):
            out.append(1)
            out.extend(k.encode('utf-8') + b'\x00')
            out.extend(v.encode('utf-8') + b'\x00')
        elif isinstance(v, int):
            out.append(2)
            out.extend(k.encode('utf-8') + b'\x00')
            out.extend(struct.pack('<I', v & 0xFFFFFFFF))
    return out

out_bytes = bytearray()
out_bytes.append(0)
out_bytes.extend(b'shortcuts\x00')
out_bytes.extend(serialize_dict(shortcuts))
out_bytes.append(8)
out_bytes.append(8)

with open(shortcuts_path, 'wb') as f:
    f.write(out_bytes)

print(f"Successfully added '{app_name}' to Steam shortcuts!")
