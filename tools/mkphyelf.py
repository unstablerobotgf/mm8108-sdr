# Build rom/mm8108_phy.elf: the firmware ELF's UPHY/LPHY segments + the dumped UPHY/LPHY ROMs + NOBITS MMIO/DMEM,
# so one Binja DB sees RAM<->ROM calls and register xrefs. Usage: mkphyelf.py [out]
import os, struct, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
PF_R, PF_W, PF_X = 4, 2, 1


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'rom', 'mm8108_phy.elf')
    elf = open(os.path.join(ROOT, 'mm8108b2-rl.bin'), 'rb').read()
    phoff, = struct.unpack_from('<I', elf, 0x1c); phnum, = struct.unpack_from('<H', elf, 0x2c)
    segs = []                                               # (vaddr, data, memsz, flags)
    for i in range(phnum):
        _, off, va, _, fsz, msz, _, _ = struct.unpack_from('<8I', elf, phoff + 32 * i)
        if va >= 0x500000:                                  # UPHY/LPHY imem/dmem/aonmem only
            x = va < 0x510000 or 0x538000 <= va < 0x548000 or va >= 0x55c000
            segs.append((va, elf[off:off + fsz], msz, PF_R | (PF_X if x else PF_W)))
    for va, fn in ((0x520000, 'uphy_rom_520000.rom'), (0x550000, 'lphy_rom_550000.rom')):
        segs.append((va, open(os.path.join(ROOT, 'rom', fn), 'rb').read(), None, PF_R | PF_X))
    # zero-fill: rest of the DMEMs (bss/stack) and the PHY register window, so data xrefs resolve
    segs = [(va, d, max(m or 0, len(d)), f) for va, d, m, f in segs]
    ext = {0x510000: 0x10000, 0x548000: 0x8000}
    segs = [(va, d, ext.get(va, m), f) for va, d, m, f in segs]
    segs.append((0x400000, b'', 0x10000, PF_R | PF_W))
    segs.sort()
    for (a, d, m, _), (b, *_rest) in zip(segs, segs[1:]):
        assert a + m <= b, f'overlap {a:#x}+{m:#x} > {b:#x}'
    hdr = bytearray(elf[:0x34])
    struct.pack_into('<III', hdr, 0x18, 0x540000, 0x34, 0)                 # e_entry, e_phoff, e_shoff (no sections)
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
