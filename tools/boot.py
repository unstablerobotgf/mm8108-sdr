# Userspace MM8108 USB firmware boot, mirroring morse_firmware_init() + mm810x_digital_reset() for USB.
# Usage: boot.py <wsl_dir> <fw.bin> <bcf.bin> [country=US]
import sys, time, struct
sys.path.insert(0, sys.argv[1])
from mmusb import MM

AON_LATCH = 0x405020          # also RC_CLK_POWER_OFF (bit6) and AON_PMU_CTRL (bit7)
AON_BASE, AON_COUNT, AON_USB_KEEP = 0x2114, 2, 0x8
MSI, MANIFEST_PTR, BOOT_ADDR, SECBOOT = 0x4100, 0x2d40, 0x2084, 0x11fff8
SKIP_PADDR = (0x400000, 0xC00000)  # iflash/dflash, self-hosted only

def elf(d):
    e_entry, = struct.unpack_from('<I', d, 0x18); phoff, shoff = struct.unpack_from('<II', d, 0x1c)
    phes, phn, shes, shn, shstr = struct.unpack_from('<HHHHH', d, 0x2a)
    ph = [struct.unpack_from('<8I', d, phoff + i*phes) for i in range(phn)]
    sh = [struct.unpack_from('<10I', d, shoff + i*shes) for i in range(shn)]
    so = sh[shstr][4]; nm = lambda o: d[so+o:d.index(b'\0', so+o)].decode()
    return e_entry, ph, {nm(s[0]): s for s in sh}

def pad(b): return b + b'\xff' * (-len(b) % 4)

def latch(m):
    v = m.r32(AON_LATCH)
    for x in (v & ~1, v | 1, v & ~1): m.w32(AON_LATCH, x); time.sleep(0.005)

def digital_reset(m):
    m.w32(AON_LATCH, m.r32(AON_LATCH) & ~0x40); latch(m); time.sleep(0.002)   # internal slow clock on
    m.w32(AON_LATCH, m.r32(AON_LATCH) & ~0x80); latch(m)                      # PMU ctrl off CPU
    m.out.write(struct.pack('<III', 0x02, 0, 0))                              # MORSE_USB_RESET (NDR)
    time.sleep(0.05)

def main():
    m = MM(); fw = open(sys.argv[2], 'rb').read(); bcf = open(sys.argv[3], 'rb').read()
    prearm = '--prearm' in sys.argv; sys.argv = [a for a in sys.argv if a != '--prearm']
    cc = sys.argv[4] if len(sys.argv) > 4 else 'US'
    print(f'chip id 0x{m.r32(0x2d20):08x}')
    digital_reset(m); print('digital reset done')
    m.w32(MANIFEST_PTR, 0)
    entry, ph, _ = elf(fw); t0 = time.time(); total = 0
    for p_type, off, va, pa, fsz, msz, fl, al in ph:
        if pa in SKIP_PADDR or p_type != 1 or not msz or not fsz: continue
        m.write(pa, pad(fw[off:off+fsz])); total += fsz
    print(f'loaded {total} B in {time.time()-t0:.2f}s ({total/(time.time()-t0)/1e6:.2f} MB/s)')
    # verify one segment readback
    p = next(x for x in ph if x[0] == 1 and x[4] and x[3] not in SKIP_PADDR)
    ok = m.read(p[3], 256) == fw[p[1]:p[1]+256]; print(f'readback @{p[3]:08x}: {"OK" if ok else "MISMATCH"}')
    if entry: m.w32(BOOT_ADDR, entry); print(f'boot addr <- 0x{entry:08x}')
    # BCF: .board_config + .regdom_<cc>, word-padded, at fw_info BCF_ADDR
    _, _, fsec = elf(fw); fi = fsec['.fw_info']; info = fw[fi[4]:fi[4]+fi[5]]; i = 0; baddr = bsize = None
    while i + 4 <= len(info):
        t, l = struct.unpack_from('<HH', info, i)
        if t == 1: baddr, = struct.unpack_from('<I', info, i+4)
        if t == 3: bsize, = struct.unpack_from('<I', info, i+4)
        if t == 0 and l == 0: break
        i += 4 + l
    _, _, bs = elf(bcf); g = lambda n: pad(bcf[bs[n][4]:bs[n][4]+bs[n][5]])
    blob = g('.board_config') + g(f'.regdom_{cc}'); assert len(blob) <= bsize
    m.write(baddr, blob); print(f'BCF {len(blob)} B -> 0x{baddr:08x} (max {bsize})')
    # trigger
    for k in range(AON_COUNT): m.w32(AON_BASE + 4*k, AON_USB_KEEP if k == 0 else 0)
    if prearm:   # ring-free image: fw can't arm the ring itself, and boot cal waits on the sample counter
        m.w32(0x409044, 0x5141b0); m.w32(0x409048, 0x708); m.w32(0x40904c, 1); print('ring pre-armed')
    latch(m); m.w32(MSI, 1); time.sleep(0.06)
    print(f'secure boot status = {m.r32(SECBOOT) & 0xff}')
    t0 = time.time()
    while time.time() - t0 < 0.55:
        try: ptr = m.r32(MANIFEST_PTR)
        except Exception: ptr = 0
        if ptr: print(f'host table ptr = 0x{ptr:08x} after {1000*(time.time()-t0):.0f} ms'); break
        time.sleep(0.005)
    else: print('host table ptr never set (boot timeout)')
    for n, a in [('RING_BASE', 0x409044), ('RING_LEN', 0x409048), ('RING_CTRL', 0x40904c), ('RING_WCNT', 0x409050)]:
        print(f'{n:10} = 0x{m.r32(a):08x}')
    c0 = m.r32(0x409050); time.sleep(0.01); c1 = m.r32(0x409050); print(f'WCNT over 10 ms: {c0} -> {c1}')

if __name__ == '__main__':
    main()
