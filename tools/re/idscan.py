# Find where a 16-bit command ID is materialised: lui rd,ID>>12 then addi rd,rd,ID&0xfff (32-bit or c.addi),
# or a data table entry (u16/u32 ID followed by a code pointer). Usage: idscan.py ROOT 0x100a [more ids]
import struct, sys

def segs(root):
    out = [(0x2c0000, open(root + "/rom/main_rom_2c0000.rom", "rb").read())]
    elf = open(root + "/mm8108b2-rl.bin", "rb").read()
    phoff, = struct.unpack_from("<I", elf, 0x1c); phnum, = struct.unpack_from("<H", elf, 0x2c)
    for i in range(phnum):
        t, off, va, _, fs, ms, _, _ = struct.unpack_from("<8I", elf, phoff + 32 * i)
        if t == 1 and fs: out.append((va, elf[off:off + fs]))
    return out

def sx(v, b): return v - (1 << b) if v & (1 << (b - 1)) else v

def main():
    root, ids = sys.argv[1], [int(x, 0) for x in sys.argv[2:]]
    for base, b in segs(root):
        for off in range(0, len(b) - 4, 2):
            w = struct.unpack_from("<I", b, off)[0]
            if (w & 0x7f) == 0x37 and (w & 3) == 3:
                rd, hi = (w >> 7) & 31, w & 0xfffff000
                pc = off + 4
                for _ in range(4):
                    if pc + 2 > len(b): break
                    h = struct.unpack_from("<H", b, pc)[0]
                    if h & 3 != 3:
                        if h & 3 == 1 and h >> 13 == 0 and ((h >> 7) & 31) == rd:
                            v = hi + sx(((h >> 12) & 1) << 5 | (h >> 2) & 31, 6)
                            if v in ids: print(f"{base+off:#x} code lui+c.addi -> {v:#x}")
                        pc += 2; continue
                    w2 = struct.unpack_from("<I", b, pc)[0]
                    if (w2 & 0x707f) == 0x13 and ((w2 >> 15) & 31) == rd:
                        v = (hi + sx(w2 >> 20, 12)) & 0xffffffff
                        if v in ids: print(f"{base+off:#x} code lui+addi -> {v:#x}")
                    pc += 4
        for off in range(0, len(b) - 8, 2):
            v16 = struct.unpack_from("<H", b, off)[0]
            if v16 in ids:
                nxt = struct.unpack_from("<I", b, (off + 4) & ~3)[0] if ((off + 4) & ~3) + 4 <= len(b) else 0
                if 0x200000 <= nxt < 0x350000 and nxt & 1 == 0:
                    print(f"{base+off:#x} data {v16:#06x} next word {nxt:#x}")

if __name__ == "__main__":
    main()
