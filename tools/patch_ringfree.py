# Build a firmware image where UPHY/LPHY never store to the I/Q ring regs (0x409044..0x409053), so the host owns the ring.
# Sites come from the resolved-store lists (<core>.mem from memref.py); each site's bytes are verified before patching.
# Usage: patch_ringfree.py <in.bin> <out.bin> <uphy.mem> <lphy.mem>
import struct, sys

RING_REGS = range(0x409044, 0x409054)
# v2: only kill stores that clear ctrl bit0 (andi -2) or move base/len; fw keeps bit1 (LPHY source) + field
CLEAR_BIT0 = {0x50619c, 0x507f84, 0x5085d8, 0x50b200, 0x50c69c, 0x50c6c8, 0x5455e4, 0x545610}
C_NOP, NOP32 = b'\x01\x00', struct.pack('<I', 0x00000013)

def sections(d):
    shoff, = struct.unpack_from('<I', d, 0x20); ne, n, si = struct.unpack_from('<HHH', d, 0x2e)
    S = [struct.unpack_from('<10I', d, shoff + i*ne) for i in range(n)]; so = S[si][4]
    return {d[so+s[0]:d.index(b'\0', so+s[0])].decode(): (s[3], s[4], s[5]) for s in S}

def sites(memfile):
    for ln in open(memfile):
        p = ln.split()
        pc, op, addr = int(p[0], 16), p[1], int(p[2], 16)
        if op in ('sw', 'sh', 'sb') and (addr in (0x409044, 0x409048) or (addr == 0x40904c and pc in CLEAR_BIT0)):
            enc = ln.split(':', 1)[1].split()[0]          # hex bytes as printed in the listing
            yield pc, bytes.fromhex(enc), ln.strip()

d = bytearray(open(sys.argv[1], 'rb').read()); secs = sections(d); n = 0
for memfile, sec in ((sys.argv[3], '.uphy_imem'), (sys.argv[4], '.lphy_imem')):
    va, off, size = secs[sec]
    for pc, orig, ln in sites(memfile):
        fo = off + pc - va
        assert va <= pc < va + size and bytes(d[fo:fo+len(orig)]) == orig, f'byte mismatch at {pc:08x}'
        d[fo:fo+len(orig)] = C_NOP if len(orig) == 2 else NOP32; n += 1
        print(f'{sec:11} {pc:08x} {orig.hex():8} -> nop   | {ln.split("  ", 1)[1]}')
# v3: skip the IRQ#12 I/Q snapshot (LO offset + capture takeover, ~30 ms episodes at 8 MHz): jal 0x50a6bc -> li a0,0
va, off, size = secs['.uphy_imem']; fo = off + 0x507e1a - va
assert bytes(d[fo:fo+4]) == bytes.fromhex('ef20300a'), 'snapshot jal bytes mismatch'
d[fo:fo+4] = bytes.fromhex('13050000'); n += 1; print('.uphy_imem  00507e1a ef20300a -> li a0,0 | skip IRQ#12 I/Q snapshot')
# v4: setter2 re-enable check 'bne a0, ptr' -> nop, so callers see success with the host-owned ring (else a firmware
# ASSERT fires and spins in puts() on the unconnected debug UART, wedging the core)
for sec, pc, enc in (('.uphy_imem', 0x50c6d0, '6315d502'), ('.lphy_imem', 0x545618, '6315d502')):
    va, off, size = secs[sec]; fo = off + pc - va
    assert bytes(d[fo:fo+4]) == bytes.fromhex(enc), f'setter2 check bytes mismatch at {pc:x}'
    d[fo:fo+4] = bytes.fromhex('13000000'); n += 1; print(f'{sec:11} {pc:08x} {enc} -> nop   | setter2 ptr==base check')
open(sys.argv[2], 'wb').write(d); print(f'{n} sites patched -> {sys.argv[2]}')
