# Dump mask ROM ranges proven mapped by call targets from the ELF (first..last referenced page per core), 4 KB per read.
# Holes wedge the bridge (needs replug), so never read outside these. Usage: romdump.py <outdir>
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usb.util
from mmusb import MM
RANGES = {'host_rom': (0x2c0000, 0x2cd000), 'mac_rom': (0x329000, 0x32e000), 'uphy_rom': (0x520000, 0x52b000), 'lphy_rom': (0x550000, 0x553000)}
out = sys.argv[1]; m = MM()
try:
    for name, (a, b) in RANGES.items():
        data = bytearray()
        for p in range(a, b, 0x1000): data += m.read(p, 0x1000)
        fn = os.path.join(out, f'{name}_{a:06x}.bin'); open(fn, 'wb').write(data)
        print(f'{name}: {a:#08x}-{b:#08x} ({len(data)} B) -> {fn}; first 8 B {bytes(data[:8]).hex(" ")}, last 16 B {bytes(data[-16:]).hex(" ")}', flush=True)
finally: usb.util.dispose_resources(m.dev)
