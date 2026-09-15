import os
import sys
import json

def get_str(data, addr):
    off = addr - 0x82000000
    if 0 <= off < len(data):
        end = data.find(b'\0', off)
        if 0 < end - off < 120:
            try:
                s = data[off:end].decode('ascii')
                if s.isprintable() and len(s) > 1:
                    return s
            except:
                pass
    return None

def main():
    basefile = sys.argv[1] if len(sys.argv) > 1 else 'Darkness/basefile.exe'
    if not os.path.exists(basefile):
        print(f"Error: {basefile} not found")
        return 1

    with open(basefile, 'rb') as f:
        data = f.read()

    start_off = 0x82A20000 - 0x82000000
    end_off = 0x82AC2FA0 - 0x82000000

    classes = []
    for off in range(start_off, end_off - 16, 4):
        name_ptr = int.from_bytes(data[off:off+4], 'big')
        func_ptr = int.from_bytes(data[off+4:off+8], 'big')
        cat_ptr  = int.from_bytes(data[off+8:off+12], 'big')
        if 0x82000000 <= name_ptr <= 0x820A0000 and 0x820C0000 <= func_ptr <= 0x829C0000 and 0x82A20000 <= cat_ptr <= 0x82AC0000:
            name = get_str(data, name_ptr)
            if name and (name.startswith('C') or name.startswith('M') or name.startswith('W') or name.startswith('X')):
                reg_addr = 0x82000000 + off
                classes.append({
                    'name': name,
                    'registry_address': f"0x{reg_addr:08X}",
                    'factory_address': f"0x{func_ptr:08X}",
                    'category_address': f"0x{cat_ptr:08X}"
                })

    out_file = 'tools/engine_classes.json'
    with open(out_file, 'w') as f:
        json.dump(classes, f, indent=2)

    print(f"Successfully extracted {len(classes)} registered engine classes to {out_file}!")
    return 0

if __name__ == '__main__':
    sys.exit(main())
