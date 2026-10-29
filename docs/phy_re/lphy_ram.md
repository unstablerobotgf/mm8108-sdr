# LPHY RAM (.lphy_imem 0x540000-0x546ab2, DMEM 0x548000-0x54ffff, AON 0x55c000-0x55d280)

All findings are from static analysis in Binja (decompile plus disassembly cross-checks). Nothing in this
file was bench-tested unless marked "verified (memory notes)". "Inferred" means the role is read from data
flow, not from a direct readback.

LPHY gp = 0x548848 (auipc/addi at 0x5400dc). gp-relative globals are invisible to Binja xrefs, so the DMEM
addresses below were computed by hand.

## 1. Subsystem overview

- Boot: `_start` -> `lphy_main_init` 0x5412c4 (ROM patch unit, ROM hook table copy, trap init, reads boot
  table at *0x100000, waits for clocks, releases mailbox lock 3) -> `lphy_app_init` 0x542d94 -> `lphy_msg_loop`.
- IPC: four mailboxes (one per core), each a single 64 B slot guarded by a spinlock word, plus a doorbell.
  The LPHY doorbell is its machine software interrupt (MSIP). The ISR copies the slot into an 8-deep queue;
  the message loop pops, dispatches, and then sleeps in `wfi`.
- Completion back to UPHY is not a mailbox message: LPHY sets bit 5 in UPHY's event word 0x510000 and rings
  0x401a40 (`lphy_signal_uphy_done`). UPHY's sync sender `sub_50cdac` waits on that bit.
- Calibration family lives here: DC offset (0x3017), single-point IQ imbalance (0x300d), per-gain IQ
  imbalance (0x301b), ADPLL KDCO cal (inside 0x300c), aux ADC two-point cal, temperature/voltage (0x3013).

## 2. Mailbox and message format (priority 1)

| item | value | evidence |
|---|---|---|
| per-channel slot ptrs | `lphy_mbox_buf_ptrs` 0x55c7b4: ch0 0x11fc80, ch1 0x12ff80, ch2 0x51ff80 (UPHY), ch3 **0x54ff80 (LPHY inbox)** | table read; used by `lphy_mbox_send` / `lphy_mbox_buf_ptr` |
| slot layout | `u32 type; u32 len; u8 payload[len]`, len <= 0x38 (assert) | `lphy_mbox_send` 0x541490, `lphy_mbox_rx_isr` 0x541cd4 |
| lock word | 0x1d30 + 4*ch (ch3 = **0x1d3c**), amoswap 1 to take, 0 to release; value 1 also means "message pending" | 0x5413f0 / 0x541410 / 0x541424 |
| doorbells | ch0 0x4100 bit0, ch1 0x4104 bit0, ch2 0x406000 bit0, ch3 **0x406004 bit0** | `lphy_mbox_send` |
| LPHY receive | MSIP trap (mcause 3) -> `lphy_mbox_rx_isr`: clears 0x406004 bit0, if lock==1 copies slot into `lphy_msgq` (0x548120, entries 0x548134, 8 x 64 B), releases lock | 0x541cd4, 0x541e18 |
| completion to UPHY | `atomic_or(*0x510000, 1<<5)`; `*0x401a40 = 1` | 0x545a24 -> 0x545fc4 |
| UPHY to LPHY events | `lphy_event_flags` 0x548000 (bits ORed by UPHY), doorbell IRQ 30 cleared by writing 0 to 0x401a44 | `lphy_uphy_doorbell_isr` 0x542d88 |

Sender side (UPHY, not renamed by me): `sub_501484(ch, type, len, payload)` is the UPHY twin of
`lphy_mbox_send`; `sub_50cdac(type, len, payload, busy*, 0x510000)` sends on ch3 and `wfi`-waits for bit 5.

### Dispatch table (every ID)

Message type is the slot `type` word. Payload is copied to stack sp+0x38; below `pN` = payload word N.

| type | handler | role | payload (as UPHY builds it) |
|---|---|---|---|
| 3 | inline 0x5406ac | host test command forwarded by UPHY | p0 cmd*, p1 len, p2 resp*. Cmd IDs: 0x19 (ack only, resp len 4), 0x24 (returns s8 power/gain sum from txpwr cfg, resp len 8; meaning inferred), 0x800E (if cmd word +0xc == 3 -> deliberate assert "cdcdf49f9b":0x21e, else not-supported), 0x8119 (sub 1 = dump 0x44 B of txpwr cfg, sub 2 = set cfg word0). Others: status 0xFFFF800B. Reply = type 4 on ch2 {resp*, cmd*, 0} |
| 0x3009 | `lphy_msg3009_sleep` 0x542c74 | clear `lphy_warm_boot_flag`, mask IRQs, save trace idx, 0x408084 = &0x548008, signal UPHY bit 7, `wfi` | none |
| 0x300a, 0x300b | default | assert "cdcdf49f9b":0x2df | |
| 0x300c | inline 0x5405aa + `lphy_msg300c_tune` 0x5435e4 | tune / RX re-config | +0 freq Hz, +4 bw MHz, +8 freq offset (added to freq), +0xc gain idx, +0xd dccal-valid flag, +0xe RX restart, +0xf LO-only retune, +0x10 (temp < -15 C flag), +0x11, +0x12 (UPHY `sub_504796`) |
| 0x300d | inline 0x5404fe + `lphy_iqcal_run` 0x543b08 | **IQ imbalance + DC cal via TX loopback tone** | p0 freq Hz, p1 bw MHz, p2 result struct ptr (UPHY AON 0x539170). len 0xc |
| 0x300e, 0x300f | default | assert | |
| 0x3010 | inline 0x5404c8 | board-config (BCF/OTP TLV) load into txpwr struct | p0 struct ptr (UPHY 0x538d90); struct +0x18 becomes `lphy_txpwr_cfg_ptr` |
| 0x3011 | inline 0x540492 | FEM config: `lphy_fem_config(p1)`, copy 64 B p1 -> `lphy_fem_cfg` 0x548008, `lphy_init_rf_table_ptrs` | p0 0, p1 = UPHY 0x538db0 |
| 0x3012 | default | assert | |
| 0x3013 | inline 0x540410 | read temperature / supply voltage | p0 s16* temp out (UPHY 0x538e28), p1 voltage out (0x538e24), p2 byte0 = ack, byte1 = read voltage, byte2 = read temp |
| 0x3014 | inline 0x5403fa | `*p0 = lrom_clk_measure_with_freq_counter(0x400)` | p0 = UPHY 0x538e9c |
| 0x3015, 0x3016 | default | assert | |
| **0x3017** | inline 0x540380 -> `lphy_msg3017_dccal_prepare` 0x546968 -> `lphy_dc_offset_cal` 0x54666e | **DC offset cal over 21 gain indices** | see section 3 |
| 0x3018-0x301a | default | assert | |
| 0x301b | inline 0x5402b0 + `lphy_iqcal_per_gain_run` 0x544eec | per-gain IQ imbalance cal (21 coefficient words) | p0 gain idx to apply after, p1 freq Hz, p2 bw MHz, p3 u32[21] out (UPHY 0x538ea0). len 0x10 |

Messages LPHY sends to ch0 (MAC side): type 6 {3} at start ("LPHY up", `lphy_mbox_notify_host_ready`),
type 0 {3}, type 1 {3, code} on exit (`lphy_exit_park`).

## 3. 0x3017 DC offset cal: exact format, state, preconditions

### Payload (len 0x34, 13 words), as UPHY `sub_5064a8` builds it (verified by disassembly of both sides)

| off | word | UPHY value | LPHY use |
|---|---|---|---|
| 0x00 | p0 | 0x5141b0 (UPHY capture ring) | capture buffer for every DC snapshot |
| 0x04 | p1 | *0x538e5c = bw MHz (1/2/4/8) | byte0 = bw; selects gain-table bw column and snapshot length; copied to `lphy_dccal_last_bw` 0x55c5f3 |
| 0x08-0x14 | p2..p5 | 0 | unused |
| 0x18 | p6 | 0x538ef4 | out: u16[21] magnitude of DAC A per gain idx |
| 0x1c | p7 | 0x538f1e | out: u16[21] magnitude of DAC B |
| 0x20 | p8 | 0x538f48 | out: u8[21] sign A |
| 0x24 | p9 | 0x538f5d | out: u8[21] sign B |
| 0x28 | p10 | 0x538f72 | out: u8[21] 5-bit range field (0x402044[27:23]) |
| 0x2c | p11 | byte0 = *0x538526 (current gain idx), byte2 = caller arg | byte0 = gain idx re-applied after cal |
| 0x30 | p12 | gp-0x33c (UPHY u8) | LPHY writes `!success` (0 = ok, 1 = failed). UPHY re-arms a 15 ms (0x3a98) timer on 1 |

LPHY builds a ctx at sp+0x70: {p0, (u16)(bw | gain<<8), p6..p10, u16 0x2040, u8 0xb5,
`lphy_rf_gain_table_ptr` value, `lphy_rf_gain_bw_table_ptr` value, &`lphy_fem_cfg`}. 0x40 = base snapshot
length, 0x20 = 8 MHz snapshot base, 0xb5 = s8 -75 power limit.

### Sequence

1. `lphy_msg3017_dccal_prepare`: 0x401334 bit0 = 0 (digital DC servo off); 0x405008 bit24 pulse 0 then 1;
   0x409468 bit5 = 0; 0x402050 bit27 = 0 then bit26 0 then 1 (sample clock on); 0x402044[27:23] = 0;
   0x402000 bit0 = 1.
2. `lphy_dc_offset_cal`: `lphy_fem_set_mode(cfg, cfg[0x3e]+4)` (FEM into a cal mode, inferred: isolation).
   bw byte maps 1/2/4/other to column 0/1/3/... (1->0, 2->1, 4->2, else 3).
3. For gain idx i = 0..20 (stops early if a measurement failed):
   `lphy_set_rf_gain_index(i, col)`, strobe 0x401d08 bit4 for 4 us, then 4 probes j:
   0x402044[27:23] = {0, 4, 0x10, 8}[j]; both DACs = {0, 0x80, 0x40, 0x100}[j] via `lphy_dccal_set_dac_codes`;
   `lphy_measure_dc` -> mean I, mean Q (64-bit).
4. If |I0| or |Q0| (probe 0) > 0x40: `lphy_dccal_fit_channel` (per I and Q) and `lphy_dccal_solve_code`
   search range 0..31 and code in [-275, +275] so the predicted residual is within 0x40; else codes = 0.
5. Write back: gain table entry i (12 B) word0[31:27] = range, [26:21] = |A| low 6, [20:15] = |B| low 6;
   word1[1] = sign A, [0] = sign B; word2[19:17] = |A| high 3, [16:14] = |B| high 3. Also UPHY arrays p6..p10.
6. After the loop: FEM mode 1 (RX), `lphy_set_rf_gain_index(p11.b0, col)`, strobe, 0x401334 bit0 = 1
   (servo back on; bit4 HOLD untouched).
7. Back in the loop: on success `lphy_apply_dccal_codes(p11.b0)` (writes the DACs for the live gain idx),
   `*p12 = !success`, then `lphy_signal_uphy_done`.

`lphy_measure_dc` (0x54623c): n = 0x40 << log2(bw) (bw 8: 0x20 << 3 = 256); writes 0x40902c = 1;
`lphy_rxcap_start(p0, n)`; waits n hwtimer ticks; power estimate from 0x401934[10:0]
(`lphy_dccal_power_estimate`: ceil(x/8) - 86 - (48 - 3g), floor -107); if that exceeds -75 + 3g returns
**1 = fail** (strong in-band signal); else averages samples n/2..n-1 (I = low s16, Q = high s16);
`lphy_rxcap_stop(p0, n)`.

### Where the result lives

- Persistent per-gain codes: the RF gain table at `lphy_rf_gain_table_21x12` 0x55c254 (AON RAM, writable),
  reached via `lphy_rf_gain_table_ptr` 0x55c35c. They are **not frequency-indexed**: a hop leaves every
  entry tuned for the old LO.
- Live DACs: written by `lphy_apply_dccal_codes` (0x3017 end, and 0x300c when no LO retune and
  `lphy_dccal_codes_valid` 0x55c5fe set and variant != 1). `lphy_dccal_range_cur` 0x55c43c mirrors 0x402044[27:23].
- UPHY mirror: 0x538ef4..0x538f87.

### Preconditions for re-triggering after a fast PLL hop

1. **Capture ring ownership (hard).** `lphy_rxcap_start` overwrites 0x409044/0x409048 with p0/n and
   requires 0x40906c == base right after; otherwise it returns 0xffff8000 and `lphy_measure_dc` asserts
   ("a31ed7725a":0x3b, LPHY dead). Your v4 ring patch NOPs that check at 0x545618, which avoids the assert
   but then measures from whatever the pointer does. After the cal the ring is left at base p0, length n,
   0x40904c bit0 = 0: the host must re-take the ring afterwards. Pointing p0 at a host-owned scratch buffer
   (any 1 KB the DMA can reach) keeps UPHY's 0x5141b0 untouched (assumed safe, not tested).
2. **LO state.** DC cal itself does not retune; it calibrates at whatever the synthesizer is on. So hop first,
   then cal. LPHY's notion of the LO (`lphy_cur_lo_hz` 0x54836c) is not read by 0x3017.
3. **Quiet band.** Any gain index whose snapshot power exceeds the limit aborts the remaining indices and
   reports failure. Expect failures near strong carriers at high gain (low index); UPHY just retries.
4. **Gain table pointers set** (0x3011 at boot does it) and the FEM cfg block present (cfg[0x3e] used).
5. **LPHY idle in its loop** (true after boot; it services MSIP even while UPHY runs).
6. Side effects to undo: digital servo re-enabled (bit0) at the end, DC servo HOLD is yours to re-apply;
   live gain idx forced to p11.b0; FEM mode forced to 1.

### Two ways to trigger

- **Via UPHY (preferred):** UPHY `sub_505784` (calibration-mask runner, reached from the UPHY message
  dispatcher at 0x505b6a; memory notes say host test cmd 0x811D) does mask bit3 -> 0x3017
  (`sub_5064a8(0)`) and bit4 -> 0x300d with (freq = *0x538e58, bw = *0x538e5c). UPHY handles the lock,
  wait and retry. Catch: it uses UPHY's channel state 0x538e58/0x538e5c, which a host-side register hop
  does not update, and it reprograms the ring to 0x5141b0.
- **Direct host mailbox write (untested):** take lock 0x1d3c (read 0, write 1; no atomics from the host, so
  race with UPHY is possible), write {0x3017, 0x34, 13 words} to 0x54ff80, set 0x406004 bit0. Point p6..p10
  and p12 at host scratch, preset *p12 = 0xff and poll it. LPHY will also OR bit5 into 0x510000: clear it
  afterwards or UPHY's next `sub_50cdac` wait returns early.

## 4. IQ calibration family

### 0x300d single-point IQ (+ optional DC) cal, `lphy_iqcal_run` 0x543b08

- Params from `lphy_iqcal_init_params(p2 struct)`: mode byte at struct+0x20 (UPHY 0x539190; `sub_506454`
  sets it to 1 when its arg3 != 0). Mode 0 also searches DC I and Q (types 1, 2) and writes **0x401378**
  (low16 = DC I, high16 = DC Q).
- `lphy_iqcal_loopback_setup`: decimator config, `lphy_rf_tune(freq, bw)` (retunes the LO in mode 1 if
  `lphy_cur_lo_hz` != freq), FEM modes 1 then 5, 0x401334 = (x & ~0x1e) | 0x28, 0x409000 bit23 = 0,
  0x40904c bit1 = 1, 0x405000/04/08, 0x402008[9:0] = 0xa3.
- `lphy_iqcal_search_param` (types 3 gain, 4 phase): bisection / geometric search; each trial loads the
  10-sample complex test tone `lphy_iqcal_tone_i/q` (0x55cd04/0x55cd2c) plus trial correction through
  `lphy_iqcal_load_tone_and_coeffs` (also captures 0x100 samples into 0x5486f4), then
  `lphy_iqcal_measure_tone_power` (92-point single-bin DFT, sqrt power).
- Result: gain g and phase phi stored in struct; **0x401374 = (tan(phi)>>2) | ((1/(g cos phi))>>2) << 16**.
- `lphy_iqcal_loopback_teardown` restores RX (0x409000 bit23 = 1, 0x40904c bit1 = 0).
- Precondition: needs the capture path to LPHY DMEM 0x5486f4 (same ring check as DC cal), and **p0 must
  be the new LO**, otherwise LPHY retunes to the stale frequency. The UPHY runner sends *0x538e58, which
  your native hop leaves stale, so either update 0x538e58/0x538e48 or send the message directly.

### 0x301b per-gain IQ cal, `lphy_iqcal_per_gain_run` 0x544eec

Work struct `lphy_iqcal_gain_work` 0x548428 (0x2cc B, defaults from `lphy_iqcal_gain_work_init`). For
each gain idx: `lphy_iqcal_tone_capture` (tone x103 into 0x40135c, scaled by
`lphy_iqcal_tone_amp_by_gain` 0x55ccc0, 0x200-sample capture into 0x5486f4), `lphy_iqcal_measure_imbalance`
(I/Q rms ratio and correlation). Up to 4 passes alternating 0x402050 bit26/bit27. Output p3[i] =
(1/(g cos phi))>>2 << 16 | tan(phi)>>2, saturated to s16 each. p3[p0] is written to **0x401384**
(`lphy_iqcal_apply_gain_coeff`). The UPHY sender (`sub_506610`) passes freq/bw = 0 (assumed meaning:
"use current"; `lphy_rf_tune_ctx` would then tune to 0 Hz, which I could not reconcile; treat as open).

### LO / PLL

- `lphy_set_lo` 0x542e60 (name confirmed): arg1 = mode (0x402050 bit27), arg2 = Hz, arg3 = &cur.
- `lphy_lo_retune_if_changed` 0x543138: retune only when freq differs from `lphy_cur_lo_hz`; mode 1
  additionally sets 0x405008 bit24, 0x402050 bit27, 0x409468 bit5.
- `lphy_pll_kdco_cal` 0x5431c8: ADPLL KDCO cal. Sets 0x40205c = 0x7000, steps 0x402060 by +-0x100000,
  reads frequency counter 0x402068[8:0], averages 20 steps into 0x40205c. Cached per frequency in
  `lphy_kdco_cached_freq/value` 0x55c668/0x55c664, so repeat tunes skip it. (Explains the "static 28672"
  KDCO in UPHY stats when no re-cal happens.)
- `lphy_rx_chain_channel_setup` 0x543330: 0x40138c[9:3] by band (0x78 below 900 MHz, 0xb0 above 930 MHz,
  ((MHz-900)/4+15)<<3 between), 0x401358 from MHz, 0x40132c bits0/1, 0x401308 bit30.
- `lphy_set_resampler_ratio` 0x545568 writes 0x409404 (matches memory note).

## 5. Function table (all names I set)

Confidence: H high, M medium, L low.

| addr | name | role | evidence | conf |
|---|---|---|---|---|
| 0x540d34 | lphy_auxadc_select_channel | aux ADC mux group 1-4 -> 0x405040/44/48 | bit fields, asserts on bad group | M |
| 0x540dcc | lphy_auxadc_configure | 0x405050/0x406500/0x405040/0x40504c config | callers | M |
| 0x540e4c | lphy_auxadc_two_point_cal | two reference conversions, gain/offset Q16 to gp-0x4e0..-0x4ec | math 0x19e/(b-a), 400-g*a | M |
| 0x540fc4 | lphy_auxadc_ensure_cal | run cal if coeffs zero | | M |
| 0x540ff0 | lphy_auxadc_convert | one conversion, 0x40651c start, poll 0x406540, result 0x406524 | | M |
| 0x541094 | lphy_auxadc_read_channel | select + convert + restore | | M |
| 0x541138 | lphy_read_temperature | aux ch 8, (x*1/3 - offset)>>16 -> 0x548048 | used by 0x3013 temp | M |
| 0x5411ac | lphy_read_supply_voltage | aux ch 9 scaled -> 0x548354 | used by 0x3013 | L |
| 0x541204 | lphy_uart_puts | UART 0x1500 busy-wait write | | H |
| 0x54127c | lphy_uart_puts_locked | lock 0x1d2c around puts | | H |
| 0x541298 | lphy_exit_park | "hart 3 exited" print, notify ch0, wfi forever | string | H |
| 0x5412c4 | lphy_main_init | boot sequence | | H |
| 0x5413c4/0x5413e0 | lphy_print_lock_acquire/release | spinlock 0x1d2c | amoswap | H |
| 0x5413f0/0x541410/0x541424 | lphy_mbox_lock_acquire/release/is_locked | lock 0x1d30+4*ch | | H |
| 0x541438/0x54144c | lphy_crash_lock_acquire/test | 0x1d00 bit0 | used in crash path | M |
| 0x541458 | lphy_inc_401a04_counter | amoadd 0x401a04 | | L |
| 0x54146c | lphy_assert_hartid3 | mhartid must be 3 | csr 0xf14 | H |
| 0x541490 | lphy_mbox_send | see section 2 | | H |
| 0x541544/0x541554 | lphy_set_chip_info / lphy_get_chip_variant | 0x2d20 fields to gp-0x4d0/4d4/4cc | | H |
| 0x54155c | lphy_mbox_send3 | send 12 B {a2,a3,a4} | | H |
| 0x541578 | lphy_msg_unpack3 | unpack {cmd,len,resp} | | H |
| 0x541598/0x5415b4/0x5415d8 | lphy_mbox_notify_host_type0/ready/exit | ch0 types 0/6/1 | | M |
| 0x5415f4 | lphy_mbox_buf_ptr | 0x55c7b4[ch] | | H |
| 0x541608 | lphy_mie_disable_timer | mie bit7 clear | | H |
| 0x541614/0x54165c | lphy_vprintf / lphy_printf | vsnprintf + puts | | H |
| 0x5416b0 | lphy_log_trap | "trap %ld:0x%lx" | string | H |
| 0x5416d8/0x541740 | lphy_assert_fail / lphy_assert_fail_pc | assert (hash, line), crash dump | strings | H |
| 0x541790/0x5417fc/0x54189c | lphy_bcf_get_raw/get/get_field | board-config TLV lookup (key, default, mask) via lrom_objtab_lookup at obj+0x230 | | M |
| 0x541970/0x5419e0/0x541a6c | lphy_msgq_push/pop/is_empty | 64 B entries, 0x38 max | | H |
| 0x541a78 | lphy_trap_init | mtvec = _start (vectored), intc reset, clear MSIP | | H |
| 0x541aa0 | lphy_crash_dump_and_halt | regs to 0x55c0c4.., marker 0xfee1dead | | H |
| 0x541c04 | lphy_ext_irq_complete_mret | write claim 0x404704, restore, mret | | M |
| 0x541c40 | lphy_irq_register | handler table gp-0x4b0, priority, enable | | H |
| 0x541c74/0x541d2c | lphy_isr_timer_vec / lphy_isr_msip_vec | vector stubs | | H |
| 0x541cd4 | lphy_mbox_rx_isr | see section 2 | | H |
| 0x541d8c | lphy_notify_all_cores_crash | ring all doorbells | | M |
| 0x541dd4/0x541e18/0x541fc8 | lphy_unhandled_trap / lphy_trap_dispatch / lphy_trap_entry | mcause 11 ext (claim 0x404704), 7 timer, 3 MSIP | | H |
| 0x541ed8 | lphy_trace_event | 16-entry ring 0x55c184 {time 0x406040, id, arg} | | H |
| 0x541f0c/0x541f24 | lphy_trace_idx_save/restore | across sleep | | M |
| 0x541f3c | lphy_rand_range | xoroshiro64** at 0x55c248 | | H |
| 0x541f7c/0x541f88 | lphy_set/get_time64 | gp-0x4c0 | | M |
| 0x541f94/0x541fb8 | lphy_set_phy_cfg_ptr / lphy_get_phy_cfg_word14 | 0x55c250 | | M |
| 0x5420dc | lphy_iqcal_apply_gain_coeff | 4 bytes to 0x401384 | 0x301b | M |
| 0x542146/0x54235c/0x5426b6/0x54291c | lphy_bcf_load_misc/txpwr/fem_params/chan_overrides | 0x3010 board config parsing | key numbers | M |
| 0x54230c | lphy_txpwr_derive_offsets | per-bw power offset arrays | | L |
| 0x54268c | lphy_bcf_get_board_type | key 0x2a [11:8] | | L |
| 0x542bf0..0x542d6c | lphy_timer_* | timer 0x401b00/40 enable, 0x401b88/8c compare, list gp-0x7e0, events gp-0x4f8 | | H |
| 0x542c74 | lphy_msg3009_sleep | see table | | M |
| 0x542d88 | lphy_uphy_doorbell_isr | IRQ 30, clear 0x401a44 | | M |
| 0x542d94 | lphy_app_init | bcf ptr, variant, msg queue init (cap 8), timer init | | H |
| 0x542dfc | lphy_msgq_push_or_assert | | | H |
| 0x542e2c | lphy_pulse_405020_bit0 | 0x405020 bit13 clear, bit0 pulse | | L |
| 0x543138 | lphy_lo_retune_if_changed | section 4 | | H |
| 0x5431c8 | lphy_pll_kdco_cal | section 4 | 0x402060/68/5c | M |
| 0x543330 | lphy_rx_chain_channel_setup | section 4 | | M |
| 0x543448 | lphy_lo_retune_mode1 | 0x401308 b18/b19 around retune, rxcap reset | | M |
| 0x5434a8 | lphy_rf_tune | LO + rx chain by chip variant | | M |
| 0x5435e4 | lphy_msg300c_tune | 0x300c core | | M |
| 0x54365c..0x543dc0 | lphy_iqcal_* (measure_tone_power, build_trial, search_param, run, init_params) | section 4 | | M |
| 0x543ec8 | lphy_band_table_select | 52 B table by freq (<=818, <=890, above) | thresholds | M |
| 0x543f74/0x5440fc/0x544338 | lphy_adc_codes_alt_variant / adc_enable_alt_variant / rx_analog_on_alt_variant | variant != 0 path (not MM8108B2) | | L |
| 0x54413c | lphy_dfe_set_409400_409408 | 0x409400[9:8] log2 bw, 0x409408[11:0] = 2f/1e6 | | L |
| 0x544194 | lphy_rx_pre_config | resampler, 0x409400 bits, LO mode 0, 0x401d08 | | M |
| 0x5442ac | lphy_rx_lna_mixer_config | 0x402020/0x402018 from gain entry | | L |
| 0x544714/0x5447bc/0x544858 | lphy_rx_frontend_config / rx_analog_on / rx_enable | RX bring-up for variant 0 | | M |
| 0x5448b8 | lphy_dccal_set_dac_codes | signed codes into the DC DAC fields | section 3 | H |
| 0x54494c | lphy_rf_tune_ctx | decimator + rf_tune from ctx | | M |
| 0x544984/0x544ab8/0x544b58 | lphy_iqcal_loopback_setup/teardown/mode | | | M |
| 0x544c64/0x544dfc/0x544eec/0x5454b4 | lphy_iqcal_measure_imbalance/tone_capture/per_gain_run/gain_work_init | 0x301b | | M |
| 0x545568 | lphy_set_resampler_ratio | 0x409404 | | H |
| 0x5455cc/0x5455f8 | lphy_rxcap_stop/start | ring base/len/ctrl, 0x401d04 gate, 0x401308 bit1 | | H |
| 0x545648 | lphy_rf_blocks_enable | 0x402000 b0-2, 0x402010 b28, 0x402028 b16, 0x402020 b28, 0x402004 b22 | | L |
| 0x5456a4 | lphy_rmw_402078_mask | 0x402078[20:0] all-ones or 0 | | L |
| 0x5456c4/0x545778 | lphy_rf_set_bw_codes(_default) | 0x402028/2c/30 from 0x55cd54..0x55cd84 by bw | | L |
| 0x545780/0x5457d8 | lphy_rf_loopback_off/on | 0x402050 b27, 0x405008 b24, 0x409468 | | M |
| 0x545888/0x5458c4 | lphy_rxcap_cfg_409468 / rxcap_enable_409468 | | | L |
| 0x5458f8/0x545950/0x5459d0/0x545a00 | lphy_dfe_set_bw_filter / dfe_bypass_cfg / dfe_set_401358_from_mhz / dfe_set_409470_from_hz | | | L |
| 0x545a24/0x545fc4 | lphy_signal_uphy_done / lphy_signal_core_event | section 2 | | H |
| 0x545a28/0x545c38/0x545ca8 | lphy_fem_init / fem_config / fem_set_mode | GPIO set/clear 0x1368/0x136c, pinmux 0x2008/0x200c, 0x403018 | | M |
| 0x545c20/0x545c2c | lphy_gpio_1360_clear/write | | | L |
| 0x545e14/0x545f40 | lphy_adc_set_scaled_codes / adc_codes_clear | 0x402000/0x402004 code fields from 0x55d180.. tables x scale | | L |
| 0x545fb0/0x545fdc | lphy_event_flags_clear/read | 0x548000 | | H |
| 0x545fe4 | lphy_txpwr_temp_comp | temp-dependent TX gain into cfg+0xe4 | | L |
| 0x5460a4 | lphy_fx_db8_to_lin_q16 | 1/8 dB amplitude to Q16 | constants 2.512, 1.122, 1.0145 | H |
| 0x5461e0/0x5461f4 | lphy_rf_strobe_401d08_set/clear | bit4 | | L |
| 0x546206 | lphy_dccal_power_estimate | section 3 | | M |
| 0x54623c | lphy_measure_dc (existing, kept) | section 3 | | H |
| 0x54640c/0x546578 | lphy_dccal_fit_channel / dccal_solve_code | section 3 | | M |
| 0x54666e | **lphy_dc_offset_cal** (renamed from lphy_dc_offset_cal_entry) | section 3 | | H |
| 0x546968 | lphy_msg3017_dccal_prepare (existing, kept) | section 3 | | H |
| 0x5469e0/0x546a50/0x546a76 | lphy_strlen / lphy_lshr64 / lphy_ctz32 | libc helpers | | H |
| 0x55c000 | lphy_aon_install_rom_patches | ROM patch unit, see below | | M |
| 0x55c08c | lphy_aon_install_rom_hooks | copy 0x55c8a4 (0x100 B) to 0x54fe80 hook table | | H |

Existing names reviewed and kept: lphy_msg_loop, lphy_apply_dccal_codes, lphy_init_rf_table_ptrs,
lphy_set_lo, lphy_iqcal_load_tone_and_coeffs, lphy_set_adc_clkdiv_and_loop_codes, lphy_set_rf_gain_index,
lphy_measure_dc, lphy_msg3017_dccal_prepare.

**Fixed:** the symbol `lphy_dc_offset_cal` at 0x546684 sat in the middle of an instruction sequence
(`addi a5,a5,0x10` inside the prologue of the real function; no xrefs). I removed it and renamed the real
function 0x54666e from `lphy_dc_offset_cal_entry` to `lphy_dc_offset_cal`. BRIEF.md's "0x546684" address is
therefore off by 0x16.

Left unnamed: one-instruction `ret` stubs (0x541294, 0x54158c, 0x541594, 0x54195c-0x54196c, 0x541ed4,
0x541f08) and the AON junk stubs.

## 6. Data tables and globals

| addr | name | notes |
|---|---|---|
| 0x55c254 | lphy_rf_gain_table_21x12 | per gain idx: word0 [31:27] range, [26:21]/[20:15] DAC A/B low bits, [14:0] gain fields; word1 [1:0] signs + gain fields; word2 [19:14] DAC high bits + gain fields |
| 0x55c35c / 0x55c358 / 0x55c354 / 0x55c350 | lphy_rf_gain_table_ptr / lphy_rf_gain_bw_table_ptr / (0x55cd94) / (0x55d27c) | set by lphy_init_rf_table_ptrs |
| 0x55c43c | lphy_dccal_range_cur | |
| 0x55c5e0 | lphy_txpwr_cfg_ptr | big struct from 0x3010 (fields 0xe0-0xf5, 0x121.., 0x1b0.., 0x1dc-0x1e5) |
| 0x55c5f3 / 0x55c5fe | lphy_dccal_last_bw / lphy_dccal_codes_valid | |
| 0x55c660 / 0x55c661 | lphy_cur_bw_mhz / lphy_warm_boot_flag | |
| 0x55c662 / 0x55c664 / 0x55c668 | lphy_rxchain_last_mode / lphy_kdco_cached_value / lphy_kdco_cached_freq | |
| 0x55c66c / 0x55c66e / 0x55c670 / 0x55c0c0 | temp last applied / temp comp enabled / temp C (default 25) / temp cal offset (0x71fd16) | |
| 0x55c0c4, 0x55c154, 0x55c158 | crash regdump, assert line, assert file hash (32 B) | |
| 0x55c180 / 0x55c184 | trace idx saved / trace ring 16x12 | |
| 0x55c248 / 0x55c250 | prng state / phy cfg ptr | |
| 0x55c7b4 | lphy_mbox_buf_ptrs | followed by build date "2026-07-01 12:29" |
| 0x55c858 | lphy_msg_jump_table_3009_301b | 19 entries |
| 0x55c8a4 | lphy_rom_hook_init_table | 0x100 B, the "functions" there are junk |
| 0x55c9a4 / 0x55c9d8 / 0x55ca0c | lphy_band_tbl_818_890mhz / above_890 / below_818 | 0x34 B each |
| 0x55ccc0 | lphy_iqcal_tone_amp_by_gain | u16[21] |
| 0x55ccf4 | lphy_iqcal_loopback_cfg_bytes | 2-entry byte arrays at 0x55ccf4..0x55cd00 |
| 0x55cd04 / 0x55cd2c | lphy_iqcal_tone_i / q | 10 words each |
| 0x55cd54..0x55cd84 | lphy_rf_bw_codes_* | 4 words each, by bw |
| 0x55d180 | lphy_adc_scaled_code_tables | 5 tables x 12 words at stride 0x30 |
| 0x55d274 | lphy_dfe_bw_filter_bytes | + 0x55d278 |
| 0x548000 | lphy_event_flags | UPHY sets bits |
| 0x548008 | lphy_fem_cfg | 64 B from 0x3011 |
| 0x548048 | lphy_temp_reading | |
| 0x548068 | lphy_timer_list_head | gp-0x7e0 |
| 0x548120 / 0x548134 | lphy_msgq / entries 8x64 | {rd, wr, count, cap, entries*} |
| 0x548338 | lphy_irq_pending_bitmap | gp-0x510 |
| 0x54834c / 0x548350 | trace idx / timer event bits | |
| 0x548354 / 0x54835c | supply voltage / aux ADC cal coeffs (to 0x548368) | |
| 0x54836c | lphy_cur_lo_hz | gp-0x4dc, written by lphy_set_lo |
| 0x548374 / 0x548378 / 0x548380 / 0x548394 | variant copy / chip id low / bcf obj ptr / lphy_chip_variant (0x2d20[13:12]) | |
| 0x548398 | lphy_irq_handler_table | gp-0x4b0 |
| 0x548428 | lphy_iqcal_gain_work | 0x2cc B |
| 0x5486f4 | lphy_cal_capture_buf | IQ cal captures, up to 0x200 words |
| 0x54ff80 | lphy_mbox_inbox | ch3 slot |

## 7. Registers learned (static)

| reg | field | meaning | evidence |
|---|---|---|---|
| 0x1d2c | word | print spinlock | amoswap |
| 0x1d30+4*ch | word | mailbox lock / pending | mbox code |
| 0x4100, 0x4104, 0x406000, 0x406004 | bit0 | doorbells ch0..ch3 (0x406004 = LPHY MSIP) | |
| 0x401a40 / 0x401a44 | word | LPHY->UPHY doorbell / UPHY->LPHY doorbell clear | |
| 0x404704 | | ext IRQ claim/complete | trap dispatch |
| 0x406400 / 0x406404 | | ROM patch unit: address / replacement JAL | lphy_aon_install_rom_patches |
| 0x406040 | | timestamp counter | trace |
| 0x401334 | bit0 | DC servo enable (cleared during DC cal) | prepare/cal |
| 0x401374 | | IQ correction (tan, 1/(g cos)) | 0x300d |
| 0x401378 | | DC correction I/Q 16+16 | 0x300d mode 0 |
| 0x401384 | | per-gain IQ correction word (same format) | 0x301b |
| 0x40135c | | test tone FIFO | iqcal |
| 0x401934 | [10:0] | power/RSSI metric used as DC-cal interference gate | measure_dc |
| 0x401d08 | bit4 | strobe after gain change (4 us) | |
| 0x402018 | [20],[21],[29:24] | DC DAC signs A/B, A magnitude low | apply_dccal |
| 0x40201c | [5:0] | DC DAC B magnitude low | |
| 0x402024 | [26:24] | DC DAC B magnitude high | |
| 0x40202c | [19:17] | DC DAC A magnitude high | |
| 0x402044 | [27:23] | DC coarse range (5 bit) | |
| 0x402050 | bit26 / bit27 | sample clock on / loopback(TX) mode | |
| 0x402054 | [10:6] | VCO band field: 0x16 if temp >= 0, 0x14 if < 0, above 880 MHz | 0x3013 |
| 0x40205c / 0x402060 / 0x402068 | | KDCO value / KDCO step / freq counter [8:0] | kdco cal |
| 0x40138c | [9:3] | band-dependent RF code by frequency | rx chain setup |
| 0x409044/48/4c/6c | | ring base/len/ctrl/write ptr (as in memory notes) | rxcap |
| 0x40902c | | written 1 before each DC snapshot | measure_dc |
| 0x2d20 | [13:12] | chip variant (0 on MM8108B2 selects the path used here) | |

## 8. Open questions

- 0x301b from UPHY passes freq = 0, bw = 0; how LPHY avoids tuning to 0 Hz is not resolved (maybe
  `lphy_rf_tune` with freq 0 is a no-op through a path I did not trace).
- Which physical DAC is I vs Q (A vs B): the code only tells me A pairs with probe "low s16" ordering in the
  solver; not verified.
- FEM mode numbers (4/5 used during cal) are inferred from usage only.
- Message 0x24 test command semantics.
- The ROM patch unit at 0x406400/04 could allow redirecting ROM functions; never exercised, would need a
  bench test.

## 9. Notes for other regions (not renamed by me)

- UPHY `sub_501484` = UPHY mailbox send (twin of lphy_mbox_send); `sub_50cdac` = sync send to LPHY and wait
  bit5 of 0x510000.
- UPHY `sub_5064a8` = send 0x3017 DC cal (payload in section 3), retries via timer 0x3a98 when failed.
- UPHY `sub_506454(freq, bw, set_mode1)` = send 0x300d, result struct 0x539170.
- UPHY `sub_506610` = send 0x301b, coeff array 0x538ea0.
- UPHY `sub_504796` = send 0x300c (tune), state 0x538e58 (freq), 0x538e5c (bw), 0x538526 (gain idx).
- UPHY `sub_505548` = send 0x3013 (temp/volt), `sub_505f4a` = send 0x3010, `sub_506678` = PHY init sequence
  (0x3011, 0x300d, 0x3014, then calibrations), `sub_505654` = stop, sends 0x3009.
- UPHY `sub_505784` = calibration-mask runner: bit0 temp, bit1 voltage, bit3 DC cal, bit4 IQ cal; called
  from the UPHY dispatcher at 0x505b6a (consistent with the 0x811D note; not verified here).
- UPHY channel state 0x538e48 (freq), 0x538e4c (bw), copied to 0x538e58/0x538e5c before 0x300c.

## Hardware check 2026-10-25: 0x40138c[9:3]
Boot value is 0x94 (code 18) at 763, 880 and 980 MHz alike, so the band formula above is not what runs on this
board. Paired A/B sweeps of codes 0..127 vs 18 changed in-band power by under 1 dB (mostly within 2 to 3 standard
errors) at all three frequencies. It sits in the 0x401xxx DFE block, so it is likely a digital setting, not RF tuning.

## Hardware check 2026-10-25: ADPLL tracking word, KDCO, VCO range (mmtest MM_VCOMAP / MM_TSTEP)
- 0x402068[8:0] with 0x402050[29:28]=2 is the ADPLL tracking (DCO) word, centre 0x100, not a frequency counter.
  It settles within 1 ms of an FCW nudge (0x402060, 0x100000 = 1 MHz; +5 MHz moved it 256 -> 323 at 900 MHz).
  Raw code: same response whatever 0x40205c holds. hwtimer 0x406040 = 1.0002 MHz (fw "ticks" are us).
- Setting 0x401308 b0 / 0x40130c b3 (as lphy_pll_kdco_cal does) freezes the readback at 0x100 while we stream.
- Host replica of lphy_pll_kdco_cal at 900 MHz gives 0x6c66 vs fw boot 0x674c..0x6799 (~5%).
- Calibrated 0x40205c follows K = 2.0429e13 / f_MHz^3 within ~2% over 770-1095 MHz (LC-DCO gain ~ f^3).
- Stale KDCO is a real hop bug: after a <=880 hop leaves the 0x19000 seed, hops to 1020-1120 MHz lock at N-1
  (VCO exactly 32 MHz low, LO 16 MHz low, T centred so nothing looks wrong). Seeding K(f) before acquisition and
  re-calibrating after fixes it: 750-1100 MHz exact (+-0.016%) including 115-355 MHz jumps; hop 61 ms, 0 laps.
- Range on the x2 path with band field 0x402054[10:6] = 20/22 and coarse code 0x402058[16:10] = 0: floor ~750 MHz
  (745 fails, VCO 1.49 GHz); ceiling a cliff between 1100 (T centred) and 1115 MHz (railed, VCO -4%).
  So the 1.1-1.49 GHz hole is real for these settings (x2 path VCO > 2.2 GHz, direct path VCO < 1.49 GHz).
  Untested levers: band field 0x402054[10:6] above 22, fw coarse tune (uphy_pll_coarse_tune 0x5048e6).
- Top end follow-up (same day, mmtest MM_BANDSWEEP / MM_CCSWEEP, hooks MM_BAND54 / MM_CC in setLo, >1 GHz only):
  0x402054[10:6] at 1100 MHz: 16, 18, 20-29 lock exactly (T centred, gain unchanged), 17/19 fail, 30/31 stop the
  oscillator (fs ~0.1 MS/s). At 1150 no value locks; several pin at VCO 2206.1 MHz with T railed. So the field is
  not a frequency band (looks like a bias/amplitude setting, inferred).
  Signed coarse bank 0x402058[16:10] (sign 0x402064 b4), -127..+64: 1120 locks with -64/-16 but not 0/-8/-32/-48/+16
  (coin flip, no order); 1150 / 1200 never lock. Highest VCO in ~60 attempts: 2240 (locked), 2236 (not).
  Conclusion: VCO ceiling ~2.21-2.24 GHz, LO ~1100-1120 MHz on the x2 path. The old "1.49-1.6 GHz direct path"
  evidence is suspect: DIV2 patches only change the synth word, and the matched 1570/1600 carriers sit near
  32 MHz comb lines (49x, 50x). Untested: 2nd/3rd harmonic mixing with LO 750-1100.

## Hardware check 2026-10-25: RX filter code hunt (mmtest MM_LPF + MM_ABF, paired boot/X/boot PSD, 881.5 MHz LTE, 9.7 MS/s)
- lphy_rf_gain_bw_table_4x21 byte -> 0x402038[27:20] + 0x40203c bytes 0-2 (bw8 idx0 = 8, ~1/BW). Writes verified
  by readback; codes 1..16 (and 32, 0x7f) change the response < 0.7 dB anywhere in +-4.6 MHz. Not in our RX path.
- lphy_rf_set_bw_codes (from lphy_rx_lna_mixer_config): 0x402030[7:0] + 0x402028[15:8] (bw8 0x1e) move the shape
  monotonically but only ~1 dB at 4.6 MHz over 0x0a..0x78. 0x40202c[27:20] + 0x402028[7:0] (bw8 0x0f): 6..255 within
  ~1 dB; 5 and 4 add +10 / +17 dB at 2-3 MHz only (peaking or oscillation onset, not a corner move).
- uphy_rf_set_402000_codes 0x50cf84 (corrected: not TX-only) writes per-bw codes 0x402000[10:3]/[18:11]/[26:19] +
  0x402004[7:0] (2,4,8/9,20 for bw 1/2/4/8) and 0x402004[15:8] (4,8,15,30), gating ADC clock bits b16/b17/b21;
  live chip = bw8 column. A/B 10..80 (plain and with the fw load strobe): no ordered shape change (strobe itself
  causes ADC clips). Not the rolloff either.
- So none of the per-bw code fields sets the +-4 MHz rolloff; no RC-cal message exists in the LPHY loop.
- Per-gain-index fields (lphy_set_rf_gain_index), each swept around its boot value at gain 8, 881.5 MHz:
  0x402018[23:22], 0x40201c[28:25], 0x402020 b10, 0x402024[9:0]/[19:10], 0x402028[26:17], 0x40202c[13:10],
  0x402044[2:0]/[12:3]/[22:13]: all within ~+-1.5 dB shape at 4.6 MHz (null rows up to +-0.7), no ordered trend.
  0x40202c[9:0] (boot 44) = 0 stops the sample stream (mmtest hung; now times out with STALL). Not a filter corner.

## Hardware check 2026-10-26: LO below 750 MHz (VCO floor, hidden divider hunt)
Question: can a larger LO divider (or a lower VCO) reach below 750 MHz? Answer: not with anything we can find.
- LO is always VCO/2 here. lphy_set_lo 0x542e60 and lrom_pll_lo_bringup 0x550210 vary only the word doubling and
  the 880 MHz band split with frequency. No write selects an LO divider.
- VCO floor (mmtest MM_VCOMAP, `cc`/`band54` now also act below 750 MHz):
   - Stock: 750 locks with T -34 (edge); 745 locks on some boots; 0 of 50 attempts at 740 or below locked.
   - Tried: stock, coarse bank 0x402058[16:10] at -127..+64 (10 codes x 745/735/720), band field 0x402054[10:6]
     16-31 at 745, and band 25/28 maps 745-700.
   - Unlocked points always sit at VCO >= ~1.48-1.50 GHz. The floor is ~1.49 GHz, and unlike the top end
     (1120 rescued by cc) no knob moves it.
- Divider hunt, statics: in 0x402040-0x40207f every word is referenced by the PHY image except 0x402040 and
  0x40207c, and the MAC image and main ROM never touch either. Caveat: only base+const accesses are resolved.
- Divider hunt, hardware (mmtest MM_FIELDSCAN):
   - Method: at 851.875 MHz, poke each constant field written by set_lo/bringup, then measure log-PSD
     correlation vs stock, fs (VCO alive), T and the 851.4375 carrier. Re-acquire (same-f hop) after each restore:
     without it, one lock loss contaminated the rest of a run (stock then read fs 7.95, T 0/1).
   - A divider would show corr << 0.5 with fs and T unchanged. Seen: none.
   - Inert (corr 0.77-0.97, fs 8.00 +-0.02, T 254-258; level changes up to ~7 dB, bias-like):
     0x40206c[6:4], 0x402020[12:11], 0x402050[14:13], 0x402054[21:18], 0x402054[25:22], 0x402058[19:17],
     0x402058[22:20], 0x402064[10:7], and all 32 bits of 0x402040.
   - 0x402040 is a real R/W register: ffffffff and 5a5a5a5a read back. It just does nothing visible to RX.
   - Break lock (T railed or 0, carrier gone): 0x402068[20:19]=0, 0x402048[4]=1, 0x402050[5:4]=3.
   - Moves the VCO itself (fs 7.71/8.14/8.16 MS/s): 0x402050[3:2] != 1. Loop/VCO setting, not an output divider.
- Not tested: 0x40207c. It sits next to RF power enables 0x402078 (0x1fffff) and the TX FSM 0x402070/74, and
  the LNA is on the RF port.
- Conclusion (assumed from the above): a fixed /2 quadrature divider, no selectable /4. Coverage stays ~745-1120 MHz.
