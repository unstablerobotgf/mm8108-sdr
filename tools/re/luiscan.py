# Effective-address scan of a raw RV32GC image: follow each `lui rd, imm` through addi and loads/stores off rd
# (32-bit and RVC) for a few instructions, and report accesses inside [lo, hi). Approximate: linear, no branches.
import struct, sys, collections

def sx(v, bits): return v - (1 << bits) if v & (1 << (bits - 1)) else v

def follow(b, off, rd, val, base):
    out = []; pc = off + 4
    for _ in range(24):
        if pc + 2 > len(b): break
        h = struct.unpack_from("<H", b, pc)[0]
        if h & 3 != 3:                                   # RVC
            q, f3 = h & 3, h >> 13
            if q == 0 and f3 in (2, 6):                  # c.lw / c.sw
                rs1 = ((h >> 7) & 7) + 8
                imm = ((h >> 10) & 7) << 3 | ((h >> 6) & 1) << 2 | ((h >> 5) & 1) << 6
                if rs1 == rd: out.append((base + pc, "st" if f3 == 6 else "ld", val + imm))
                if f3 == 2 and ((h >> 2) & 7) + 8 == rd: break
            elif q == 1 and f3 == 0 and ((h >> 7) & 31) == rd:          # c.addi
                val += sx(((h >> 12) & 1) << 5 | (h >> 2) & 31, 6)
            elif ((h >> 7) & 31) == rd and (q == 1 and f3 in (2, 3) or q == 2 and f3 in (2, 4)):
                break                                    # c.li / c.lui / c.lwsp / c.mv/c.add overwrite rd
            pc += 2; continue
        if pc + 4 > len(b): break
        w = struct.unpack_from("<I", b, pc)[0]; op = w & 0x7f; wrd = (w >> 7) & 31; rs1 = (w >> 15) & 31
        if op == 0x03 and rs1 == rd: out.append((base + pc, "ld", val + sx(w >> 20, 12)))
        if op == 0x23 and rs1 == rd: out.append((base + pc, "st", val + sx(((w >> 25) << 5) | ((w >> 7) & 31), 12)))
        if op == 0x13 and (w >> 12) & 7 == 0 and wrd == rd and rs1 == rd: val += sx(w >> 20, 12)
        elif wrd == rd and op in (0x03, 0x13, 0x17, 0x33, 0x37, 0x67, 0x6f): break
        pc += 4
    return out

def main():
    path, base, lo, hi = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), int(sys.argv[4], 0)
    b = open(path, "rb").read(); acc = []
    for off in range(0, len(b) - 3, 2):
        w = struct.unpack_from("<I", b, off)[0]
        if (w & 0x7f) == 0x37 and 0x3ff <= (w >> 12) <= 0x411:
            acc += follow(b, off, (w >> 7) & 31, w & 0xfffff000, base)
    acc = [a for a in acc if lo <= a[2] < hi]
    for pc, k, ea in sorted(acc): print(f"{pc:#x} {k} {ea:#x}")
    print("by register:", {hex(k): v for k, v in sorted(collections.Counter(a[2] for a in acc).items())})

if __name__ == "__main__":
    main()
