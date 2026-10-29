# Build x64 import libs for DLLs shipped without them (SDR++ release): dumpbin /exports -> .def -> lib /def.
# Usage: mkimplib.py <vc_bin_dir> <out_dir> <dll...>
import os, re, subprocess, sys
vc, out = sys.argv[1], sys.argv[2]
for dll in sys.argv[3:]:
    txt = subprocess.run([os.path.join(vc, 'dumpbin.exe'), '/exports', dll], capture_output=True, text=True, check=True).stdout
    names = [m.group(1) for m in re.finditer(r'^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)', txt, re.M)]
    base = os.path.splitext(os.path.basename(dll))[0]; deff = os.path.join(out, base + '.def')
    with open(deff, 'w') as f: f.write(f'LIBRARY "{os.path.basename(dll)}"\nEXPORTS\n' + ''.join(f'    {n}\n' for n in names))
    subprocess.run([os.path.join(vc, 'lib.exe'), '/nologo', '/machine:x64', f'/def:{deff}', f'/out:{os.path.join(out, base + ".lib")}'],
                   check=True, stdout=subprocess.DEVNULL)
    print(f'{base}.lib: {len(names)} exports')
