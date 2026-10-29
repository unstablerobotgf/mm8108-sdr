# Unique-ID probe: physical OTP banks (mm81xx_otp_read sequence, on a reset chip) then the fw MAC from the
# extended host table after boot. Usage: uidprobe.py TOOLS_DIR FW BCF [nbanks]
import struct, subprocess, sys, time

def otp_read_all(m, nbanks):
    w, r = m.w32, m.r32
    w(0x4010, 1); time.sleep(0.001); w(0x4014, 1); time.sleep(0.001); w(0x4018, 1); time.sleep(0.001)   # power up
    w(0x401c, 0); w(0x4020, 1); time.sleep(0.001)                                                     # read enable
    out = {}
    for ecc in (1, 0):                   # PECCRDB: 1 = ignore ECC (as the driver reads board type/country)
        for bank in range(nbanks):
            w(0x4034, bank); w(0x4048, ecc)
            w(0x400c, (r(0x400c) & ~1) | 1)
            for _ in range(5):
                time.sleep(0.0002)
                if not r(0x400c) & 1: break
            else:
                out[(ecc, bank)] = None; continue
            out[(ecc, bank)] = r(0x4040)
    w(0x4020, 0); time.sleep(0.001)                                                                   # read disable
    w(0x4018, 0); w(0x4014, 0); w(0x4010, 0)                                                          # power down
    return out

def main():
    tools, fw, bcf = sys.argv[1:4]; nb = int(sys.argv[4]) if len(sys.argv) > 4 else 32
    sys.path.insert(0, tools)
    from mmusb import MM
    import importlib.util                # boot.py by path: it reads sys.argv[1] at import for its own path setup
    spec = importlib.util.spec_from_file_location("boot", f"{tools}/boot.py"); boot = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(boot)
    m = MM()
    print(f"chip id {m.r32(0x2d20):#010x}")
    boot.digital_reset(m)
    otp = otp_read_all(m, nb)
    for bank in range(nb):
        a, b = otp[(1, bank)], otp[(0, bank)]
        f = lambda v: "  timeout " if v is None else f"{v:#010x}"
        print(f"otp bank {bank:2d}: raw {f(a)}  ecc {f(b)}")
    import usb.util; usb.util.dispose_resources(m.dev); del m   # boot.py opens its own handle
    r = subprocess.run([sys.executable, "-I", f"{tools}/boot.py", tools, fw, bcf, "US"], capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-400:])
    m = MM()
    ht = m.r32(0x2d40)
    if not ht or m.r32(ht) != 0xdeadbeef: print(f"fw not running (host table {ht:#x})"); return
    ext = m.r32(ht + 0x18); et = m.read(ext, 16)
    print(f"ext host table @{ext:#x}: {et.hex()}  -> MAC {':'.join(f'{x:02x}' for x in et[4:10])}")

if __name__ == "__main__":
    main()
