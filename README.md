# mm8108

Morse Micro MM8108 (HaLow) as a receive-only SDR: reverse-engineering notes plus the userspace driver.

| Path | What |
|:--|:--|
| `docs/phy_re/` | PHY/MAC reverse-engineering notes (UPHY/LPHY RAM and ROM, BCF, OFDM coprocessor) |
| `tools/` | Python reference stack the driver was ported from (`mmusb`, `boot`, `yaps`, `tune`, `owner`, `radio`), ROM dumper, ELF builders for Binja |
| `tools/re/` | Static-analysis helpers (custom-1 census and listing, command ID scan, USB descriptors). Each takes the repo root as its first arg |
| `driver/` | libmmsdr (`mmsdr.dll`, C ABI in `include/mmsdr.h`, see [docs/libmmsdr.md](docs/libmmsdr.md)) and its clients: SDR++ module, `mm_rtltcp`, `mmtest`, `mmsdr_rx`, `mmsdr_info`; hardware tests in `driver/test/` |

## Local-only files (not in git)

These are Morse Micro property and stay out of the repo, and so does their content: `driver/src/patches.h` carries only our patch masks plus one signature of the stock words, which the driver recomputes from the user's firmware at boot. Put the files here so the tools and build run in place:

| File | Source |
|:--|:--|
| `mm8108b2-rl.bin` | Morse firmware ELF |
| `tools/bcf/bcf_mf15457.bin` | Board config for the MF15457 module |
| `tools/mm8108b2-rl-ringfree.bin` | `tools/patch_ringfree.py` output |
| `rom/*.rom` | `tools/romdump.py` |
| `rom/mm8108_phy.elf`, `rom/mm8108_mac.elf` | `tools/mkphyelf.py`, `tools/mkmacelf.py` |

## Build (Windows, for now)

`driver\build.bat` builds against the installed SDR++ release. Deploy `mmsdr.dll` and `mm_libusb-1.0.dll` next to each client exe (for SDR++, next to `sdrpp.exe`; the module goes in `modules\`). It needs SDR++ source at `%SDRPP_SRC%` (default `..\..\sdrpp_src`) and import libs from `mkimplib.py` in `driver\build\`.

When a client passes no firmware path, the library looks for `mm8108b2-rl.bin` and `bcf_mf15457.bin` (or `bcf\bcf_mf15457.bin`) in: the `fw_dir` open argument, `%MMSDR_FW_DIR%`, the directory holding `mmsdr.dll`, then the working directory. For SDR++ that means dropping both files next to `sdrpp.exe`, or setting `MMSDR_FW_DIR`, or editing `mm8108_config.json`.

## Rules

- MMIO reads go only to registers the firmware itself touches, at most 60 B per read.

## License

MIT (see `LICENSE`). Third-party pieces: `driver/include/libusb.h` is libusb's header (LGPL-2.1 or later, its notice is in the file); the SDR++ module (`driver/src/main.cpp`) links the GPL-3 SDR++ core, so that binary is GPL-3 as a whole. The Morse Micro firmware, BCF and ROM images are not part of this project and are never distributed with it.
