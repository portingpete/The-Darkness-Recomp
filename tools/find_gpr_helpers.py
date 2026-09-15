with open("Darkness/basefile.exe", "rb") as f:
    data = f.read()

# restgprlr ends with:
# lwz r31, ...
# lwz r0, ...
# mtlr r0 (0x7C0803A6)
# blr (0x4E800020)
target_tail = bytes.fromhex("7c0803a64e800020")
idx = 0
while True:
    idx = data.find(target_tail, idx)
    if idx == -1: break
    # check instructions before idx
    # idx - 8: lwz r0, ...
    # idx - 12: lwz r31, ...
    w_r31 = int.from_bytes(data[idx-8:idx-4], "big")
    w_r0  = int.from_bytes(data[idx-4:idx], "big")
    if (w_r31 & 0xFFE00000) == 0x83E00000: # lwz r31
        # check backwards 17 instructions for lwz r14
        start = idx - 8 - 17 * 4
        w_r14 = int.from_bytes(data[start:start+4], "big")
        if (w_r14 & 0xFFE00000) == 0x81C00000: # lwz r14
            print("Found restgprlr_14 at:", hex(0x82000000 + start))
    idx += 8

# savegprlr ends with:
# stw r31, ...
# stw r0, ...
# blr (0x4E800020)
target_save = bytes.fromhex("4e800020")
idx = 0
while True:
    idx = data.find(target_save, idx)
    if idx == -1: break
    w_r0  = int.from_bytes(data[idx-4:idx], "big")
    w_r31 = int.from_bytes(data[idx-8:idx-4], "big")
    if (w_r0 & 0xFFE00000) == 0x90000000 and (w_r31 & 0xFFE00000) == 0x93E00000:
        start = idx - 8 - 17 * 4
        w_r14 = int.from_bytes(data[start:start+4], "big")
        if (w_r14 & 0xFFE00000) == 0x91C00000:
            print("Found savegprlr_14 at:", hex(0x82000000 + start))
    idx += 4
