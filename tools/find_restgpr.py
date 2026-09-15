with open("Darkness/basefile.exe", "rb") as f:
    data = f.read()

target = bytes.fromhex("7d8803a64e800020")
pos = 0
found = []
while True:
    pos = data.find(target, pos)
    if pos == -1: break
    # check instructions before pos
    # pos - 4: lwz r0, ... or ld r0, ... or lwz r12, ... or ld r12, ...
    # check backwards for sequence of loads
    cnt = 0
    for back in range(1, 25):
        w = int.from_bytes(data[pos - back*4:pos - back*4+4], "big")
        op = w >> 26
        # lwz=32, ld=58
        if op in [32, 58]:
            cnt += 1
        else:
            break
    if cnt >= 15:
        addr = 0x82000000 + pos - cnt * 4
        print(f"Found restgpr candidate with {cnt} loads at: {hex(addr)}")
    pos += 8
