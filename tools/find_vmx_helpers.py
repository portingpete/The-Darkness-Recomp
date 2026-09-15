with open("Darkness/basefile.exe", "rb") as f:
    f.seek(0x82899000 - 0x82000000)
    data = f.read(0x8289D000 - 0x82899000)

base_addr = 0x82899000

# Search for stvx (opcode 31, ext 231) and lvx (opcode 31, ext 103)
# stvx: (31 << 26) | (vD << 21) | (rA << 16) | (rB << 11) | (231 << 1)
# lvx:  (31 << 26) | (vD << 21) | (rA << 16) | (rB << 11) | (103 << 1)

stvx_sites = []
lvx_sites = []

for off in range(0, len(data) - 8, 4):
    w = int.from_bytes(data[off:off+4], "big")
    if (w & 0xFC0007FE) == ((31 << 26) | (231 << 1)):
        reg = (w >> 21) & 0x1F
        stvx_sites.append((base_addr + off, reg))
    if (w & 0xFC0007FE) == ((31 << 26) | (103 << 1)):
        reg = (w >> 21) & 0x1F
        lvx_sites.append((base_addr + off, reg))

print(f"Found {len(stvx_sites)} stvx and {len(lvx_sites)} lvx sites between 0x82899000 and 0x8289D000:")
for addr, reg in stvx_sites:
    print(f"  stvx v{reg} at {hex(addr)}")
for addr, reg in lvx_sites:
    print(f"  lvx v{reg} at {hex(addr)}")
