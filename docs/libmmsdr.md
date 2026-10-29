# libmmsdr: C interface overview

Headers: [`driver/include/mmsdr.h`](../driver/include/mmsdr.h) (stable API) and [`driver/include/mmsdr_debug.h`](../driver/include/mmsdr_debug.h) (expert/RE, no ABI promise). Implementation: [`driver/src/mmsdr.cpp`](../driver/src/mmsdr.cpp) over the core in `mm8108.cpp`.

**Status (v0.1, 2026-10-26): phase 1 done on Windows.** RX, settings, host DSP, sync and callback streaming, ppb and the debug interface are implemented and checked on hardware (see [Phase 1 verification](#phase-1-hardware-verification-2026-10-26)). Placeholders return `MMSDR_ERR_UNSUPPORTED`: sensors, `cal_freq_measure`, `dbfs_to_dbm`, `lphy_msg`, all of TX.

Clients built on it: the SDR++ module, `mm_rtltcp`, `mmtest`, plus two new tools in the librtlsdr spirit: `mmsdr_rx` (capture via the sync API, like `rtl_sdr`) and `mmsdr_info` (devices, settings, debug keys). Hardware tests: `driver/test/rx_reentry`, `driver/test/uid_paths`.

## Goal

The goal is one shared driver behind every client, the way librtlsdr and libbladeRF work. Today `mm8108.cpp` is compiled separately into the SDR++ module, `mm_rtltcp` and `mmtest`. After the refactor each client becomes a thin layer over `mmsdr.dll` / `libmmsdr.so`:

```
 SDR++ module   mm_rtltcp   mmtest   SoapyMMSDR   (Python ctypes, GNU Radio, ...)
       \            |          |          /
        +------ libmmsdr (C ABI: mmsdr.h) ----+---- mmsdr_debug.h (RE knobs)
        |   C++ core: the current MM8108 class, split into
        |   usb bridge | boot+YAPS | tune (KDCO, hop, park retry) | reader/output | cal | tx (stub)
        +---------------------- libusb-1.0 ----------------------
```

## Design rules

| Rule | Why (mechanism) |
|:--|:--|
| C ABI, C++ inside | Exceptions and `std::string` cannot cross a C boundary. A C ABI binds to ctypes, Rust, Soapy and GNU Radio without shims. |
| Opaque `mmsdr_dev*`, many devices | Today `libusb_open_device_with_vid_pid` takes the first device it finds. Listing plus a serial/path selector fixes that. |
| `struct_size` first in every struct | The library reads only the fields the caller's build knows about, so minor versions can add tail fields without breaking old binaries. |
| Typed negative errors, plus detail text | Callers branch on `NEEDS_REPLUG` vs `PARK` vs `USB`. The rich message we print today stays available via `mmsdr_last_error_detail`. |
| No `getenv` in the library | The ten `MM_*` hooks become `mmsdr_debug_set` keys, so a library user can't be surprised by the environment. |
| Log callback, no `fprintf` | Hosts route messages into their own logs (SDR++ spdlog, Soapy logger). |
| One owner of the USB pipe | Live peek/poke and gain writes already queue through the reader so reads complete in submit order. The public API keeps that path and adds no side channel. |
| Chip-safety rules live in the library | The 60 B MMIO read cap and the "firmware-touched registers only" allowlist protect every client from wedging the chip (a 256 B read of 0x409000 did). These protect the silicon, never the operator: TX is ungated. |
| Every toggle is discoverable | Typed setters plus named settings (`mmsdr_list_settings`), so generic hosts (Soapy `getSettingInfo`/`writeSetting`, GUIs) can see and flip AGC, IQ correction, EQ, DC mode and hop without knowing this header. |

## Lifecycle

```
mmsdr_open ──► OPEN (USB claimed) ──► BOOTED (fw running) ──► STREAMING
                 │  mmsdr_boot or first tune/start ▲               │
                 └──────────── mmsdr_close ◄───────┴── rx_stop ◄───┘
```

- `open` only claims USB. Boot (~0.35 s) happens lazily or via `mmsdr_boot`, so a scanner UI can open several devices quickly.
- Firmware and BCF are never bundled (Morse property). Paths are searched in this order: the explicit path, then `fw_dir`, then `$MMSDR_FW_DIR`, then the directory holding `mmsdr.dll`, then `.`. Our firmware patches and the UPHY packer are compiled in (`patches.h`, `packer_blob.h`), so they come along automatically.
- `patches.h` holds only our side of each patch: `(addr, mask, bits)` per ring word and the NOP sites. The stock words are read from the user's firmware ELF at boot and checked as one FNV-1a 64 signature (`STOCK_SIG`); a different image fails with `MMSDR_ERR_FIRMWARE` before anything is written. Nothing of the vendor image is in the repo.

## Applying settings

Each setter fills `mmsdr_apply_result`, saying what it actually did:

| action | when | stream gap |
|:--|:--|:--|
| `NONE` | not streaming; stored for the next start | n/a |
| `QUEUED` | anything that touches the chip, requested from inside the RX callback (tune, rate, CS8 format, ppb, gain, DC mode); runs on the control thread | as below |
| `LIVE` | gain, DC mode, AGC, IQ correction, EQ, CS16/CF32 switch | none (gain and DC mode re-hold the servo for 20 ms) |
| `HOP` | retune while streaming with hop enabled | ~60 ms (verified 61 ms) |
| `REBOOT` | first tune, format change (CS8 packer), or a hop not possible | ~0.75 s |

The result also reports the locked LO and delivered fs from hardware readback (synth word and resampler ratio), not the requested values. The park retry and the ratio re-check from the wide-mode race fix stay inside the library, and `retries` reports them.

## Streaming

- Two modes, picked by `mmsdr_rx_args.callback`:
  - **async callback**, the librtlsdr style, for SDR++ and rtl_tcp;
  - **sync `mmsdr_rx_read`** into a host ring, the libbladeRF style, a natural fit for Soapy `readStream`.
- Formats: `CS16` (native), `CS8` (UPHY packer, needs a reboot), and `CF32` (converted in the library, with amplitude cal applied when present). Spectral inversion is always undone inside, so no client ever conjugates.
- Every block carries `mmsdr_meta`:
  - `sample_index`, a count that includes dropped samples, so gaps are measurable;
  - flags: `DISCONTINUITY`, `RETUNED`, `GAIN_CHANGED`, `OVERFLOW`, `CLIPPED`, `SOURCE_GAP` (the fw ring stalls at 8 MHz);
  - the current freq, fs and gain;
  - `dbfs_to_dbm`.

  Today a client can't tell where in the stream a retune landed; metadata fixes that.
- **Flag accuracy.**
  - A reboot's `RETUNED` lands on the first sample of the new run.
  - A hop's lands on the first block that starts after the hop finished. That is block-accurate, not sample-accurate: the ~60 ms during the hop (sample clock gated, reader still pacing on time) arrive unflagged.
  - `SOURCE_GAP` is defined but not detected yet.
- Re-entrancy: any setter may be called from inside the callback.
  - Host-side changes (AGC, IQ, EQ, hop flag, CS16/CF32) apply at once.
  - Anything that touches the chip is posted to a library **control thread** and returns `QUEUED`, gain and DC mode included. The chip path can't run on the output thread: a reboot stops the very thread the callback runs on (deadlock), and even a gain write would stall delivery for the 20 ms DC re-hold. Back-to-back requests coalesce, so the newest value wins.
  - Blocking calls (`rx_stop`, `close`, `recal_dc`, `rx_start`, `boot`, every debug access including `allow_range` and `set_hook`) return `STATE` from inside the callback.
  - The first block at the new setting carries `RETUNED` or `GAIN_CHANGED`.
  - Precedent: rtl_tcp and Soapy hosts also retune from a command thread separate from the sample path.
- **Sync mode.**
  - `mmsdr_rx_read` never spans a meta change, so `meta` always describes sample 0 of what it returns. Host overflow drops the newest block and flags `OVERFLOW | DISCONTINUITY` (plus the dropped block's own flags) on the next one kept.
  - USB-side overflow (the reader's SPSC ring filling while the consumer stalls) is positioned: the core cuts the output block at the drop, so `OVERFLOW` sits on the block whose sample 0 follows the gap and `sample_index` jumps there, not on whatever block happened to be delivered when the drop was counted.
  - `rx_read` keeps waiting through a reboot-retune (`rx_is_streaming` stays 1); it returns `STATE` only after `rx_stop` or `close`. `close` wakes blocked readers and waits for them to leave before freeing the handle.
  - Overflow is reported only in meta; the return value is always the sample count.
  - The format is fixed while a sync stream runs, because the ring is sized in format units.

## Host-side DSP moving into the library

These lived in the SDR++ module (`main.cpp`) and are now in the library, so every client behaves the same:

- **Clip-guard AGC**: back off 2 steps on ≥8 clips per 250 ms, creep up 1 step after 5 s quiet and 15 s since the last back-off, never above the set index. Runs on the control thread.
- **Blind IQ imbalance correction.** Its image-rejection estimate is reported in `mmsdr_stats.iq_irr_db`.
- **Passband EQ**: a 31-tap inverse of the measured rolloff, boost capped at 10 dB.
- **DC residual tracker**, part of `dc_mode = hold`. The fw servo converges for 20 ms and is then frozen, which leaves a random offset; a block-mean tracker (tau ~0.2 s, fast re-acquire after each re-hold) removes it. Both old clients did this themselves.

AGC, IQ correction and EQ default to **off**. Each one has a typed getter/setter and a named setting, so any host can show it as a switch. The debug key `raw` bypasses all host DSP (`mmtest` sets it, so its numbers compare with the old driver).

The SDR++ module keeps its own UI defaults (AGC, IQ and EQ on, as before) and pushes them into the library.

## SDR enhancements (placeholders)

### Frequency calibration
- **Mechanism:** one crystal feeds the ADPLL, and both the LO and the ADC clock come from it. An XO error e therefore scales the LO and the sample rate together.
   - Since the ADC clock is VCO/128 and the resampler ratio is computed from the true f, correcting the synth word alone corrects fs too.
   - The reader's pacing divides by (1+e).
- **Implemented and verified** (2026-10-26, `mmsdr_rx -p`, the 851.4375 MHz carrier tuned to -437.5 kHz):
   - +10000 ppb moved it +8.503 kHz (expected +8.515), and -10000 moved it -8.449 kHz (expected -8.515). Both agree within one 122 Hz FFT bin, and a repeat at 0 reproduced to 3 Hz.
   - The readback stays at the requested LO to within the 1.9 Hz word step.
- **Gotcha found doing it:** a correction hop right after SET_CHANNEL is undone. The SYNTH word reads back nominal and the carrier doesn't move, because UPHY CFO-retunes until it parks. The library applies it after the park, where hops stick.
- **This board:** at ppb 0 the 851.4375 MHz carrier sits 563 Hz low, i.e. the XO runs about **+0.66 ppm**. That assumes the site is on frequency (P25 sites are usually GPS-disciplined), so `freq_correction_ppb ≈ +660` for this module. The earlier "-0.4 kHz" on 851.975 was read off the SDR++ display.
- **`mmsdr_cal_freq_measure`:** estimates ppb from a known carrier in the capture.
- **Parameter only, no file.** The correction is set via `open_args.freq_correction_ppb` or the setter, the same way rtl_sdr's `-p` works. A purpose-built dongle with this chip would carry a TCXO and run at 0.
- **`mmsdr_get_temperature`:** LPHY 0x3013 reads aux ADC ch 8; the scaling is unverified.

### Amplitude conversion (rough, built in)
This uses the rough numbers we already have, with no reference gear:

- **Gain:** `mmsdr_gain_db` maps an index to a nominal 3 dB/step (0 to 48 dB). That's the slope we saw in the noise floor tracking gain down to reduction 12. It returns `source = NOMINAL` so a client knows the figure is not measured. `mmsdr_gain_idx_for_db` gives Soapy-style dB clients the nearest index.
- **Passband:** `mmsdr_passband_db` returns the measured analog rolloff (verified on an LTE slice): 0 to -1 dB at ±1.5, -3 at ±3, -6 at ±4, -10 at ±5 MHz. The same curve drives the optional wide-mode EQ.
- **Absolute dBm** (`dbfs_to_dbm`) stays `NaN`/UNSUPPORTED until there's a reference. Adding a measure call later is a minor version, thanks to `struct_size`.

### Cal storage (deferred)
Not in v0.x: ppb is a plain parameter. These notes are for when measured tables exist.

How existing libraries do it:
- **[UHD](https://uhd.readthedocs.io/en/latest/page_calibration.html):** the library owns it. Data is keyed by cal type plus device serial and stored in `$XDG_DATA_HOME` (`~/.local/share/uhd/cal/`), overridable with `UHD_CAL_DATA_PATH`. It loads at device init, and an overwrite renames the old file to `.cal.$TIMESTAMP`.
- **[bladeRF](https://github.com/Nuand/bladeRF/wiki/DC-offset-and-IQ-Imbalance-Correction/2fb9f289bc0be5d51e16d2bdce3258d9ef3620cd):** `<serial>_dc_rx.tbl` tables, auto-loaded at open from a known location.
- **LimeSuite:** a per-user cache (`~/.limesuite/LMS7002M_cache_values.db`). [Stale entries there have caused bad RX](https://discourse.myriadrf.org/t/something-wrong-with-calibration-procedure/1163?page=2), fixed by deleting the file or passing `cacheCalibrations=0`.
- **librtlsdr:** none. The user passes ppm on every run.

**If we ever add it:** do it the UHD way, with Lime's lesson built in.
- Files are `<cal_dir>/<uid>_<kind>.cal`, with `cal_dir` coming from `open_args`, else `$MMSDR_CAL_PATH`, else `%LOCALAPPDATA%\mmsdr\cal` / `$XDG_DATA_HOME/mmsdr/cal`.
- They auto-load at open and back up on overwrite.
- `no_cal_autoload` is the escape hatch, and `mmsdr_cal_loaded_from` shows exactly what was applied, so stale data is never invisible.

## TX (placeholders only)

Every `mmsdr_tx_*` returns `MMSDR_ERR_UNSUPPORTED` in v0.x. The shape reserves the names so adding TX later won't break the ABI.

- **Ungated, like a signal generator.** There's no unlock, no callsign and no band policy. The only limits are what the hardware reports (`mmsdr_tx_get_freq_range`, `mmsdr_tx_get_power_range`), and going outside them returns `MMSDR_ERR_RANGE`. What gets transmitted and where is the operator's call, as with any instrument.
- **Path, from the static RE (not yet tested on hardware):**
  - `0x402070` analog enable;
  - a 2300 µs warm-up FSM, exposed as `mmsdr_tx_get_state`;
  - 32-bit I/Q words via DMA ch3 into the TX FIFO at `0x40135c`;
  - power from the BCF tables.
- **Underrun:** either end the burst or pad with zeros (`underrun_zero`).
- **`tx_stop`** is also implied by close and by any RX reboot, because RX and TX share the chip and a reboot resets both.

Bench note (ours, not the library's): our TX experiments wait until the LNA is off the port or a load is fitted.

## Expert interface (`mmsdr_debug.h`)

- **Register access:** `peek/poke` go through the reader pipeline, with the MMIO policy and an allowlist that an explicit `mmsdr_debug_allow_range` call can widen.
- **Policy, as implemented:**
   - Register classes (`< 0x10000` and `0x400000-0x40ffff`) cap reads at 60 B and must fall inside the allowlist. The default list holds exactly the blocks our port touches: RF/ADPLL `0x402000-7f`, DFE `0x4013xx`, gain `0x4030xx`, ring `0x40900x`, resampler `0x4094xx`, aux ADC, sems, OTP and host table.
   - fw RAM is open at any length: MAC `0x100000+192K`, `0x200000+704K`, PHY `0x500000-0x55ffff`.
   - Anything else returns `POLICY` until `mmsdr_debug_allow_range`.
- **`mmsdr_debug_set` keys** replace the env hooks:
  - `unclamp`, `nokdco`, `nohop`, `band54`, `cc`, `crashbcast`, `raw`;
  - the USB tuning keys `usb.*`.
- **Raw commands:** YAPS `mmsdr_cmd` and `mmsdr_generic_param` (0x003E). These need booted fw and no stream, so use `mmsdr_boot` or a hook. Direct LPHY messages (`mmsdr_lphy_msg`) are still a placeholder.
- **Boot hooks**, on the thread doing the reboot-retune:
   - `FW_LOADED`: fw up and patched.
   - `PRE_MEDIUM_EVAL`: tuned, generic params still accepted.
   - `TUNED`: after medium eval, UPHY still alive.
   - `RING_TAKEN`: parked, reader not started.

   `mmtest` keeps the GENPARAM sweep and the 0x811D cal experiments through them.
- **`mmsdr_debug_pll_state`** reads the ADPLL tracking word, KDCO, band, coarse bank and VCO (2 x synth readback).

`mmtest` is now a pure client of both headers. Its `MM_*` env vars map onto debug keys and settings.

## Mapping from today's class

| `MM8108` today | libmmsdr |
|:--|:--|
| ctor(fw, bcf, onIQ) | `mmsdr_open` + `mmsdr_rx_start{callback}` |
| `retune(f)` → string, `lastTuneWasHop()` | `mmsdr_set_freq(.., &res)` → error + `res.action` |
| `setWide(fs)`, `setPacked(b)` | `mmsdr_set_sample_rate`, `mmsdr_set_format(CS8)` |
| `setGain`, `setDcMode`, `recalDc` | same names, `mmsdr_` prefix |
| `takePeak`, `stats`, `dcEpoch` | `mmsdr_get_stats` (peak, dc_epoch inside) |
| `r32/w32` | `mmsdr_peek32/poke32` (debug header) |
| `MM_*` env vars | `mmsdr_debug_set` keys |
| AGC + IQ corr in `main.cpp` | `mmsdr_set_agc`, `mmsdr_set_iq_correction` |

## SoapySDR mapping (phase 2)

| Soapy | libmmsdr |
|:--|:--|
| `findSDRDevice` / args `serial=` | `mmsdr_list` / `open_args.select` (Soapy `serial` = our `uid`) |
| `getSettingInfo` / `writeSetting` | `mmsdr_list_settings` / `mmsdr_write_setting` |
| `setFrequency` / `setSampleRate` | `mmsdr_set_freq` / `mmsdr_set_sample_rate` |
| `setGain` (dB), "LNA" element | `mmsdr_gain_idx_for_db` (nominal 3 dB/step) |
| `setGainMode(true)` | `MMSDR_AGC_CLIP_GUARD` |
| `setDCOffsetMode` | `mmsdr_set_dc_mode` |
| `setFrequencyCorrection` (ppm) | `mmsdr_set_freq_correction_ppb` |
| `readStream` + flags/timeNs | `mmsdr_rx_read` + meta (`END_ABRUPT` on discontinuity) |
| `getNativeStreamFormat` | CS16, or CS8 when packed |
| `readSensor("temperature")` | `mmsdr_get_temperature` |

## Phases

1. **Library + C ABI on Windows. Done 2026-10-26.**
   - Moved `mm8108.cpp` behind `mmsdr.h`, added listing and typed errors, and turned the env hooks into debug keys.
   - Rebuilt the three clients on top.
   - Hardware A/B against the old DLL: see below. SDR++ GUI streaming checked 2026-10-29. Still open: the 12-boot wide-mode soak.
2. **SoapyMMSDR module.**
3. **CMake + Linux/NixOS.** Swap the Win32 thread priority and `timeBeginPeriod` calls, and add `libusb_detach_kernel_driver` for morse_driver.
4. **Calibration**: the rough amplitude conversion (built in, no gear), then frequency ppb from a known carrier. Absolute dBm waits for a reference.
5. **Timestamps.** Align the 1 MHz hwtimer to the ring pointer.
6. **TX** (bench: once the LNA is off or a load is on).

## Decisions (2026-10-26)

1. Prefix is `mmsdr_`.
2. TX is ungated; only hardware range checks apply.
3. AGC, IQ correction and passband EQ are off by default and visible as named settings.
4. Hops and reboots requested from the callback are queued to a control thread.
5. Amplitude means the rough built-in conversion (3 dB/step plus the measured rolloff); absolute dBm comes later.
6. ppm/ppb is a parameter only; file-based cal storage is deferred.
7. `uid` is the factory MAC from OTP, as 12 lowercase hex digits (e.g. `0cbf7400329f`).

## Unique ID (verified on hardware 2026-10-26, `tools/re/uidprobe.py`)

**The USB iSerial is useless as an ID.** It is `MM0000000123`, a constant in the mask ROM (string table at 0x310c74, right after "MM81xx Wi-Fi HaLow 802.11ah Transceiver"). Windows uses it verbatim as the instance ID `USB\VID_325B&PID_8100\MM0000000123`, so it can't tell two modules apart.

**The factory MAC in OTP is the ID.** I read the physical OTP with the `mm81xx_otp_read` sequence on a freshly reset chip, twice, with identical results:

| bank | word | meaning |
|:--|:--|:--|
| 0 | `0x00000010` | unknown |
| 5, 6 | `0x688bce0f`, `0x0075aeb9` | unknown; possibly a die/lot ID (assumed) |
| 25 | `0x25420000` | country code `[15:0]` = 0 (unset); upper half unknown |
| 26 | `0xbf0c0807` | board type `[15:0]` = `0x0807` (matches the known Etna value); bytes 2-3 = `0c bf` |
| 27 | `0x9f320074` | bytes `74 00 32 9f` |
| all others 0-31 | 0 | ECC and raw reads agree everywhere |

- **The banks form a MAC.** Bank 26 bytes 2-3 followed by bank 27 give `0c:bf:74:00:32:9f`.
- **The firmware uses that same MAC.** After boot it publishes `0c:bf:74:00:32:9f` at extended host table +4 (`0x11fd00`), so it takes its MAC from OTP.
- **The prefix is Morse's.** [`0C:BF:74` is Morse Micro's registered OUI](https://maclookup.app/macaddress/0cbf74).
- **Uniqueness per chip is assumed**, as with any factory MAC; we only have one module to compare.

**How the library gets it:**
- If the firmware is running, read it from the extended host table (read-only).
- Otherwise use the OTP sequence: power up `0x4010/14/18`, read enable `0x401c/20`, then for each bank set PA `0x4034` and ECC `0x4048`, start with `0x400c` bit 0, poll it, read `0x4040`, and power down.

`mmsdr_list` can therefore fill `uid` for idle devices. For devices busy in another process it can't, because reading would need the USB handle.

## Phase 1 hardware verification (2026-10-26)

A/B against the pre-library binaries (`mmtest` with `raw`, so both see identical samples), same module, same antenna:

| Check | Old driver | libmmsdr |
|:--|:--|:--|
| Boot + stream at 851.975 MHz | 844 ms, 0 laps | 692 ms, 0 laps |
| Hop 851.975 -> 1000 MHz | 57.6 ms | 54.9 ms |
| Power 851.975 / 1000 MHz | -23 to -27 / -40.5 dBFS | -23 to -26 / -40.5 dBFS |
| Average fs after hop (5 s) | 8.00001 MS/s | 7.99997 MS/s (2 ms block quantization) |
| Wide 9.7 MS/s at 881.5: ratio 0x409404 over 3 s | b5c0a8e8, held, parked | b5c0a8e8, held, parked |
| Wide hop 881.5 -> 950 | 59.2 ms, 0 laps | 59.2 ms, 0 laps |

New paths (no old equivalent):

| Check | Result |
|:--|:--|
| Packed CS8 (`MM_PACK`) | 8.0 MS/s, 0 laps, packer heartbeat alive |
| Sync `mmsdr_rx_read`, 16M samples | 971 reads, contiguous `sample_index`, 0 overflow |
| `rx_reentry`: setters from the callback | 7/7 PASS (set_freq QUEUED, set_gain queued, rx_stop and peek STATE, RETUNED with 870.5 MHz in meta, GAIN_CHANGED, contiguous index) |
| `uid_paths`: host table vs OTP | `0cbf7400329f` on both paths (OTP forced by clearing `0x2d40`) |
| ppb +-10000 | carrier moves +8.503 / -8.449 kHz (expected +-8.515) |
| `mm_rtltcp` as rtl_tcp client | header R820T/17 gains, 8.0006 MS/s, tune 0x01 hops, ppm 0x05 (new) applies, gain 0x04 30 dB -> idx 10 |
| SDR++ module | deployed (old copy `mm8108_source.dll.bak-prelibmmsdr`), loads with all imports resolved; GUI streaming verified 2026-10-29 |

Not a regression, noted: `0x401334[3:1]` read 2 on one boot and 4 on another, under both binaries. The fw writes it; we only touch bits 0 and 4.

### Re-verification 2026-10-29 (pre-release review fixes, ELF-derived patch table)

| Check | Result |
|:--|:--|
| `rx_reentry` (now 9 checks: adds `mmsdr_list` while streaming) | 9/9 PASS; own device listed busy with uid, stream untouched |
| Boot with stock words from the ELF, `STOCK_SIG` match | 709 ms at 1000 MHz (div2 NOPs applied, so the LO locked) |
| Hop 1000 -> 851.975 MHz | 87 ms, 0 laps, hop mark carries the new freq |
| Packed CS8 | 8.0 MS/s, packer heartbeat alive, 0 laps |
| `recal_dc` after a hop | 905 MHz: ok in 64 ms, DC -96 dBFS after. 870.5 MHz (cellular downlink): fw reports cal failure and leaves clipping, same as before; a quiet channel is required |
| SDR++ GUI streaming | verified by Taylor on the 2026-10-26 build; the 2026-10-29 build is deployed, GUI recheck pending |
