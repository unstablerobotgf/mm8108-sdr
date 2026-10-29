# Census of RISC-V custom-1 (opcode 0x2b) instructions in UPHY RAM + ROM, with constant operand tracking.
# Linear sweep over known code ranges (RV32IMC); constants from lui/addi/c.li/c.lui/c.addi/c.mv in the same block.
import struct, sys, collections, json

def sx(v, b): return v - (1 << b) if v & (1 << (b - 1)) else v

def load(root):
    elf = open(root + "/rom/mm8108_phy.elf", "rb").read()
    phoff, = struct.unpack_from("<I", elf, 0x1c); phnum, = struct.unpack_from("<H", elf, 0x2c)
    segs = []
    for i in range(phnum):
        t, off, va, _, fs, ms, _, _ = struct.unpack_from("<8I", elf, phoff + 32 * i)
        if fs: segs.append((va, elf[off:off + fs]))
    return segs

RANGES = [(0x500000, 0x50fb9e), (0x520000, 0x52c000), (0x52cf10, 0x535000)]

def main():
    root = sys.argv[1]
    mem = {}
    for va, d in load(root):
        mem[va] = d
    def get(a, n):
        for va, d in mem.items():
            if va <= a and a + n <= va + len(d): return d[a - va:a - va + n]
        return None
    out = []
    for lo, hi in RANGES:
        pc, regs = lo, {}
        while pc < hi:
            h = get(pc, 2)
            if h is None: break
            h = struct.unpack("<H", h)[0]
            if h & 3 != 3:                                       # RVC
                q, f3 = h & 3, h >> 13; rd = (h >> 7) & 31
                if q == 1 and f3 == 2 and rd: regs[rd] = sx(((h >> 12) & 1) << 5 | (h >> 2) & 31, 6)   # c.li
                elif q == 1 and f3 == 3 and rd not in (0, 2): regs[rd] = sx(((h >> 12) & 1) << 5 | (h >> 2) & 31, 6) << 12  # c.lui
                elif q == 1 and f3 == 0 and rd: regs[rd] = regs[rd] + sx(((h >> 12) & 1) << 5 | (h >> 2) & 31, 6) if rd in regs else None
                elif q == 2 and f3 == 4 and not (h >> 12) & 1 and (h >> 2) & 31 and rd:   # c.mv
                    regs[rd] = regs.get((h >> 2) & 31)
                elif q == 1 and f3 in (5, 6, 7) or q == 2 and f3 == 4 and (h >> 2) & 31 == 0: regs = {}  # branch/jump
                elif rd: regs.pop(rd, None)
                regs = {k: v for k, v in regs.items() if v is not None}
                pc += 2; continue
            w = get(pc, 4)
            if w is None: break
            w = struct.unpack("<I", w)[0]; op = w & 0x7f; rd = (w >> 7) & 31; rs1 = (w >> 15) & 31; rs2 = (w >> 20) & 31
            f3 = (w >> 12) & 7; f7 = w >> 25
            if op == 0x2b:
                out.append({"pc": pc, "f7": f7, "f3": f3, "rd": rd, "rs1": rs1, "rs2": rs2,
                            "v1": regs.get(rs1), "v2": regs.get(rs2)})
                if rd: regs.pop(rd, None)
            elif op == 0x37: regs[rd] = w & 0xfffff000
            elif op == 0x13 and f3 == 0: regs[rd] = (regs[rs1] + sx(w >> 20, 12)) & 0xffffffff if rs1 in regs else (sx(w >> 20, 12) if rs1 == 0 else None)
            elif op in (0x63, 0x6f, 0x67): regs = {}
            elif rd: regs.pop(rd, None)
            regs = {k: v for k, v in regs.items() if v is not None}
            pc += 4
    json.dump(out, open(sys.argv[2], "w"))
    by = collections.defaultdict(list)
    for o in out: by[(o["f3"], o["f7"])].append(o)
    print(f"{len(out)} custom-1 instructions, {len(by)} (f3,f7) kinds")
    for (f3, f7), lst in sorted(by.items()):
        v2 = collections.Counter(hex(o["v2"]) for o in lst if o["v2"] is not None).most_common(4)
        v1 = collections.Counter(hex(o["v1"]) for o in lst if o["v1"] is not None).most_common(3)
        rdu = sum(1 for o in lst if o["rd"])
        print(f"f3={f3} f7={f7:#04x} n={len(lst):4d} rd-used={rdu:3d} ranges={sorted(set(hex(o['pc'] & ~0xfff) for o in lst))[:4]} rs2c={v2} rs1c={v1}")

if __name__ == "__main__":
    main()
