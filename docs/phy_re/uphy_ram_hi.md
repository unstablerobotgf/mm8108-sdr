# UPHY RAM upper half (0x508000-0x50fb9e), UPHY AON (0x538000-0x53a850), UPHY DMEM globals

Region agent report, 2026-10-25. Binja DB handle `1fd851abcb69f454:ELF` (not saved; coordinator saves).
131 functions named, 50 data symbols defined, 26 comments. Confidence: H = read in full from code and
registers, M = role clear but semantics partly inferred, L = structural guess.
"Verified" below means verified by reading code in this DB, not on hardware, unless stated.

## 1. Subsystem overview

UPHY is a **software OFDM baseband**. Hardware only detects packets, estimates nothing, and DMAs complex
samples into the capture ring; UPHY RAM code does CFO estimation, FFT/symbol decode, SIG parse, and
hands a result descriptor to the MAC. TX is also symbol-built in software (scrambler/encoder/IFFT via ROM
helpers plus an FFT command engine).

Top level (main loop `0x5001e8`, other region) per iteration:
`uphy_tx_task 0x5096d8` then `uphy_rx_task 0x509424`, then timers 1..9 (`0x50430c(n)`), mailbox
(`0x5022a0/0x502208` then host cmd handler `0x50583e`), then `wfi`. Loop exits when PSM state == 0xe.

Interrupt handlers registered in `0x506144` via `0x50251c(irq, fn)` (verified):

| IRQ | handler | role |
|---|---|---|
| 0x3 | 0x502f1a (other region) | |
| 0xa | 0x505780 (other region) | |
| 0xb | 0x507d14 (other region) | |
| 0xc | `uphy_irq12_pktdet_handler 0x5082ac` | packet detect: 0x507dbc -> `uphy_rxcap_on_detect`, refresh PSM cache, cancel timers 3/6, clear event 2 |
| 0xd | `0x5085c0 -> uphy_irq13_psm_handler 0x5084c4` | PSM state 1: reset capture block via ROM 0x522710(0x5141b0, 0x538e58) + 0x409000 b0 pulses; state 5: latch 64-bit timestamp 0x40905c/60 |
| 0xf | 0x507d04 (other region) | |
| 0x13 | `uphy_irq19_rssi_handler 0x508008` | PSM state 1/0xa: RSSI 0x401934 + gain idx to dBm (0x507936), threshold, 0x401904 b1..3 |
| 0x1d | 0x505f40 (other region) | |

### 1.1 Capture ring and RX path (priority 1, H)

Hardware block 0x409000 (verified register use):
- `0x409044` ring base, `0x409048` length (samples), `0x40904c` b0 ring enable, **[17:2] = HW-latched ring
  index at packet detect** (read at 0x509508 as `(0x40904c>>2)&0xffff`, only ever masked by firmware:
  `&= 0xfffc0003` in `uphy_rxfe_config_bw`). This explains the "RING_CTRL field bits [17:2] take values
  never written by code" observation in the bench notes: it is a status field.
- `0x409050` free-running sample (write) counter; firmware **zeroes it** in `uphy_rxcap_on_detect` (0x50a8f2).
- `0x409064` sample-count threshold IRQ arm: `((n<<12)&0x1ff000) | 0x3fe00002` (0x50889c).
- `0x40906c` HW write pointer (address). `0x40905c/0x409060` 64-bit timestamp (IRQ13 state 5).
- `0x409030[19:16]` detected bandwidth/mode on packet detect; `0x409020/0x409024` detector status/metric;
  `0x4090bc..` 32-sample snapshot window (I lo s16, Q hi s16).
- `0x409014[3:0]` scale (also used as autocorr shift), `[30:27],[26:23],[22:19]` per-BW gains.
- `0x409204..0x409224` nine identical detector thresholds (clamped 0x3e..0x3e8, 0xfa for 8 MHz).

Flow (H unless noted):
1. `uphy_rxfe_config_bw 0x50adec` (from 0x502dfc) programs the detector per BW index 0..3 (1/2/4/8 MHz)
   and sets ring base/len from its args.
2. IRQ12 -> `uphy_rxcap_on_detect 0x50a6bc`: reads BW 0x409030[19:16]; picks output rate
   (1/2/4/8 MHz constants 0xf4240..0x7a1200) and per-BW 0x409000/0x40901c/0x409018 fields (two jump
   tables at 0x539fa8/0x539fcc); lag-1 autocorrelation over the 0x4090bc window; if `0x50369c()` then
   **retunes the LO** via `uphy_set_lo(s5, cfo*0x26db2>>(16-bw), 1)` else writes 0x40131c (DFE freq
   correction, M); then capture takeover: `0x409050=0`, `0x40904c|=1`, `0x409000|=0x1001`,
   `0x401308|=6`. This is the routine Taylor already patched out (jal at 0x507e1a) to stop LO blips.
3. `uphy_rx_task 0x509424`: on event 3, start index = latched ring index + `uphy_rx_delay_lookup`
   (ROM table 0x52b1c0 or AON 0x539ff0..) -> `uphy_rx_start_idx 0x5104b0` and `uphy_rx_read_idx 0x510530`.
   (Memory note called 0x5104b0 a "gate byte/state counter"; it is this start index, H.)
   Then either `uphy_rx_sync_start 0x5085f0` (CFO via `uphy_rxcap_cfo_autocorr 0x50b840`, rx ctx init) or
   `0x504e44`. While PSM state is 7 or 9: `uphy_rxcap_wait_samples` + `uphy_rx_process_ring`.
4. `uphy_rx_process_ring 0x508940` feeds the ring (0x5141b0, 0x708 samples, wraps) into
   `uphy_rx_symbol_step 0x50d5a0` (ctx `uphy_rx_ctx 0x515dd0`, 0x744 B), packs bits to
   `uphy_rx_bitbuf 0x5122d0` (0xfa0 B circular), fills `uphy_rx_result_desc 0x514170` and writes its address
   to `0x408078` (MAC-side), PSM transitions 0x30/0x31/0x32.
5. `uphy_rx_stop 0x50935c`: clears 0x401314 b0/b1, 0x403000 b5, 0x409000 b0, 0x401308 b1, pulses
   0x409400 b5 around ROM 0x52102c. `uphy_rxcap_disable 0x5085c8`: 0x40902c=1, ring off.

### 1.2 The ring-take ASSERT at 0x50e502 (priority 1, H)

0x50e502 is the **entry of `uphy_meas_capture_fft_loop`**, not of the assert itself. Call chain:
main loop timer 7 (1 ms re-arm, only if byte 0x5104ac != 0) or timer 2 (PSM state 1) ->
`uphy_meas_spectrum_task 0x509db6` -> `uphy_meas_spectrum_run 0x50e6fa` -> `uphy_meas_capture_fft_loop`.
For each of N averages it calls `uphy_rxcap_set_ring_checked 0x50c6b0`(ring, FFT size), which disables the
ring (0x40904c b0), writes base/len, and **requires `0x40906c == base`**; otherwise it returns 0xffff8000
and the loop calls `ASSERT("332c19ed9a", 0x62)` at 0x50e554. On success: ROM 0x521124 (wait) and ROM
0x527c2c (FFT accumulate into ctx+0x14). Afterwards 0x50e6fa restores the stock ring with
`uphy_rxcap_set_ring(0x5141b0, 0x708)`. Result (0xa8 B: noise floor, peak bins, etc., M) goes to
`uphy_meas_result 0x538ce8` (AON), timestamp at 0x538d84.
So the assert fires whenever the host owns the ring and the HW pointer is not parked at the base the
firmware just wrote. Patching 0x50c6d0 (the bne) is exactly the minimal fix Taylor used (v4).

### 1.3 Assert / panic machinery (H; code is in the other half, data in AON)

- File ids are 10-hex-digit strings in AON 0x5391a4..0x5393c8 (e.g. "332c19ed9a" = measurement file,
  "2498a227df" = rx_process_ring file, "f3bf57c914" = tx task, "1c1ac2dd5d", "a515555404" = AGC DMA,
  "6bc54700d0" = rxfe config, "2cba7dd694" = DMA, "d742eb1a92" = AGC thresholds, "3144100dac" = rx symbol).
- `0x501f00 ASSERT(file, line)`: csrrc mie 0x808; trace(0xa5); `0x502670` (stop/flag other cores via
  0x4100/0x4104 b0, 0x406004 b0); line -> **0x538164**, last 32 chars of file -> **0x538168**;
  printf "\nassertion %s:%d\n" (0x539288) via 0x501e7c (UART, dead on Etna); then `0x502384`.
- `0x501f68 ASSERT_PC(pc)`: same but "\nassertion pc:0x%08lx\n", pc to 0x538164 (in hook slot +0x14).
- `0x502384 panic`: clears mstatus/mie/mip bits, 0x406000 b0 = 0, saves all regs to **0x5380d8..0x538150**,
  pc marker 0x538154, mepc 0x538158, mcause 0x53815c, then writes **0xfee1dead to 0x5380d4**, fence.i,
  `wfi` loop. All in AON, so a host mem_read of 0x5380d4..0x5381a0 after a hang gives magic, regs and the
  assert file/line without a UART.
- Trace log `0x5027c4(code, arg)`: 64-entry ring at **0x538194**, 12 B {time 0x406040, code, arg},
  index at 0x5103ac. Codes seen: 3 (PSM state), 0x2b/0x2e/0x38/0x39/0x45/0x47/0x48/0x94/0x9a/0x9e/0xa0/0xa5.

### 1.4 PSM (PHY state machine) (H for mechanism, M for state meanings)

HW at 0x408000: `[10:7]` state, `[14:11]` sub-state, `[19:15]` aux; `0x408038` = trigger (1<<bit);
`0x408058` b2 = error (history dump + assert); `0x408078` RX result desc ptr; `0x408080` TX result desc
ptr; `0x40807c` TX request desc from MAC. Firmware caches the state in DMEM `uphy_psm_state_cache
0x510578` {u32 sub, **u32 state 0x51057c**, u32 aux} with a shadow copy at 0x510584.
Transition table `uphy_psm_transition_table 0x5394ec`: 12 B entries {expected new state, trigger bit,
aux}; `0x5015dc(idx)` fires it and checks the new state. 57 entries decoded (see section 4).
States (M): 1 idle/listen, 7 RX preamble, 9 RX data, 0xa/0xb TX prepare, 0xc TX active, 8 sleep-ish,
0xe shutdown (main loop exit), 2/3/4/5 other.
The "PSM_ST_L 0x%x PSM_ST_H 0x%x PSM_TR_L ... DCF_CF 0x%x" format at 0x539214 has **no code xref**
in RAM or ROM (verified by xrefs); likely dead or referenced via a computed pointer.

### 1.5 Resampler, FEM, AGC, DC servo (M)
- `uphy_set_resamp_ratio 0x50c620` (pre-named) is called only from 0x504e44.
- `uphy_fem_set_state 0x50ce18`: cfg struct (0x510050). State 0: pins cfg+0x28; 1 (RX): pins cfg+0x24,
  and if cfg+0x36 and arg3: **pinmux 0x2008/0x200c switched to alt function 2 (0x12492492 pattern),
  0x403018 b22 set, 0x403018[1:0] = [5:4]**, i.e. AGC hardware drives the FEM pins during RX (inferred).
  This may explain why software pin toggles had no RF effect. 4: pins cfg+0x22; 5: cfg+0x20; 2/3/6
  bookkeeping only. GPIO set/clear at 0x1368/0x136c.
- AGC: `uphy_agc_pick_gain_idx`, `uphy_agc_calc_thresholds`, `uphy_agc_set_401928_lo/hi` (0x401928 16-bit
  fields), `uphy_agc_set_403030_target` (0x403030[30:28], 0x403034[15:0] from AON tables 0x53a1e0/0x53a1b4),
  `uphy_agc_load_gain_tables` (5 x 0x20 B into 0x403040..0x4030c0), DMA of 0xfc B to 0x4030e0 and 0x150 B
  to 0x403200. Gain index range 0..0x14 (21 entries, matches LPHY table). Thresholds in dBm (-85, -55).
- RSSI: `0x401934[10:0]` in 1/8 dB, dBm = ceil(x/8) - 133 (`uphy_rssi_read_dbm`); `uphy_rssi_median8`.
- DC servo 0x401334: `[10:8]` and `[4:1]` params, b5 hold, b0 enable; 0x401314 b0/b1 + [11:2] window
  (`uphy_dc_servo_*`).

### 1.6 Other infrastructure (H)
- **ROM patch unit** (`uphy_rom_patch_apply 0x538010`, AON): pairs at 0x538000 -> 0x406300 (ROM addr),
  0x406304 (JAL x0 opcode to RAM target), 0x2000 b23 gate. Patches: ROM 0x520c1c -> 0x50a39c,
  ROM 0x520e0c -> 0x50a3bc (both semaphore locks). Same mechanism could patch any ROM address.
- **ROM hook table**: `uphy_rom_hooks_install 0x53809c` copies 0x539ea8 (0x100 B) to DMEM **0x51fe80**.
  Slots: +0x00 0x501e2c, +0x04 0x501fa0, +0x08 0x501fb0, +0x0c 0x501fb8, +0x10 0x501fa8, +0x14 0x501f68
  (assert_pc), +0x18 0x501e74, +0x20 0x5028bc, +0x24 0x5027c4 (trace), +0x28 0x502908, +0x2c 0x501484
  (LPHY msg send, M), +0x30 0x50154c, +0x38 0x5027bc, +0x60 0x50a0e4 (fft desc), +0x64 0x503ff8,
  +0x68 0x502bcc, +0x6c 0x502bd4, +0x70 0x502d2c, +0x74 0x502d86, +0x78 0x50369c, +0x7c 0x5011ac
  (uphy_set_lo), +0x80 0x503fcc, +0x84 0x509f68, +0x88 0x50a0c0, +0x8c 0x50113c.
- HW semaphores: amoswap spinlocks at 0x1d10, 0x1d1c+4n, 0x1d2c, 0x1d30+4n, 0x401a00; bit locks on
  0x1d00 (b0, b2 via amoor/amoand); 0x401a04 atomic counter.
- DMA: 4 channels, ch0 0x1a00, ch1 0x1b00, ch2 0x401700, ch3 0x401800: +4 src, +8 dst, ctrl [23:0] len,
  b24 go, b26; +0xc busy; +0x10 extra. 0xd000/0xd100/0x408400 channel ids assert/forward to ROM.
- Command/FFT engine at 0x3800: 0x3804 descriptor ptr, 0x3808 = 0xe then b0 go, 0x380c b0 busy, b5 ack;
  descriptor ring 0x51665c, descriptor 0x51667c; commands `(len<<16)|0x2fd` / `0x2fe`.
- **Custom ISA**: UPHY uses RISC-V custom-1 (opcode 0x2b) instructions (e.g. 0x50d34c `0x70e787ab`,
  funct7 0x38 polled until a status value). Binja cannot decode them, so 0x50d348, 0x50eae4, 0x50eb2c,
  0x50ec64.., 0x50eff8..0x50fac3 have broken boundaries; that range is the FFT/DSP driver (M).
- Stats: `uphy_stats_id_table 0x5399f4`, 139 x {addr, u16 id, u16 size} for IDs **0x3001..0x308b** ->
  AON `uphy_stats 0x5389f8..` (looked up by 0x5041c4). Most `data_538axx = 1` writes are these stats.
- Event flags `uphy_evt_flags 0x510000` (atomic bit ops `uphy_evt_*`): bit 2 cleared by IRQ12,
  3 = RX samples ready (set IRQ13), 4, 5 (LPHY reply wait in `uphy_lphy_call_sync`), 7 (msg 0x5002),
  8, 9 (0x200 TX abort).
- Version "rel_mm8108_2_1_0_2026_Jul_01" at 0x5397b4, build date "2026-07-01 12:29" at 0x5397a0.

## 2. Function table

| addr | name | role | evidence | conf |
|---|---|---|---|---|
| 0x508008 | uphy_irq19_rssi_handler | IRQ 0x13: RSSI/CCA when idle/TX-prep | reg 0x50251c(0x13), 0x401934, 0x403004, 0x401904 | M |
| 0x5082ac | uphy_irq12_pktdet_handler | IRQ 0xc: packet detect | 0x50251c(0xc), calls 0x507dbc | H |
| 0x508438 | uphy_rx_timeout_cb | RX timeout: deadline +30 ms, rx stop | no xref; 0x7530, 0x50935c | L |
| 0x5084c4 | uphy_irq13_psm_handler | IRQ 0xd: PSM state 1/5 actions | 0x50251c(0xd), 0x408000[10:7] | M |
| 0x5085c8 | uphy_rxcap_disable | capture off | 0x40902c, 0x40904c b0, 0x409000 b0 | H |
| 0x5085f0 | uphy_rx_sync_start | rx ctx init + CFO estimate | 0x50d9f4, 0x50b840, asserts 0x106 | M |
| 0x50889c | uphy_rxcap_wait_samples | spin/IRQ until 0x409050 >= need | 0x409050, 0x409064 | H |
| 0x508940 | uphy_rx_process_ring | software RX ring consumer | 0x5141b0 % 0x708, 0x408078 | H |
| 0x509280 | uphy_rx_set_enable | 0x401904 b0, 0x403000 b0/b5 | regs | M |
| 0x5092a8 | uphy_rssi_median8 | 8 RSSI reads, insertion sort, median | 0x401934 x8 | H |
| 0x50935c | uphy_rx_stop | stop RX datapath | regs list 1.1 | H |
| 0x5093c4 | uphy_rx_arm_timeout | deadline +30 ms, 0x408034 b0 clr | 0x7530, 0x502b50 | M |
| 0x509424 | uphy_rx_task | RX top-level per loop | main loop, evt 3, PSM 7/9 | H |
| 0x5095e4 | uphy_chan_tone_mode_update | channel-dependent 0x40132c mode | ch ranges 11/20/30, 0x7000 | L |
| 0x5096d8 | uphy_tx_task | TX top-level (PSM 0xa/0xb/0xc) | 0x40807c, 0x408080, asserts | H |
| 0x509ca0 | uphy_noise_avg_update | moving avg of 0x401998 | 0x50e296 | M |
| 0x509ce8 | uphy_rssi_read_dbm | RSSI -> dBm | /8 - 133 | H |
| 0x509d6a | uphy_linkq_update | MCS/rate pick into 0x538cd0 | 0x50e0c4 | L |
| 0x509db6 | uphy_meas_spectrum_task | measurement wrapper | 0x50e6fa, 0x538ce8 | H |
| 0x509ee4 | uphy_agc_idle_update | idle AGC reset/RSSI/noise | 0x403004, 0x52102c | M |
| 0x509f68 | uphy_fftq_desc_init | init descriptor (0x51665c/0x51667c) | hook +0x84 | H |
| 0x509f88 | uphy_fftq_submit | submit to 0x3800 engine | 0x3804/08/0c | H |
| 0x509fdc | uphy_fftq_cmd_2fe | command (len<<16)\|0x2fe | | M |
| 0x50a044 | uphy_fftq_cmd_2fd | command (len<<16)\|0x2fd | used with 0x5122d0/0xfa0 | M |
| 0x50a0ac | uphy_fftq_wait_idle_lock | wait 0x380c idle then lock 0x1d10 | | H |
| 0x50a0c0 | uphy_fftq_desc_append_op | append 0x19 or 0x1225 word | hook +0x88 | L |
| 0x50a0e4 | uphy_fftq_get_desc | returns 0x51667c | hook +0x60 | H |
| 0x50a0f0 | uphy_rmw_405020_b13_clr | 0x405020 b13 clr + b0 pulse | | L |
| 0x50a124 | uphy_rmw_405020_b13_set | 0x405020 b13 set + b0 pulse | | L |
| 0x50a158 | uphy_dma_wait | wait DMA ch | 0x1a0c/0x1b0c/0x40170c/0x40180c | H |
| 0x50a218 | uphy_dma_start | start DMA ch 0..3 | src/dst/len regs | H |
| 0x50a360 | uphy_hwsem_lock_1d2c | spinlock | amoswap | H |
| 0x50a37c | uphy_hwsem_lock_1d30_n | spinlock n | amoswap | H |
| 0x50a39c | uphy_hwsem_lock_1d1c_n | spinlock n (ROM patch target) | 0x538000 | H |
| 0x50a3bc | uphy_hwsem_lock_1d10 | spinlock (ROM patch target) | 0x538000 | H |
| 0x50a3d8 | uphy_hwsem_lock_401a00 | spinlock | amoswap | H |
| 0x50a3f8 | uphy_hwsem_unlock_401a00 | unlock | amoswap 0 | H |
| 0x50a40c | uphy_hwsem_lock_1d00_b0 | bit lock | amoor | H |
| 0x50a420 | uphy_hwsem_read_1d00_b0 | read bit | | H |
| 0x50a42c | uphy_hwsem_lock_1d00_b2 | bit lock | amoor | H |
| 0x50a440 | uphy_hwsem_unlock_1d00_b2 | bit unlock | amoand | H |
| 0x50a450 | uphy_atomic_inc_401a04 | amoadd 1 | | H |
| 0x50a464 | uphy_fx_log2_floor | floor log2 | | H |
| 0x50a480 | uphy_fx_pow_to_db_approx | ~3*log2 coarse dB | | M |
| 0x50a4fc | uphy_rxfe_set_409084_nibbles | pack 8 nibbles to 0x409084, 0x409080=0x1ff | | M |
| 0x50a56c | uphy_rx_phase_thresh_init | pi/2 Q16 thresholds per mod type | 0x1921f | L |
| 0x50a65c | uphy_rx_delay_lookup | per-BW sample delay table | 0x52b1c0, 0x539ff0.. | M |
| 0x50a6bc | uphy_rxcap_on_detect | detect -> CFO -> capture takeover | see 1.1 | H |
| 0x50adec | uphy_rxfe_config_bw | per-BW detector + ring setup | 0x4090xx, 0x409204.. | H |
| 0x50b840 | uphy_rxcap_cfo_autocorr | ring autocorr CFO, 160 us timeout | 0x409044/48/50, pi 0x3243 | H |
| 0x50ba3c | uphy_rx_calc_sym_offset | offset 0xa0/0x50 + table | 0x53a82b | L |
| 0x50ba8c | uphy_rxfe_set_scale_409014 | 0x409014[3:0], 0x409088 | | M |
| 0x50bb48 | uphy_set_40132c_tone_mode | 0x40132c b0/b1, 0x40205c, 0x401358 | | L |
| 0x50bc6c | uphy_set_409204_thresholds | 9 thresholds | clamp 0x3e/0xfa..0x3e8 | H |
| 0x50bcb8 | uphy_cal_409204_thresholds | avg 9 x 0x409024 then set | | M |
| 0x50bde2 | uphy_rf_gain_mode_update | switch 0x402028/30 codes | | L |
| 0x50be26 | uphy_agc_pick_gain_idx | dBm -> gain index 0..20 | | M |
| 0x50bf02 | uphy_agc_calc_thresholds | CCA thresholds, assert 0xde | | M |
| 0x50bfa8 | uphy_agc_gain_to_offset | gain idx -> dB offset (3 dB/step) | | M |
| 0x50bfd8 | uphy_agc_set_401928_hi | 0x401928[31:16] | | M |
| 0x50c044 | uphy_agc_thresh_to_lin | dB thr -> linear | 0x50e300 | M |
| 0x50c080 | uphy_agc_set_401928_lo | 0x401928[15:0] | | M |
| 0x50c1ae | uphy_agc_set_403030_target | 0x403030[30:28], 0x403034[15:0] | | M |
| 0x50c254 | uphy_dc_servo_set_params | 0x401334 [10:8],[4:1] | | H |
| 0x50c294 | uphy_dc_servo_restart | servo restart + window | 0x401314/34/40 | H |
| 0x50c324 | uphy_dc_servo_config | same, no restart | | H |
| 0x50c3a0 | uphy_dc_servo_restart_cond | conditional restart | | M |
| 0x50c3e4 | uphy_set_40947c | store | | H |
| 0x50c3f0 | uphy_set_4094b0 | store | | H |
| 0x50c3fc | uphy_rx_coef_reset_409558 | identity coefs 0x80008000 | | M |
| 0x50c468 | uphy_load_4013a8 | 16 B copy or zero | | H |
| 0x50c494 | uphy_load_409480 | 16 B copy or zero | | H |
| 0x50c4c0 | uphy_rx_coef_load_4094b4 | 0xa0 B to 0x4094b4 + 0x60 B to 0x409558 | | M |
| 0x50c514 | uphy_set_401390_mode | 0x401390[1:0] | | H |
| 0x50c52c | uphy_load_401394_pair | 0x401394/98 from table | | M |
| 0x50c548 | uphy_bwcfg_lookup | per-BW table value | | L |
| 0x50c588 | uphy_bwcfg_apply | apply per-BW descriptor list | 0x539848 | M |
| 0x50c684 | uphy_rxcap_set_ring | ring base/len, unchecked | 0x409044/48 | H |
| 0x50c6b0 | uphy_rxcap_set_ring_checked | ring setup + ptr==base check | 0x40906c | H |
| 0x50c700 | uphy_rf_set_402028_b16 | 0x402028 b16, 0x402020 b28 | | H |
| 0x50c728 | uphy_rf_set_402000_enables | 0x402000 b0..2 etc. | | M |
| 0x50c784 | uphy_rf_set_402078_mask | 0x402078[20:0] all/none | | H |
| 0x50c7a4 | uphy_rf_set_gain_codes | 0x402028/2c/30 from 0x53a174.. | | M |
| 0x50c860 | uphy_set_nco_slot | 0x40940c+8n slots | jt 0x53a160 | M |
| 0x50c9c0 | uphy_agc_load_gain_tables | 5 x 0x20 B to 0x4030xx | | M |
| 0x50ca30 | uphy_agc_dma_table_4030e0 | DMA 0xfc B | | H |
| 0x50ca74 | uphy_agc_dma_table_403200 | DMA 0x150 B | | H |
| 0x50cab8 | uphy_rf_set_402030_defaults | 0x402030/34/38 constants | | H |
| 0x50cb00 | uphy_pulse_409434 | 0x25 then 0x21 | | H |
| 0x50cb3c | uphy_rssi_restart_401900 | 0x401900 pulse, 0x401908 | | M |
| 0x50cb60 | uphy_rxcap_is_idle | ring off or 0x401308 b1 clr | | H |
| 0x50cb80 | uphy_cal_loopback_disable | undo 0x50cbf4 | | M |
| 0x50cbd8 | uphy_tone_fifo_flush | write 0 to 0x40135c n times | | M |
| 0x50cbf4 | uphy_cal_loopback_enable | 0x402050 b27, 0x405008 b24, 0x409468 | | M |
| 0x50cca4 | uphy_dfe_set_401308_bw | 0x401308[26:20], 0x40130c | | M |
| 0x50ccfc | uphy_dfe_tone_disable | restore 0x401308/0c/2c, 0x401358=0 | | M |
| 0x50cd7c | uphy_dfe_set_401358_recip | (1/N^2, 1/N) | | H |
| 0x50cdac | uphy_lphy_call_sync | 0x501484(3,..) + wait evt 5 | | M |
| 0x50cf84 | uphy_rf_set_402000_codes | 8-bit code fields 0x402000/04 | 0x53a5e4.. | M |
| 0x50d0b0 | uphy_rf_clear_402000_codes | clear them | 0x2d20 check | M |
| 0x50d120 | uphy_evt_clear_all | zero 4 B | | H |
| 0x50d134 | uphy_evt_set | amoor | | H |
| 0x50d144 | uphy_evt_get | lw | | H |
| 0x50d14c | uphy_evt_test_clear | amoand | | H |
| 0x50d164 | uphy_temp_hyst_update | hysteresis flag on 0x50113c value | | L |
| 0x50d198 | uphy_txpwr_calc_table | per-MCS power table | | L |
| 0x50d5a0 | uphy_rx_symbol_step | OFDM symbol state machine | ROM 0x525bc8/0x525c00 | M |
| 0x50d9f4 | uphy_rx_ctx_init | init 0x744 B ctx | | M |
| 0x50db04 | uphy_tx_ofdm_symbol_build | IFFT + CP copy (inferred) | | L |
| 0x50db9c | uphy_tx_symbol_step | TX bit/scramble/encode step | 0x53a724 | L |
| 0x50df94 | uphy_rx_sig_length_check | SIG/length sanity | | L |
| 0x50dfee | uphy_calc_quality_pct | 0..100 score | | L |
| 0x50e0c4 | uphy_mcs_select_by_snr | MCS from SNR thresholds | 0x53a730, 0x53a6d4 | M |
| 0x50e1c0 | uphy_s1g_ppdu_duration_us | PPDU end time | 560/240/360, 40/36 us | H |
| 0x50e278 | uphy_bits_pack7 | 7 bits MSB first | | H |
| 0x50e296 | uphy_movavg_push | moving average ring | assert 0xcd | H |
| 0x50e300 | uphy_fx_db_to_lin_q16 | dB to linear Q16 | 0x2830a = 10^(8/20) | M |
| 0x50e460 | uphy_meas_fft_size | table 0x53a848 | | M |
| 0x50e492 | uphy_meas_bin_to_freq | bin -> Hz (31250 based) | | M |
| 0x50e4d8 | uphy_meas_check_valid | validity flag ctx[0x1f] | | L |
| 0x50e502 | uphy_meas_capture_fft_loop | ring re-arm + FFT accumulate | ASSERT 98 | H |
| 0x50e5ac | uphy_meas_noise_floor | quarter-band power min, dB | | L |
| 0x50e6d0 | uphy_fx_mag_approx | max+min/2 magnitude | | H |
| 0x50e6fa | uphy_meas_spectrum_run | measurement core | | H |
| 0x50e87a | uphy_meas_bin_to_freq_off | | | L |
| 0x50e8b8 | uphy_meas_set_freq_shift | program 0x40940c slot | | L |
| 0x50ea02 | uphy_meas_avg_update | running average 8/12 | | L |
| 0x50f7f4 | uphy_fft_run | FFT/IFFT driver (custom ISA) | 0x51667c | M |
| 0x50faf4 | uphy_strlen | strlen | used by assert | H |
| 0x50fb62 | uphy_ctz | count trailing zeros | table 0x52cca4 | H |
| 0x538010 | uphy_rom_patch_apply | ROM patch unit | 0x406300/04 | H |
| 0x53809c | uphy_rom_hooks_install | hook table copy | 0x51fe80 | H |

Unnamed in range (deliberately): 0x50bd7c, 0x50c578, 0x50c858 (thunk to 0x50c7a4), 0x50e046,
0x50e43c, 0x50e444, 0x50e484, 0x50e89c, 0x50e8aa, 0x50e990, 0x50e9c4, 0x50eaf4, 0x50eb64 (FFT window
gen via ROM 0x52a330, L), 0x50ebb8, 0x50ed78, 0x50ee4c, 0x50ee70, 0x50f73c, 0x50f678 and all custom-ISA
fragments 0x50d348, 0x50d416, 0x50eae4..0x50fade. AON junk stubs 0x5385ae..0x53a82a are inside data
tables; not named.

## 3. Data and globals

| addr | name | notes | conf |
|---|---|---|---|
| 0x510000 | uphy_evt_flags | atomic event bits | H |
| 0x510424 | uphy_rx_bitbuf_wptr | write ptr into 0x5122d0 | H |
| 0x51047c | uphy_chip_variant | 0x2d20[13:12]; 0 normal, 1/2 alter many paths | H |
| 0x5104b0 | uphy_rx_start_idx | latched ring idx + delay | H |
| 0x5104b4 | uphy_rx_sample_rate | 1000000 marks 1 MHz | M |
| 0x5104ac | (byte) | enables 1 ms measurement timer 7 | M |
| 0x510530 | uphy_rx_read_idx | ring read index | H |
| 0x510540 | uphy_rx_cfo_est | | M |
| 0x510558 | uphy_rx_detect_time | 64-bit | M |
| 0x510578 / 0x51057c | uphy_psm_state_cache / uphy_psm_state | | H |
| 0x5122d0 | uphy_rx_bitbuf | 0xfa0 B circular | H |
| 0x514170 | uphy_rx_result_desc | -> 0x408078 | H |
| 0x5141b0 | uphy_rxcap_ring | 0x708 x u32 | H |
| 0x515dd0 | uphy_rx_ctx | 0x744 B | M |
| 0x516518 | uphy_tx_result_desc | -> 0x408080 | M |
| 0x51665c / 0x51667c | uphy_fftq_desc_ring / uphy_fft_desc | | M |
| 0x51fe80 | uphy_rom_hook_table | RAM copy of 0x539ea8 | H |
| 0x538000 | uphy_rom_patch_table | 2 patches | H |
| 0x5380d4 | uphy_crash_dump | 0xfee1dead + regs | H |
| 0x538164 / 0x538168 | uphy_assert_line / uphy_assert_file | | H |
| 0x538194 | uphy_trace_ring | 64 x 12 B | H |
| 0x538518 | uphy_agc_state | AGC/CCA state struct | M |
| 0x5389f8 | uphy_stats | stats IDs 0x3001.. | H |
| 0x538ce8 | uphy_meas_result | 0xa8 B | H |
| 0x539214 | uphy_str_psm_regs | unreferenced | H |
| 0x5394ec | uphy_psm_transition_table | 57 x 12 B | H |
| 0x5397a0 / 0x5397b4 | build date / version strings | | H |
| 0x539834 | uphy_jt_502d86 | 5-entry jt for 0x502d86 | H |
| 0x539848 | uphy_bwcfg_desc_table | {table, values, reg ptr, callback} | M |
| 0x5399e8 | (3 fn ptrs 0x503ed8/0x503d84/0x503d38) | used by 0x503fa4 | M |
| 0x5399f4 | uphy_stats_id_table | 139 entries | H |
| 0x539e4c | uphy_jt_host_cmd_dispatch | jt in 0x50583e | H |
| 0x539ea8 | uphy_rom_hook_init_table | 0x100 B | H |
| 0x539fa8 / 0x539fcc | uphy_jt_rxcap_on_detect_a/_b | per-BW cases | H |
| 0x539ff0 | uphy_rx_delay_tbl | (+0x53a000, 0x53a010) | M |
| 0x53a160 | uphy_jt_set_nco_slot | | H |
| 0x53a174 | uphy_rf_gain_code_tbl | (+0x53a184/94/a4) | M |
| 0x53a1b4 / 0x53a1e0 | AGC target u16 / byte tables | | M |
| 0x53a5c8 | uphy_jt_fem_set_state | | H |
| 0x53a5e4 | uphy_rf_402000_code_tbl | 5 tables at 0x30 stride | M |
| 0x53a6d4 | uphy_mcs_rate_tbl | u16 [bw][10] | M |
| 0x53a724 | uphy_tx_xor_seq12 | 12 B | L |
| 0x53a730 | uphy_mcs_snr_thresh_tbl | | M |
| 0x53a75c | uphy_pilot_pn127 | 127 x +-1 bytes | M |
| 0x53a848 | uphy_meas_fft_size_tbl | u16 {32, 64, 256, 128} | M |
| 0x538340.. 0x538490 | ROM-owned state (many ROM xrefs) | not named | |

## 4. Registers learned (new or refined)

| reg | field | meaning | evidence |
|---|---|---|---|
| 0x40904c | [17:2] | HW ring index latched at packet detect (RO status) | 0x509508 read, fw only masks it |
| 0x409050 | all | sample counter; fw zeroes on capture takeover | 0x50a8f2 |
| 0x409064 | [20:12] | sample-count threshold IRQ, base 0x3fe00002 | 0x50889c |
| 0x40905c/60 | | 64-bit timestamp | IRQ13 |
| 0x409030 | [19:16] | detected BW/mode at detect | 0x50a6fa |
| 0x409030 | [2:0],[5:3],[15:12] | per-BW config | 0x50adec |
| 0x4090bc.. | | detect snapshot window, 32 x (I16,Q16) | 0x50a830 |
| 0x409204..24 | | 9 detector thresholds | 0x50bc6c |
| 0x40940c+8n | b0, [4:1]; word [14:0],[29:15] | 5 NCO/notch slots (inferred) | 0x50c860 |
| 0x409400 | b2, b5 | pulsed around rx config/stop | 0x50adec, 0x50935c |
| 0x4094b4/0x409558 | | 0xa0 + 0x60 B coefficient tables | 0x50c4c0 |
| 0x408000 | [10:7],[14:11],[19:15] | PSM state/sub/aux | many |
| 0x408038 | | PSM trigger 1<<bit | 0x5015dc |
| 0x408078/7c/80 | | RX result desc / TX req desc / TX result desc pointers | 0x508940, 0x5096d8 |
| 0x401934 | [10:0] | RSSI, 1/8 dB, dBm = x/8 - 133 | 0x509ce8 |
| 0x401928 | [15:0],[31:16] | AGC/CCA thresholds | 0x50c080, 0x50bfd8 |
| 0x403030/34 | [30:28], [15:0] | AGC target per gain idx | 0x50c1ae |
| 0x403018 | b22, [1:0] | HW FEM control during RX (inferred) | 0x50ce18 |
| 0x2008/0x200c | 3-bit fields | pinmux, 0x12492492 = func 2 | 0x50ce18 |
| 0x401334 | [10:8],[4:1],b5,b0 | DC servo params / hold / en | 0x50c254 |
| 0x401358 | | (1/N^2)\|(1/N)<<16 | 0x50cd7c |
| 0x40131c | | CFO correction value (inferred) | 0x50a8ec |
| 0x3800..0x380c | | command/FFT engine | 0x509f88 |
| 0x406300/04, 0x2000 b23 | | ROM patch addr/insn, enable | 0x538010 |
| 0x1a00/0x1b00/0x401700/0x401800 | | DMA channels | 0x50a218 |
| 0x1d00..0x1d3c, 0x401a00/04 | | HW semaphores | 0x50a360.. |

PSM transition table (index: new state, trigger bit, aux), verified dump:
0:1/0/2 1:1/1/3 2:1/1/4 3:1/1/5 4:1/1/6 5:1/1/7 6:1/1/8 7:1/1/9 8:1/1/13 9:1/1/1 a:11/2/3 b:11/2/2
c:11/2/5 d:11/2/6 e:11/2/7 f:3/3/2 10:5/4/1 11:5/4/3 12:5/4/10 13:5/4/5 14:6/5/1 15:3/6/10 16:3/6/3
17:4/7/8 18:4/7/1 19:3/8/10 1a:3/8/1 1b:2/9/3 1c:2/9/10 1d:2/9/10 1e:12/11/11 1f:12/11/3 20:4/12/8
21:4/12/7 22:8/12/5 23:8/12/2 24:3/14/5 25:8/14/6 26:8/14/7 27:8/14/8 28:4/14/1 29:10/15/1
2a:12/16/11 2b:12/16/10 2c:1/17/11 2d:1/17/12 2e:1/18/6 2f:7/19/7 30:9/20/7 31:1/21/9 32:1/22/1
33..38: 13/23/{0,3,5,6,8,10}.
Uses seen: 0x26/0x27 rx abort, 0x2b TX done, 0x2e TX end, 0x2f rx start, 0x30 SIG ok, 0x31/0x32 rx end,
0x33 sleep, 0x14 debug.

## 5. Open questions
- Exact meaning of PSM states 2..6, 8, 0xd and the aux column. Hardware readback of 0x408000 during
  RX/TX would settle it (assumed meanings above).
- `0x50369c()` selects LO-retune vs 0x40131c CFO correction in `uphy_rxcap_on_detect`; not decoded.
- Custom-1 (0x2b) instruction semantics (FFT/DSP accelerator). A Binja arch plugin or manual decode
  would recover 0x50eff8..0x50fac3.
- `0x40940c` slot semantics (NCO vs notch) and why the measurement task clears slot 0 b0.
- What sets byte 0x5104ac (enables 1 ms measurement timer); possibly MEDIUM_EVAL related, unverified.
- The 0x539214 PSM printf string has no xref.

## 6. Notes for other regions (do not rename there; facts only)
- 0x5001e8: UPHY main loop (timers 1..9 via 0x50430c, mailbox 0x5022a0/0x502208 -> 0x50583e).
- 0x50583e: host/event message handler. IDs: 0x0003 (host command wrapper; inner u16 at +2:
  0x811c MEDIUM_EVAL -> 0x50397a(0x538898) + 0x509cca + timer 2; 0x811d calibration runner -> 0x505784,
  0x504608; 0x8023 -> 0x504538; others forwarded with 0x501484(3,3,..)), 0x200c -> 0x5055cc,
  0x2013 (writes 0x53919c/9e/a0), 0x201d (reply 0x201e via 0x501484), 0x2023, 0x3015 (copies 0x1c B to
  0x513670), 0x3019 (-> 0x504cd8), 0x5000 (assert), 0x5002 (-> 0x505654, waits evt 7, ROM 0x5281c8).
  Jump table 0x539e4c lives in AON.
- 0x506144: init + IRQ registration (table in section 1); reads chip id 0x2d20.
- 0x501f00 assert(file,line), 0x501f68 assert_pc, 0x502384 panic dump, 0x5027c4 trace, 0x502670 halt
  peers, 0x501e7c printf, 0x5015dc PSM transition, 0x5017fc PSM init (0x408034/40/3c/04, csr mie bits
  19/21), 0x501ca8 PSM trigger + cache refresh, 0x501d74 PSM history print.
- 0x507dbc (IRQ12 body) calls `uphy_rxcap_on_detect` at 0x507e1a (Taylor's patch point) and
  `uphy_rxcap_set_ring` at 0x507de2.
- 0x504e44 is the "go back to listen" routine (calls uphy_set_resamp_ratio, uphy_fem_set_state).
- ROM: 0x527c2c FFT-accumulate, 0x521124 delay, 0x524e20 atan2, 0x5248f4 Q16 mul, 0x52ada0/0x52aa02
  64-bit div, 0x522710 capture block reset, 0x52004a memcpy, 0x520106 memset, 0x521ad8 64-bit time,
  0x52102c delay, 0x520ed0/0x520edc irq save/restore, 0x52a330 window/twiddle gen, ROM table 0x52b1c0
  rx delay, 0x52b1f4 autocorr lag per BW, 0x52cdb0 per-BW 0x409000 field, 0x52cca4 ctz table.
  ROM 0x520c1c and 0x520e0c are patched by RAM (lock routines).
- ROM uses AON 0x538340..0x538490 heavily (state structs; also data refs from 0x536f0c.. pointer table).
