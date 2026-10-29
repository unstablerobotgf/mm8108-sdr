# Build rom/mm8108_mac.elf: the firmware ELF's host/MAC segments (< 0x500000) + the dumped main ROM + the BCF
# segments + NOBITS SoC/PHY register windows, so one Binja DB sees RAM<->ROM calls, BCF refs and register xrefs.
# Usage: mkmacelf.py [out]
import os, struct, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
PF_R, PF_W, PF_X = 4, 2, 1


def load_segs(path, keep):
    elf = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', elf, 0x1c); phnum, = struct.unpack_from('<H', elf, 0x2c)
    out = []
    for i in range(phnum):
        t, off, va, _, fsz, msz, fl, _ = struct.unpack_from('<8I', elf, phoff + 32 * i)
        if t == 1 and msz and keep(va): out.append((va, elf[off:off + fsz], max(msz, fsz), fl & 7))
    return elf, out


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'rom', 'mm8108_mac.elf')
    elf, segs = load_segs(os.path.join(ROOT, 'mm8108b2-rl.bin'), lambda va: va < 0x500000)
    segs += load_segs(os.path.join(ROOT, 'tools', 'bcf', 'bcf_mf15457.bin'), lambda va: True)[1]
    rom = open(os.path.join(ROOT, 'rom', 'main_rom_2c0000.rom'), 'rb').read()
    segs.append((0x2c0000, rom, len(rom), PF_R | PF_X))
    # zero-fill register windows: low SoC regs (0x2d20 chip id, 0x1d00.., 0x4100 doorbells) and the PHY block
    segs += [(0x1000, b'', 0xf000, PF_R | PF_W), (0x400000, b'', 0x10000, PF_R | PF_W)]
    segs.sort()
    for (a, d, m, _), (b, *_rest) in zip(segs, segs[1:]):
        assert a + m <= b, f'overlap {a:#x}+{m:#x} > {b:#x}'
    entry, = struct.unpack_from('<I', elf, 0x18)
    hdr = bytearray(elf[:0x34])
    struct.pack_into('<III', hdr, 0x18, entry, 0x34, 0)                    # e_entry, e_phoff, e_shoff (no sections)
    struct.pack_into('<HHHHHH', hdr, 0x28, 0x34, 32, len(segs), 0, 0, 0)  # keeps e_flags (RVC/float ABI)
    body, ph, off = b'', b'', 0x34 + 32 * len(segs)
    for va, d, m, f in segs:
        ph += struct.pack('<8I', 1, off + len(body), va, va, len(d), m, f, 4)
        body += d + b'\0' * (-len(d) % 4)
    open(out, 'wb').write(bytes(hdr) + ph + body)
    for va, d, m, f in segs:
        print(f'{va:#08x} file {len(d):#7x} mem {m:#7x} {"R" if f & 4 else "-"}{"W" if f & 2 else "-"}{"X" if f & 1 else "-"}')
    print('wrote', out)


if __name__ == '__main__':
    main()
