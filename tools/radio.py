# Full-rate SDR mode for the MM8108 (Etna MF15457): one call retunes and leaves the ring streaming at ~8 MS/s.
# retune = reboot -> SET_CHANNEL -> MEDIUM_EVAL=1 (pure measurement PHY) -> freeze UPHY in its measurement path
# (ring patch WITHOUT the setter2 safety NOPs: a fw ASSERT parks UPHY in puts()) -> host owns the ring.
# After freezing, the firmware ignores commands; the next retune reboots it.
import os, sys, time, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, '..', 'mm8108b2-rl.bin'); BCF = os.path.join(HERE, 'bcf', 'bcf_mf15457.bin')

def _boot():
    import subprocess
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'boot.py'), HERE, FW, BCF, 'US'], capture_output=True, text=True)
    if 'host table ptr = 0x' not in r.stdout: raise RuntimeError('boot failed:\n' + r.stdout + r.stderr)

CLAMP_W, CLAMP_ENC = 0x286988, 'e362f716'    # MAC SET_CHANNEL: bltu at 0x28698a rejects (unsigned)(f-750M) > 200M

def unclamp(m):
    # NOP the 750-950 MHz range check (8 B RMW: the 4 B branch sits at +2)
    o = m.read(CLAMP_W, 8)
    if o[2:6] == bytes.fromhex('13000000'): return
    assert o[2:6] == bytes.fromhex(CLAMP_ENC[2:] + CLAMP_ENC[:2]) or o[2:6] == bytes.fromhex('6362f716'), o.hex()
    m.write(CLAMP_W, o[:2] + bytes.fromhex('13000000') + o[6:])

DIV2_SITES = [(0x5011e8, 2, '63e38700'), (0x542eb4, 0, '63e42701')]   # UPHY/LPHY set_lo 'if f > 1 GHz: no doubling'

def force_div2(m):
    # make both set_lo routines always program VCO = 2 x LO (the /2 path), even above 1 GHz
    for wa, off, enc in DIV2_SITES:
        o = m.read(wa, 8)
        if o[off:off+4] == bytes.fromhex('13000000'): continue
        assert o[off:off+4] == bytes.fromhex(enc), (hex(wa), o.hex())
        m.write(wa, o[:off] + bytes.fromhex('13000000') + o[off+4:])

def retune(freq_hz, bw_mhz=8, unclamped=False, div2=False):
    from yaps import Yaps
    import owner, usb.util
    from tune import tune
    t0 = time.perf_counter(); _boot(); tb = time.perf_counter()
    y = Yaps(); m = y.m
    try:
        if unclamped: unclamp(m)
        if div2: force_div2(m)
        st, pwr, w, fs, fu = tune(y, freq_hz, bw_mhz, 1 if bw_mhz < 4 else 2)
        if st: raise RuntimeError(f'SET_CHANNEL status {st}')
        ms, _ = y.send(0x811C, b'\x01')
        for addr, old, new in owner.diff():
            if addr in owner.SAFETY_WORDS: continue
            if owner._stable_read(m, addr, old, new) == old: m.write(addr, new)
        owner.take_ring(m)
    finally:
        usb.util.dispose_resources(m.dev)       # WinUSB is exclusive: always release, even on a rejected tune
    return {'synth_hz': fs, 'meval_status': ms, 'boot_s': tb - t0, 'total_s': time.perf_counter() - t0}

if __name__ == '__main__':
    r = retune(float(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 8)
    print(f"retuned to {r['synth_hz']/1e6:.4f} MHz; boot {1e3*r['boot_s']:.0f} ms, total {1e3*r['total_s']:.0f} ms")
