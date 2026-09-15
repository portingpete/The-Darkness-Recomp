with open("Darkness/basefile.exe", "rb") as f:
    data = f.read()

# Inspect code near 0x8289A190
off_start = 0x82899F00 - 0x82000000
off_end   = 0x8289A800 - 0x82000000

# Look for stw r31 / lwz r31
# stw is 36 (0x90000000 | (31 << 21) = 0x93E00000)
# lwz is 32 (0x80000000 | (31 << 21) = 0x83E00000)

for off in range(off_start, off_end, 4):
    w1 = int.from_bytes(data[off:off+4], "big")
    if (w1 & 0xFFE00000) == 0x93E00000: # stw r31
        # check if preceded by stw r14..r30
        w_start = int.from_bytes(data[off - 17*4:off - 17*4+4], "big")
        if (w_start & 0xFFE00000) == (0x90000000 | (14 << 21)):
            print("Candidate savegprlr_14:", hex(0x82000000 + off - 17*4))

    if (w1 & 0xFFE00000) == 0x83E00000: # lwz r31
        w_start = int.from_bytes(data[off - 17*4:off - 17*4+4], "big")
        if (w_start & 0xFFE00000) == (0x80000000 | (14 << 21)):
            print("Candidate restgprlr_14:", hex(0x82000000 + off - 17*4))

    # Look for vmx: stvx (opcode 31, ext 231) or lvx (opcode 31, ext 103)
    # v31 is (31 << 21)
    if (w1 & 0xFC0007FE) == ((31 << 26) | (231 << 1)): # stvx
        reg = (w1 >> 21) & 0x1F
        if reg == 31:
            print("Candidate savevmx near:", hex(0x82000000 + off))
    if (w1 & 0xFC0007FE) == ((31 << 26) | (103 << 1)): # lvx
        reg = (w1 >> 21) & 0x1F
        if reg == 31:
            print("Candidate restvmx near:", hex(0x82000000 + off))
