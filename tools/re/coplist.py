# Linear listing of a UPHY range: capstone RV32IMC + custom-1 (0x2b) decode. Usage: coplist.py ROOT start end
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "pylib"))
import capstone

def load(root):
    elf = open(root + "/rom/mm8108_phy.elf", "rb").read()
    phoff, = struct.unpack_from("<I", elf, 0x1c); phnum, = struct.unpack_from("<H", elf, 0x2c)
    out = []
    for i in range(phnum):
        t, off, va, _, fs, ms, _, _ = struct.unpack_from("<8I", elf, phoff + 32 * i)
        if fs: out.append((va, elf[off:off + fs]))
    return out

R = ["zero","ra","sp","gp","tp","t0","t1","t2","s0","s1","a0","a1","a2","a3","a4","a5","a6","a7",
     "s2","s3","s4","s5","s6","s7","s8","s9","s10","s11","t3","t4","t5","t6"]

def main():
    root, lo, hi = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
    seg = next((va, d) for va, d in load(root) if va <= lo < va + len(d))
    va, d = seg; b = d[lo - va:hi - va]
    md = capstone.Cs(capstone.CS_ARCH_RISCV, capstone.CS_MODE_RISCV32 | capstone.CS_MODE_RISCVC)
    pc = 0
    while pc < len(b):
        h = struct.unpack_from("<H", b, pc)[0]
        if h & 3 == 3 and pc + 4 <= len(b):
            w = struct.unpack_from("<I", b, pc)[0]
            if w & 0x7f == 0x2b:
                f3, f7 = (w >> 12) & 7, w >> 25
                print(f"{lo+pc:#x}:  COP f7={f7:#04x} f3={f3} rd={R[(w>>7)&31]} rs1={R[(w>>15)&31]} rs2={R[(w>>20)&31]}   ; {w:08x}")
                pc += 4; continue
        ins = next(md.disasm(b[pc:pc + 4], lo + pc, 1), None)
        if ins is None:
            print(f"{lo+pc:#x}:  .half {h:#06x}"); pc += 2; continue
        print(f"{ins.address:#x}:  {ins.mnemonic} {ins.op_str}")
        pc += ins.size

if __name__ == "__main__":
    main()
