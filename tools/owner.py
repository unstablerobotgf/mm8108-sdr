# Ring ownership helpers: live patch/revert (word diff stock vs ringfree), safe tune, ring take, fs measurement.
import os, sys, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from yaps import Yaps
from tune import tune

HERE = os.path.dirname(os.path.abspath(__file__))
STOCK = os.path.join(HERE, '..', 'mm8108b2-rl.bin'); RINGFREE = os.path.join(HERE, 'mm8108b2-rl-ringfree.bin')
CTRL, BASE, LEN, PTR = 0x40904c, 0x409044, 0x409048, 0x40906c
SAFE_BASE, SAFE_LEN = 0x229000, 30144        # ends 0x246700: inside YAPS tc_tx pool (unused, we never TX); tc_cmd pool starts 0x246800

def _imem(path):
    d = open(path, 'rb').read(); shoff, = struct.unpack_from('<I', d, 0x20); ne, n, si = struct.unpack_from('<HHH', d, 0x2e)
    S = [struct.unpack_from('<10I', d, shoff + i*ne) for i in range(n)]; so = S[si][4]; out = {}
    for s in S:
        nm = d[so+s[0]:d.index(b'\0', so+s[0])].decode()
        if nm.endswith('_imem'): out[nm] = (s[3], d[s[4]:s[4]+s[5]])
    return out

def stock_bytes(addr, n, path=STOCK):
    # bytes at a load address in the stock ELF (any PT_LOAD segment, so MAC code too, not just *_imem)
    d = open(path, 'rb').read(); phoff, = struct.unpack_from('<I', d, 0x1c); pe, pn = struct.unpack_from('<HH', d, 0x2a)
    for i in range(pn):
        t, off, va, pa, fsz = struct.unpack_from('<5I', d, phoff + i*pe)
        if t == 1 and pa <= addr and addr + n <= pa + fsz: return d[off + addr - pa:off + addr - pa + n]
    raise KeyError(f'{addr:#x} not in a loaded segment')

_DIFF = None
def diff():
    global _DIFF
    if _DIFF is None:
        a, b = _imem(STOCK), _imem(RINGFREE)
        _DIFF = [(va + o, a[k][1][o:o+4], b[k][1][o:o+4]) for k, (va, _) in a.items()
                 for o in range(0, len(a[k][1]) - 3, 4) if a[k][1][o:o+4] != b[k][1][o:o+4]]
    return _DIFF

def patched(m):
    states = {m.read(addr, 4) == new for addr, old, new in diff()}
    return states.pop() if len(states) == 1 else None

SAFETY_WORDS = {0x50c6d0, 0x545618}     # setter2 ptr==base checks: must be NOP'd before (and restored after) the rest
def set_patch(m, on):
    # ordering avoids a window where base/len writes are NOP'd but the setter's check still fails -> fw ASSERT -> UART spin
    ws = sorted(diff(), key=lambda w: (w[0] not in SAFETY_WORDS) if on else (w[0] in SAFETY_WORDS))
    for addr, old, new in ws:
        want, put = (old, new) if on else (new, old); live = _stable_read(m, addr, want, put)
        if live == put: continue
        if live != want: raise RuntimeError(f'unexpected word at {addr:08x}: {live.hex()}')
        m.write(addr, put)

def safe_tune(y, f, bw=2, pbw=1):
    set_patch(y.m, False); r = tune(y, f, bw, pbw); set_patch(y.m, True); return r

def take_ring(m, base=SAFE_BASE, ln=SAFE_LEN):
    c = m.r32(CTRL); m.w32(CTRL, c & ~1); m.w32(BASE, base); m.w32(LEN, ln); m.w32(CTRL, c | 1)

def measure_fs(m, secs=2.0):
    base, ln = m.r32(BASE), m.r32(LEN); prev = (m.r32(PTR) - base) // 4; t0 = tp = time.perf_counter(); acc = 0; worst = 0
    while time.perf_counter() - t0 < secs:
        p = (m.r32(PTR) - base) // 4; now = time.perf_counter(); worst = max(worst, now - tp)
        acc += (p - prev) % ln; prev, tp = p, now
    rate = acc / (tp - t0)
    return rate, worst < 0.5 * ln / max(rate, 1)     # trustworthy if no poll gap could hide a wrap

# patch-site groups (for bisecting side effects); a diff word may belong to several groups
SNAP_SITES = {0x507e1a}
BIT0_SITES = {0x50619c, 0x507f84, 0x5085d8, 0x50b200, 0x50c69c, 0x50c6c8, 0x5455e4, 0x545610}
def words_for(groups):
    sel = []
    for addr, old, new in diff():
        span = range(addr, addr + 4)
        g = set()
        if any(s in span or s + 2 in span for s in SNAP_SITES): g.add('snap')
        if any(s in span for s in BIT0_SITES): g.add('bit0')
        if not g: g.add('baselen')
        else:
            # a word can also hold a base/len NOP next to a bit0 NOP
            if any(old[i:i+2] != new[i:i+2] for i in (0, 2)) and len([1 for i in (0, 2) if old[i:i+2] != new[i:i+2]]) > len(g & {'bit0', 'snap'}): g.add('baselen')
        if g & set(groups): sel.append((addr, old, new))
    return sel

def _stable_read(m, addr, want, put, tries=50):
    # single-word reads occasionally glitch (LPHY IMEM words the core is fetching read back junk): retry, spaced out
    for i in range(tries):
        live = m.read(addr, 4)
        if live in (want, put): return live
        time.sleep(0.01)
    return live

def set_patch_groups(m, groups, on, cores=None):
    for addr, old, new in words_for(groups):
        if cores and not any(lo <= addr < hi for lo, hi in cores): continue
        want, put = (old, new) if on else (new, old); live = _stable_read(m, addr, want, put)
        if live == put: continue
        if live != want: raise RuntimeError(f'unexpected word at {addr:08x}: {live.hex()}')
        m.write(addr, put)
