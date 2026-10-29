# Find USB device/config/interface/endpoint descriptors in the MAC/host images (main ROM + fw segments < 0x500000).
import struct, sys

def segs(root):
    rom = open(root + "/rom/main_rom_2c0000.rom", "rb").read()
    out = [(0x2c0000, rom)]
    elf = open(root + "/mm8108b2-rl.bin", "rb").read()
    phoff, = struct.unpack_from("<I", elf, 0x1c); phnum, = struct.unpack_from("<H", elf, 0x2c)
    for i in range(phnum):
        t, off, va, _, fs, ms, _, _ = struct.unpack_from("<8I", elf, phoff + 32 * i)
        if t == 1 and va < 0x500000 and fs: out.append((va, elf[off:off + fs]))
    return out

def main():
    root = sys.argv[1]
    for base, b in segs(root):
        for i in range(len(b) - 9):
            if b[i] == 0x12 and b[i + 1] == 0x01 and b[i + 3] in (1, 2, 3) and b[i + 7] in (8, 16, 32, 64):
                vid, pid = struct.unpack_from("<HH", b, i + 8)
                print(f"{base + i:#x} DEVICE bcdUSB {b[i+3]:x}.{b[i+2]:02x} class {b[i+4]:#x} mps0 {b[i+7]} vid {vid:#06x} pid {pid:#06x} ncfg {b[i+17]}")
            if b[i] == 0x09 and b[i + 1] in (0x02, 0x07) and 0 < b[i + 4] < 8:
                tl, = struct.unpack_from("<H", b, i + 2)
                if 9 < tl < 512 and i + tl <= len(b):
                    print(f"{base + i:#x} CONFIG total {tl} nif {b[i+4]} attr {b[i+7]:#x} power {2*b[i+8]} mA")
                    j = i + 9
                    while j < i + tl and b[j] >= 2:
                        L, T = b[j], b[j + 1]
                        if T == 4: print(f"    IF #{b[j+2]} alt {b[j+3]} neps {b[j+4]} class {b[j+5]:#x}/{b[j+6]:#x}/{b[j+7]:#x}")
                        elif T == 5:
                            mps, = struct.unpack_from("<H", b, j + 4); at = b[j + 3]
                            kind = ["ctrl", "ISO", "bulk", "intr"][at & 3]
                            print(f"    EP {b[j+2]:#04x} {kind} attr {at:#x} mps {mps & 0x7ff} (x{((mps >> 11) & 3) + 1}) interval {b[j+6]}")
                        else: print(f"    desc type {T:#x} len {L}")
                        j += L
            if b[i] == 0x07 and b[i + 1] == 0x05 and (b[i + 2] & 0x70) == 0 and (b[i + 2] & 0xf) and (b[i + 3] & 0xc0) == 0:
                mps, = struct.unpack_from("<H", b, i + 4)
                if mps & 0x7ff in (8, 16, 32, 64, 128, 256, 512, 1023, 1024) and b[i + 3] & 3 in (1, 2):
                    print(f"{base + i:#x} lone EP {b[i+2]:#04x} {['ctrl','ISO','bulk','intr'][b[i+3]&3]} mps {mps & 0x7ff} interval {b[i+6]}")

if __name__ == "__main__":
    main()
