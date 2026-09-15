with open("Darkness/basefile.exe", "rb") as f:
    data = f.read()

# 1. Look for savegprlr ending with blr (0x4e800020)
# stw rX is opcode 36: (36 << 26) | (reg << 21) | (12 << 16) or (11 << 16) or (1 << 16)
# stfd fX is opcode 54: (54 << 26) | (reg << 21)
# lfd fX is opcode 50: (50 << 26) | (reg << 21)

import struct

def find_fpr_helpers():
    # stfd f30, stfd f31, blr
    # stfd is 54 (0xD8000000)
    # f30: (54 << 26) | (30 << 21) = 0xDBC00000
    # f31: (54 << 26) | (31 << 21) = 0xDBE00000
    for off in range(0, len(data) - 16, 4):
        w1 = int.from_bytes(data[off:off+4], "big")
        if (w1 & 0xFFE00000) == 0xDBC00000: # stfd f30
            w2 = int.from_bytes(data[off+4:off+8], "big")
            w3 = int.from_bytes(data[off+8:off+12], "big")
            if (w2 & 0xFFE00000) == 0xDBE00000 and w3 == 0x4E800020: # stfd f31, blr
                # Check backwards 16 instructions (16*4 = 64 bytes) for stfd f14 (f14=14)
                start_off = off - 16 * 4
                w_start = int.from_bytes(data[start_off:start_off+4], "big")
                if (w_start & 0xFFE00000) == ((54 << 26) | (14 << 21)):
                    print("Found savefpr_14_address at:", hex(0x82000000 + start_off))

    # lfd f30, lfd f31, blr
    # lfd is 50 (0xC8000000)
    for off in range(0, len(data) - 16, 4):
        w1 = int.from_bytes(data[off:off+4], "big")
        if (w1 & 0xFFE00000) == ((50 << 26) | (30 << 21)): # lfd f30
            w2 = int.from_bytes(data[off+4:off+8], "big")
            w3 = int.from_bytes(data[off+8:off+12], "big")
            if (w2 & 0xFFE00000) == ((50 << 26) | (31 << 21)) and w3 == 0x4E800020: # lfd f31, blr
                start_off = off - 16 * 4
                w_start = int.from_bytes(data[start_off:start_off+4], "big")
                if (w_start & 0xFFE00000) == ((50 << 26) | (14 << 21)):
                    print("Found restfpr_14_address at:", hex(0x82000000 + start_off))

find_fpr_helpers()
