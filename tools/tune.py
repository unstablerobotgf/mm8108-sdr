# Tune via SET_CHANNEL and verify: tune.py <freq_hz> [op_bw_mhz=2] [pri_bw_mhz=1]
import os, sys, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from yaps import Yaps

SYNTH, UPHY_FREQ = 0x40204c, 0x538e48

def synth_hz(w):
    # firmware: word = round(f*2^24/32 MHz), doubled when f <= 1 GHz (UPHY 0x5011bc)
    lo = (w / 2) * 32e6 / (1 << 24); hi = w * 32e6 / (1 << 24)
    return lo if lo <= 1e9 else hi

def tune(y, f, bw=2, pbw=1, idx=0):
    st, data = y.send(0x0001, struct.pack('<IBBBBBB', int(f), bw, pbw, idx, 0, 1, 0), timeout=3.0)
    pwr = struct.unpack_from('<i', data, 0)[0] if len(data) >= 4 else None
    time.sleep(0.02)
    w = y.m.r32(SYNTH)
    return st, pwr, w, synth_hz(w), y.m.r32(UPHY_FREQ)

if __name__ == '__main__':
    y = Yaps(verbose='-v' in sys.argv); sys.argv = [a for a in sys.argv if a != '-v']; print('status', y.status())
    f = float(sys.argv[1]); bw = int(sys.argv[2]) if len(sys.argv) > 2 else 2; pbw = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    st, pwr, w, fs, fu = tune(y, f, bw, pbw)
    print(f'SET_CHANNEL {f/1e6:.4f} MHz bw {bw}: status {st}, power_qdbm {pwr} ({pwr/4 if pwr is not None else 0:.2f} dBm)')
    print(f'  synth word 0x{w:08x} -> {fs/1e6:.6f} MHz (err {fs-f:+.0f} Hz); UPHY freq var {fu/1e6:.6f} MHz')
