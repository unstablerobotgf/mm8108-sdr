# MM8108 PHY RE index

DB: `rom/mm8108_phy.elf.bndb` (built from `rom/mm8108_phy.elf` by `tools/mkphyelf.py`). Conventions: `BRIEF.md`.
All region findings are static analysis unless a report says "verified on hardware".

| Region | Range | Report | Named |
|:--|:--|:--|:--|
| UPHY RAM low | 0x500000-0x507fff | uphy_ram_lo.md | 166 fn |
| UPHY RAM high + AON | 0x508000-0x50fb9e, 0x538000-0x53a850 | uphy_ram_hi.md | 131 fn, 50 data |
| UPHY ROM lib | 0x520000-0x52bfff | uphy_rom_lo.md | 206 fn, 16 data |
| UPHY ROM app (dead fallback) | 0x52c000-0x537fff | uphy_rom_hi.md | 146 fn, 22 data |
| LPHY RAM + tables | 0x540000-0x546ab2, 0x55c000-0x55d280 | lphy_ram.md | 142 fn, 66 data |
| LPHY ROM (lib + dead fallback app) | 0x550000-0x559fff | lphy_rom.md | ~190 fn, 41 data |

## Layout in one paragraph
Each PHY ROM = shared `morse_libc_rom.c` runtime (UPHY 0x520000-0x524f80 == LPHY 0x550000-0x552c00, same order)
then UPHY-specific DSP/coprocessor accessors (UPHY only), then a complete 2024-08-27 fallback firmware app that
the RAM images never call. RAM installs hook tables the ROM calls through (UPHY 0x51fe80, LPHY 0x54fe80).
UPHY uses custom-1 (opcode 0x2b) instructions for the OFDM engine; Binja cannot decode them.

## Cal re-trigger facts (for post-hop DC/IQ cal)
- Host cmd 0x811D (handler 0x505b50, checked by hand): payload u32 mask (b3 = DC 0x3017, b4 = IQ 0x300d), then
  u8 run != 0. It first writes PSM trigger 0x408038 = 1<<23. Path: uphy_cal_run_mask 0x505784 ->
  uphy_dccal_request_3017 0x5064a8 (disables RX, reprograms 0x409034/0x401924/0x401920, captures into the stock
  ring 0x5141b0, sync-sends 0x3017, clears 0x40904c b1; on failure retries via timer event 8 after 15 ms).
- Caveat: while streaming, UPHY is parked in the ring-take ASSERT, so it cannot service 0x811D. The live route is
  then LPHY's inbox directly: slot 0x54ff80 {u32 type, u32 len, payload <= 0x38}, lock 0x1d3c, doorbell
  0x406004 b0; completion sets 0x510000 b5. 0x3017 payload = 13 words (see lphy_ram.md).
- Hazards: cal reprograms the capture ring (host must re-take it); strong signals abort the sweep (> about
  -75+3g dB); 0x300d retunes to UPHY's stale channel 0x538e58 unless updated.
- Crash forensics: UPHY panic dump in AON 0x5380d4 (0xfee1dead marker) .. 0x538494; LPHY 0x55c084..0x55c10c.

## MAC/host side
DB: `rom/mm8108_mac.elf.bndb` (from `rom/mm8108_mac.elf`, built by `tools/mkmacelf.py`): firmware segments below
0x500000 + main ROM 0x2c0000 (host CPU to 0x329000, MAC CPU after; assert paths intact) + BCF at 0x11c680 + NOBITS
register windows 0x1000-0xffff and 0x400000-0x40ffff. Only `rom_power_on_pll_bringup` 0x2c0134 writes RF 0x4020xx
(boot ADPLL at VCO 1900 MHz); no RF filter trim on the MAC/host side. `mac/calibration.c` only schedules airtime.

## Gap closures 2026-10-25 (static)
- LPHY hook slots 1-4 (RAM 0x541774/84/8c/7c) are stubs too (`c.li a0,0; ret` / `ret`): unused in this build.
- 0x402070 = TX analog control. `uphy_txa_power_on` 0x503e50 (b9 master, [17:14]=0xd, b13/b21, 0x402074 |= 0xd3,
  0x405038 b24) + warm-up FSM 0x503d38..0x503fa4: off -> warming 2300 us -> ready (b19 set, b13 clr; inferred);
  driven from tx_prepare/tx_task/rx_return_idle/phy_init. b0 = TX datapath enable (real TX and cal loopback).
- TX sample path (back pocket; TX ON HOLD while the LNA is on the RF port): `uphy_tx_encode_chunk` fills ping-pong
  buffers 0x5136cc / 0x513c1c (guard 0x5eadbeef at +0x54c), `uphy_tx_dma_push` DMA ch3 -> 0x40135c (0x401324 if
  0x2d20 b12) as 32-bit I/Q words; LPHY's IQ-cal tone also writes 0x40135c. `uphy_tx_prepare` clears the RX ADC
  codes (0x402000), `uphy_tx_done` restores them. PHY init sends 0x300d (loopback IQ cal) on every boot.
- Platform 0x2d20 = 0x809 on our chip: [13:12] = 0 (normal silicon, not the FPGA path).
