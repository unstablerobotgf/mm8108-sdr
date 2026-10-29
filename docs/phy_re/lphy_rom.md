# LPHY ROM (0x550000-0x559fff)

Binja DB `1fd851abcb69f454:ELF`, region owner: this report. Not saved (coordinator saves).
Static analysis only; nothing here was bench-tested. "Verified" below means verified by disassembly/readback of
the ROM image, not on hardware.

## Overview: the ROM is two images

The 40 KB LPHY ROM is **two separately linked images**, not one library:

| Range | Content |
|---|---|
| 0x550000-0x5532df | **Image 1: shared runtime library** (`common/libc/morse_libc_rom.c` string at 0x553304). libc, printf core, clocks/timers, intc, hw semaphores, PRNG, object table, hook thunks, aux ADC, ADC/resampler/DFE/capture helpers, Q16 fixed-point math, libgcc `__divdi3`/`__udivdi3`. All 31 ROM entry points that LPHY RAM calls live here. |
| 0x5532e0-0x55437b | Image 1 rodata: jump tables, strings ("abort", libc file name, digit tables, "<NULL>"), DFE/ADC coefficient tables, `__clz_tab` (0x554214), clock tables, `.eh_frame` (0x554330, FDEs exactly cover `__divdi3` 0x552be4+0x39e and `__udivdi3` 0x552f82+0x35e). |
| 0x55437c-0x55804f | **Image 2: a complete ROM-resident LPHY firmware** for hart 3: crt0, trap handling, mailbox IPC, msg loop, LO/channel setup, DC-offset cal, IQ cal, FEM pin control, SW timers, crash dump. Build strings: `rel_rom_b2_0_2_2024_Aug_27_debug`, `2024-08-27 00:00`. |
| 0x558050-0x559e73 | Image 2 rodata: printf strings, mailbox table, IQ-cal tone tables, two RF table sets (gain, bw, misc), ADC code tables, msg jump table, hook default table. |
| 0x559e74-0x559fff | 0xdeadbeef fill (unused). |

Image 2 is **not used by the RAM firmware** (verified: no RAM call/data xref into 0x55437c-0x55804f; RAM only
calls image 1). It is the pre-RAM / fallback LPHY firmware, and it is the clearest "reference implementation"
of what the RAM code at 0x540000 does (same msg IDs 0x3009-0x3016, same DC-cal and IQ-cal algorithms, same
table layouts), so it is the best place to read those algorithms without RAM-side noise.

Hart numbering (verified from strings + per-hart tables): **LPHY = hart 3** ("hart 3 exited with non-zero code"),
UPHY = hart 2, harts 0/1 = host/MAC side.

## Boot / reset / trap

- `lrom_reset_entry` 0x55437c: crt0. Reads 0x200011c bit0 and mhartid, zeroes all regs, gp = 0x548818,
  writes stack canary 0x66a5eea5 x4 at 0x54f680 (stack bottom), sp = 0x54fe80. Zeroes .bss
  [0x5482a0,0x548c70) and [0x548018,0x54829c) with a per-hart **fill engine** (`lrom_hwcmd_issue_1a05`, cmd
  0x1a05, base by hartid: 0xd000, 0xd100, 0x408400, 0x408500). Then `lrom_crt0_call_main` 0x55447e: platform
  init, checks the canary (exit code -0x7f on overflow), calls `lrom_main`.
- Note the stack top 0x54fe80 is exactly the hook table base: the 0x100 B hook table sits right above the stack.
- `lrom_platform_init` 0x5549ac: platform = 0x2d20[13:12] stored in gp-0x573; installs hook defaults
  (`lrom_hooks_install_defaults`), `lrom_trap_init`, sets mie, runtime init, sends boot-ready mailbox
  (hart 0, id 0, payload 3), then waits for 0x401d00 / 0x2000 clock-ready bits and releases hw semaphore 3.
- `lrom_trap_init` 0x554e64: **mtvec = 0x55c000 (AON RAM)**, mie = 0, intc reset, clear own doorbell
  0x406004 b0. The vector table at 0x55c000 is NOT in the ROM; in the RAM ELF 0x55c000 is ordinary AON code,
  so in ROM-only mode something else must populate it (assumed: host load). ROM trap entry stubs (save regs,
  mret), identified by structure; they have no static refs:
  - `lrom_trap_entry_exc` 0x555354 -> `lrom_trap_dispatch` 0x5551e8 (mcause, mepc)
  - `lrom_trap_entry_msi` 0x55510c -> `lrom_mbox_rx_isr` 0x5550c0
  - `lrom_trap_entry_mti` 0x555060 -> `lrom_trap_timer_irq` 0x554bfc (clears mie.MTIE)
  - `lrom_trap_entry_mei` 0x554f78: claim 0x404704, call handler[irq-1] from gp-0x500 (36 entries), complete
    by writing back 0x404704; out-of-range irq -> `lrom_exit(irq+1)`.
- `lrom_trap_dispatch`: async cause 11/7/3 handled as above, sync cause 3 (ebreak) spins forever, anything else
  -> `lrom_unhandled_trap` ("Unhandled Trap %lu from pc=0x%lx mtval=0x%lx for hart=%lu") -> crash dump.
- `lrom_crash_dump_halt` 0x554e90: disables irqs, clears doorbell, stores all GPRs + mepc/mcause/mtval to AON
  0x55c084..0x55c10c with magic **0xfee1dead** at 0x55c084, fence.i, wfi loop. `lrom_assert_fail` 0x554c50
  ("assertion pc:0x%08lx") and `lrom_unhandled_trap` both broadcast panic first (`lrom_panic_broadcast`: sets
  shared flag 0x1d00 and rings every other hart's doorbell).

## Hook / function-pointer table (what RAM installs)

RAM-visible hook table at **0x54fe80** (LPHY DMEM, 64 x 4 B). ROM code calls through 10-byte thunks at
0x55149c + 10*k (`auipc t1; lw t1, slot(t1); jr t1`), slot k = 0x54fe80 + 4k. Image 2 seeds it from
`lrom_hook_defaults_tab` 0x559d64; the RAM firmware overwrites it in RAM init 0x5412c4 -> AON 0x55c08c, which
copies 0x100 B from **0x55c8a4** (verified both copies by disassembly).

| Slot | Addr | Thunk | ROM default | RAM installs | Role (evidence) |
|---|---|---|---|---|---|
| 0 | 0x54fe80 | `lrom_hook_vprintf` 0x55149c | 0x554c08 `lrom_vprintf_uart` | 0x541614 | vprintf (called by `lrom_log_printf`) |
| 1 | 0x54fe84 | `lrom_hook_slot1` 0x5514a6 | 0x554c7c (return 0) | 0x541774 | unknown, stub in ROM |
| 2 | 0x54fe88 | `lrom_hook_slot2` 0x5514b0 | 0x554c8c (return 0) | 0x541784 | unknown |
| 3 | 0x54fe8c | `lrom_hook_slot3` 0x5514ba | 0x554c94 (ret) | 0x54178c | unknown |
| 4 | 0x54fe90 | `lrom_hook_slot4` 0x5514c4 | 0x554c84 (return 0) | 0x54177c | unknown |
| 5 | 0x54fe94 | `lrom_hook_fatal` 0x5514ce | 0x554c50 `lrom_assert_fail` | 0x541740 | fatal/assert (11 ROM callers) |
| 6 | 0x54fe98 | `lrom_hook_vsnprintf` 0x5514d8 | 0x554c48 -> vsnprintf_core | 0x541654 (j vsnprintf_core) | vsnprintf (renamed from "trace") |
| 7 | 0x54fe9c | `lrom_hook_slot7` 0x5514e2 | thunk itself (would spin) | 0 | unused |
| 8 | 0x54fea0 | `lrom_hook_rand` 0x5514ec | thunk itself | 0x541f3c (uses xoroshiro) | random number (inferred from RAM impl) |
| 9 | 0x54fea4 | `lrom_hook_trace_event` 0x5514f6 | 0x5552a8 `lrom_trace_event` | 0x541ed8 | (id,arg) event trace |
| 10 | 0x54fea8 | `lrom_hook_get_time_base` 0x551500 | 0x55531c | 0x541f88 | 64-bit time base |
| 11 | 0x54feac | `lrom_hook_mbox_send` 0x55150a | 0x554aa8 `lrom_mbox_send` | 0x541490 | IPC send |
| 12 | 0x54feb0 | `lrom_hook_get_platform` 0x551514 | 0x554b68 | 0x541554 | platform (0x2d20[13:12]) |
| 13 | 0x54feb4 | `lrom_hook_slot13` 0x55151e | thunk itself | 0 | unused |
| 14 | 0x54feb8 | `lrom_hook_get_rom_state` 0x551528 | 0x555298 (returns 0x548018) | 0x541ec8 | state block ptr |

Within the ROM only slots 0, 5, 6 are called from code; the rest form an exported API (assumed: called by
other code via the table). Note the existing DMEM data symbol `lphy_ram_hook_trace_ptr` at 0x54fe98 (outside
my range) is really the **vsnprintf** slot.

## RAM -> ROM calls (LPHY RAM 0x540000-0x546ab2), ranked

537 call xrefs out of RAM; 160 land in the ROM, on 31 distinct functions (all image 1, all previously named):

| Calls | ROM fn | RAM caller functions |
|---|---|---|
| 35 | lrom_memcpy | 0x541490 0x5416d8 0x541790 0x5417fc 0x54189c 0x541970 0x5419e0 0x54235c 0x544eec |
| 26 | lrom_fx_mul | 0x540e4c 0x540ff0 0x541138 0x5411ac 0x54377c 0x5438c2 0x543b08 0x544c64 0x544dfc 0x544eec 0x5460a4 |
| 18 | lrom_delay_hwtimer_ticks | 0x541204 0x542e60(set_lo) 0x5431c8 0x5437f0 0x544984 0x544dfc 0x54623c |
| 17 | lrom_memset | 0x54291c 0x5454b4 0x54666e |
| 17 | lrom_delay_us64 | 0x540e4c 0x540ff0 0x541094 0x542e2c 0x542e60 0x5447bc 0x545e14 0x54666e |
| 12 | lrom_fx_div_q16 | 0x540e4c 0x5411ac 0x54377c 0x5438c2 0x543b08 0x544c64 0x544eec 0x546578 |
| 8 | lrom_fx_sqrt | 0x54365c 0x5438c2 0x544c64 |
| 4 | lrom_fx_cos_q16 | 0x54365c 0x54377c 0x543b08 0x544eec |
| 3 | lrom_irq_restore / lrom_vsnprintf_core / lrom_rxcap_set_mode / lrom_dfe_config_decimator / lrom_fx_tan_q16 | |
| 2 | lrom_irq_save, lrom_intc_enable_irq, lrom_rxcap_reset, lrom_dfe_set_lo_ratio_401354, lrom___udivdi3 | |
| 1 | clk_measure_with_freq_counter (msg loop 0x5401e8), hwtimer_read64, hwtimer_disarm_compare, log_printf, snprintf_via_hook, intc_reset_all, intc_set_priority, prng x2 (0x541f3c), objtab_lookup, dfe_load_test_tone_pattern, dfe_decimator_disable, fx_sin_q16 | |

RAM also reads ROM **data**: 0x543f74 uses `lrom_adc_coef_tabs_common` 0x5534cc..0x55350c (5 x 16 B), and RAM AON
0x55c9a4..0x55ca3c holds pointers to the 3 x 13 frequency-band ADC coefficient tables at 0x55351c..0x55377c.
RAM 0x546a76 uses `lrom___clz_tab` 0x554214 (its own 64-bit divide). The fx-heavy RAM callers (0x54377c,
0x543b08, 0x544eec with cos/tan/div) match the IQ-cal coefficient math in ROM `lrom_iqcal_coef_update`
(1/(g cos phi), tan phi), so they are very likely the RAM IQ-cal (inferred).

## ROM image 2 message loop (`lrom_main` 0x5544cc)

Messages arrive by mailbox: doorbell IRQ (MSI) -> `lrom_mbox_rx_isr` copies hart-3 slot 0x54e000 into the
queue at gp-0x790 (64 B entries, payload <= 0x38) under hw semaphore 3. Main loop pops (`lrom_msgq_pop`), sleeps
with wfi when idle. Reply to UPHY = `lrom_msg_send_done_300a` (hart 2, id 0x300a). Jump table
`lrom_main_msg_jumptab` 0x559d2c (14 entries, 0x3009..0x3016):

| Msg | Handler | Action |
|---|---|---|
| 0x3009 | `lrom_enter_sleep` 0x557bb0 | irqs off, 0x408084 = 0x548018, notify UPHY (msg 0x5000), wfi (inferred: sleep/retention) |
| 0x300c | `lrom_msg_set_channel` 0x557374 | f = arg0+arg2, bw = arg1, gain = byte; `lrom_dfe_chain_setup` + `lrom_set_channel` |
| 0x300d | `lrom_iqcal_run` 0x556c6c | IQ cal into capture buf 0x548470 |
| 0x300f | config load | `lrom_cfg_load_defaults`, board type -> 0x55c2a0, power table, `lrom_pwr_struct_init_db` |
| 0x3010 | `lrom_fem_mode_from_board` + `lrom_select_rf_tables` | board type -> FEM mode 0..6, RF table set |
| 0x3012 | `lrom_auxadc_read_temp` (+ `lrom_auxadc_read_supply`) | temperature / supply measurement |
| 0x3013 | `lrom_clk_measure_with_freq_counter(0x400)` | clock measurement |
| 0x3016 | `lrom_msg_dccal` 0x5572d8 | DC-offset cal over 21 gain indices |
| 3 / 0x19 / 0x800e | inline | 0x19 replies with an (id 4) status block; 0x800e echo; else error 0xffff800b |

Also in the loop: when software timer 2 fires and 0x408000[10:7] == 1, it computes an RSSI-like value
(0x401934, gain index from 0x403004 [7:2] or [25:20] by 0x403000 b5, clamped 21) into 0x548008 (inferred).

## Calibration / DSP / RF helpers found in the ROM

- **LO**: `lrom_pll_lo_bringup` 0x550210 (only caller `lrom_lo_set_and_store`): word = round(f*2^24/32e6),
  doubled if f <= 1 GHz, band fields fixed at the >880 MHz values (0x402054[15:6]=0x580), 0x40205c = 0xbe00
  (RAM uses 0x19000/0x7000, so ROM band handling differs), full ADPLL reset sequence (0x402048 b0/b1,
  0x402050 b31, 0x402054 b0..b3, 1-20 us steps; hwtimer ticks on platform 2). `lrom_lo_retune_rx` 0x555404
  gates 0x402050 b26 + 0x405008 b24 off/on around the retune (this is the "set b26 afterwards or fs = 0"
  behaviour seen on the bench). `lrom_lo_retune_loopback` 0x555c2c does the same with 0x402050 b27 / 0x409468 b5.
- **Resampler**: `lrom_resampler_set_ratio_409404` 0x551ca0 confirms 0x409404 = 2^32 f/1024 MHz below 1 GHz;
  above 1 GHz uses f>>11 (f/2048) or f/1280 above 3 GHz or for bw code 20. Reset/enable via 0x409400
  (`lrom_resampler_reset` b2/b5 pulses + b7 mode, `lrom_resampler_enable` b6|b3).
- **ADC**: `lrom_adc_set_loop_codes` 0x5578c4 (ROM twin of RAM 0x544380, same layout: clkdiv {3,2,1,0} +
  12 x 10-bit codes), `lrom_adc_set_cal_codes_402000` 0x5577a0 (0x402000 bytes and 0x402004[15:0] with a
  b16/b17/b21 start sequence + 30 us), `lrom_adc_config_bw` 0x551940 (by log2 bw: 0x402004[20:18], 0x402000
  fields, 0x402008 codes from the frequency-band tables picked by `lrom_select_adc_coef_tables_by_freq`
  0x5517a4 with breakpoints 818 MHz and 890 MHz).
- **DC offset cal** (`lrom_dccal_run` 0x556be8 -> `lrom_dccal_loop_gain_indices` 0x556740): for each of 21
  gain indices, apply the gain row (`lrom_rf_apply_gain_index`), for each 0x402044[27:23] stage in
  {1..12} measure mean I/Q (`lrom_rxcap_mean_iq`: snapshot capture 128<<k samples, 64-bit sums) with DAC
  codes (0,0) and (8,8), solve the zero-crossing with `__divdi3`, clamp (+-63, or +-511 if chip rev > 7), pick
  the stage with smallest residual. Results: AON 0x55c204 (I mag x21), 0x55c219 (Q), 0x55c22e (sign I),
  0x55c243 (sign Q), 0x55c258 (stage). `lrom_dccal_apply_codes` 0x55668c writes them back to
  0x402018[29:24]/b20, 0x40201c[5:0]/0x402018 b21, 0x402044[27:23] and re-enables the servo (0x401334 b0).
- **IQ cal** (`lrom_iqcal_run` 0x556c6c): sets up internal loopback (`lrom_loopback_enable` 0x55214c,
  0x402050 b27, 0x409468 bits, 0x405000/4 TX-ish fields), plays the 10-sample complex tone
  (`lrom_iqcal_tone_capture`: tables 0x558184/0x5581ac, written 51x into 0x40135c while a 256-sample snapshot
  runs), measures the image bin with `lrom_dft_bin_magnitude` (sin/cos correlation), and bisects DC-I, DC-Q,
  gain, phase (`lrom_iqcal_search`). Writes 0x401378 (DC) and 0x401374 = (tan phi>>2) | ((1/(g cos phi))>>2)<<16;
  applies small fixed corrections (gain x 0xff43/65536, phase + 0x11e).
- **Aux ADC** (0x405040/4c/50, 0x406500/1c/24/40): `lrom_auxadc_calibrate` two-reference cal
  (mux 0x80000/0x100000, 25.0/(c2-c1)), `lrom_auxadc_read_temp` (mux 0x100), `lrom_auxadc_read_supply`
  (mux 0x40000, x3.5, inferred supply/VBAT).
- **DFE measurement block**: `lrom_dfe_meas_start` 0x551db4 (0x401900 reset pulse, 0x401908 = len, 0x401904
  b0) and `lrom_dfe_meas_read` 0x551dd8 (copies 10 words 0x40197c..0x4019a0). These are the "4019xx cal-looking
  words" from the bench notes (power/DC estimator results, inferred).
- **Snapshot capture** (distinct from the streaming ring use): `lrom_rxcap_snapshot_start` 0x551d6c
  (0x409044 base, 0x409048 len, 0x40904c b0, clear 0x409000 b0, set 0x401308 b1), `_done` (0x401308 b1 clears),
  `_stop` (0x409000 b0, disable). Decimation for snapshots via 0x40902c.
- **Math**: Q16 atan2 (polynomial, 0xc90f = pi/4), asin, acos, and `lrom_fx_db8_to_lin_q16` 0x557bfc
  (amplitude 10^(x/160): steps 0x2830a per 64, 0x11f3d per 8 = 1 dB, 0x1077a per 2, 0x103b6 per 1).

## Functions named (this pass)

Confidence: H = mechanism fully read, M = role clear but some details assumed, L = structural name.

| Addr | Name | Role / evidence | Conf |
|---|---|---|---|
| 0x550000 | lrom_libc_assert_abort | fatal("abort", libc file, line 0x53), spin | H |
| 0x5501b4 | lrom_wait_2004_bit1_edge | waits for a rising edge on 0x2004 b1 (platform 0/2) | M |
| 0x5501e4 | lrom_pulse_405020_b0_synced | 0x405020 b0 pulse aligned to that edge | L |
| 0x550210 | lrom_pll_lo_bringup | see LO section | H |
| 0x550548 | lrom_clk_sel_2000_mhz | 0x2000[6:4] -> {10,20,40,80,15,30,50} via 0x554314 | M |
| 0x5506b8/0x5506cc | lrom_hwsem_global_lock/unlock | test-and-set reg 0x1d2c (read old, write 1) | H |
| 0x5506dc/0x5506f4/0x550708 | lrom_hwsem_lock/unlock/is_locked | per-index reg 0x1d30+4n (n = hart for mbox) | H |
| 0x55071c/0x550734 | lrom_hwsem2_lock/unlock | regs 0x1d1c+4n | H |
| 0x550748 | lrom_hwreg_1d18_inc_wait | increments 0x1d18 and waits | L |
| 0x55076c/0x550850 | lrom_sync_barrier_master/join | multi-hart rendezvous on 0x1d40/0x1d44/0x1d18 + 0x401a40/44 | L |
| 0x5508c4..0x550964 | lrom_shlock_1d14/1d10/1d0c/1d04_* | amoswap spinlocks in shared RAM | H |
| 0x550974/0x550980/0x550990 | lrom_shcnt_1d08_read/inc/dec | amoadd counter 0x1d08 | H |
| 0x5509a0/0x5509b0 | lrom_panic_flag_1d00_set/get | global panic flag 0x1d00 | H |
| 0x5509d4 | lrom_cfg_20f8_405030 | 0x20f8 fields, 0x1304/1308 clear, 0x405030 b18 | L |
| 0x550a40 | lrom_clk_2108_khz | 0x2108[5:1] x 8000 kHz | M |
| 0x550adc | lrom_rdcycle64 | mcycleh/mcycle consistent read | H |
| 0x550b64 | lrom_delay_ns64 | busy-wait arg*cyc_per_us/1000 on mcycle | H |
| 0x550c10 | lrom_cfg_4108_by_platform | 0x4108[15:0] = 0x20 or 0x54 (platform 1) | L |
| 0x550d00 | lrom_popcount | | H |
| 0x550d18 | lrom_strncmp | | H |
| 0x550d40 | lrom_strnlen | | H |
| 0x550d6a | lrom_memcmp | | H |
| 0x550dd8 | lrom_fmt_skip_atoi | printf width parser | H |
| 0x550e24 | lrom_fmt_number | printf integer formatter (uses digit tables) | H |
| 0x551170 | lrom_snprintf | direct snprintf -> vsnprintf_core | H |
| 0x551190 | lrom_memmove | | H |
| 0x5511fc | lrom_memcmp_reverse | compares from last byte (LE bignum compare) | H |
| 0x551234/0x551264 | lrom_load_be32_off2(_a) | BE u32 from bytes 2..5 | H |
| 0x5512b0 | lrom_intc_set_threshold | 0x404700 | M |
| 0x5512dc | lrom_intc_disable_irq | clear bit in 0x404440[] | H |
| 0x551300 | lrom_intc_disable_all | 0x404440/44 = 0 | H |
| 0x5513bc/0x5513d8/0x55142e/0x55145a | lrom_objtab_is_set/init/register/replace | {ptr[], count} table, entry byte0 = id | H |
| 0x5514a6..0x551528 | lrom_hook_* thunks | see hook table | H |
| 0x551534 | lrom_auxadc_enable | 0x405040/4c/50, 0x406500 | M |
| 0x5515a0 | lrom_auxadc_convert | mux 0x405040[24:9], start 0x40651c, busy 0x406540, data 0x406524 | M |
| 0x5515ec | lrom_auxadc_calibrate | two references -> Q16 gain | M |
| 0x551688 | lrom_auxadc_read_supply | | L |
| 0x551714 | lrom_auxadc_read_temp | msg 0x3012 | M |
| 0x5517a4 | lrom_select_adc_coef_tables_by_freq | | H |
| 0x551928 | lrom_rmw_401d08_mode | 0x401d08[3:0] | L |
| 0x551940 (+j 0x551b8c) | lrom_adc_config_bw | jump table 0x55337c over bw {1,2,4,8,20} | M |
| 0x551b90/0x551ba4 | lrom_adc_disable/enable | 0x402000 b0-2, 0x402010 b28, 0x402004 b17 | M |
| 0x551bd8 | lrom_adc_clk_start | 0x402004 b21/b22/b17, clear b16 | L |
| 0x551c18/0x551c80/0x551ca0/0x551e40 | lrom_resampler_reset/enable/set_ratio_409404/set_b4 | 0x409400/0x409404 | H |
| 0x551d6c/0x551edc/0x551efc | lrom_rxcap_snapshot_start/done/stop | 0x409044/48/4c, 0x401308 b1 | H |
| 0x551d98 | lrom_rmw_402028_402020_enable | 0x402028 b16, 0x402020 b28 | L |
| 0x551db4/0x551dd8 | lrom_dfe_meas_start/read | 0x4019xx | M |
| 0x551e00 | lrom_cfg_405000_fields | | L |
| 0x551e54/0x551e90 | lrom_bwparam_init(_from_table) | 8-word struct from 0x5533d0 tables (21 bw codes) | L |
| 0x551eb0 | lrom_dfe_set_401384 | | L |
| 0x551f18/0x55214c | lrom_loopback_disable/enable | 0x409468 b1/5/6/7/11, 0x402050 b27, 0x405008 b24, 0x401d00 b12, GPIO 0x1308/0x130c | M |
| 0x5520d8 | lrom_dfe_test_tone_flush | 0x401308 b18 + n zero writes to 0x40135c | M |
| 0x552274 | lrom_cfg_409468_b6_b7 | | L |
| 0x5522fc | lrom_dfe_enable_401308_b3 | also 0x409468 b1, 0x40132c b1 | L |
| 0x5523c4/0x5523f0 | lrom_pulse_409468_b0 / lrom_flush_401324 | | L |
| 0x552534 | lrom_dfe_set_bw_mode | 0x40130c[1:0], 0x401308[26:20] from 0x55431c | M |
| 0x552aa8/0x552b48/0x552b9c | lrom_fx_atan2/acos/asin_q16 | | H |
| 0x552be4 | lrom___divdi3 | signed 64-bit div, eh_frame FDE matches | H |
| 0x55437c/0x55447e/0x5544cc | lrom_reset_entry/crt0_call_main/main | | H |
| 0x5548cc | lrom_lo_set_and_store | bringup + save f at gp-0x578 | H |
| 0x5548ec/0x554960 | lrom_uart_puts(_locked) | UART 0x1500 | H |
| 0x554980 | lrom_exit | prints exit code, sends exit mbox, wfi | H |
| 0x5549ac | lrom_platform_init | | H |
| 0x554a88 | lrom_assert_hart3 | | H |
| 0x554aa8/0x554b78 | lrom_mbox_send(3) | | H |
| 0x554b58/0x554b68/0x554b70 | lrom_set_chip_info/get_platform/get_chip_rev | 0x2d20 word, [13:12], [11:8] | H |
| 0x554b94 | lrom_msg_unpack3 | | M |
| 0x554bb4/0x554bcc | lrom_mbox_send_boot_ready/exit | hart 0 id 0/1 | H |
| 0x554be8 | lrom_mbox_slot_addr | | H |
| 0x554bfc | lrom_trap_timer_irq | | H |
| 0x554c08 | lrom_vprintf_uart | 256 B buffer -> UART | H |
| 0x554c50 | lrom_assert_fail | | H |
| 0x554c98/0x554cf4/0x554d28 | lrom_cfg_lookup/get_or_default/init | board config blob, objtab at +0x258 | M |
| 0x554d60/0x554dcc/0x554e58/0x557ff0 | lrom_msgq_push/pop/is_empty/push_main | 64 B ring entries | H |
| 0x554e64 | lrom_trap_init | | H |
| 0x554e90 | lrom_crash_dump_halt | | H |
| 0x554f78/0x555060/0x55510c/0x555354 | lrom_trap_entry_mei/mti/msi/exc | | H |
| 0x55502c/0x557ae0 | lrom_irq_register / lrom_irq32_register | handler table gp-0x500 | H |
| 0x5550c0 | lrom_mbox_rx_isr | | H |
| 0x55516c | lrom_panic_broadcast | | H |
| 0x5551b4/0x5551e8 | lrom_unhandled_trap / lrom_trap_dispatch | | H |
| 0x5552a8/0x5552e0/0x5552f8 | lrom_trace_event/publish_count/reset | 16 x {time 0x406040, id, arg} at AON 0x55c114, count to 0x55c110 | H |
| 0x555310/0x55531c | lrom_set/get_time_base | | M |
| 0x555330 | lrom_mbox_send_uphy_ready | hart 2 msg 0x5000 | M |
| 0x55534c | lrom_set_cfg_aux_ptr | | L |
| 0x5553a8 | lrom_rf_set_bw_codes_402028 | 0x402028/2c/30 from 0x558174 | M |
| 0x555468 | lrom_fpga_cfg_40940c | only on platform 2 | L |
| 0x555530 | lrom_rf_power_enable_402078 | 21 enable bits of 0x402078 | M |
| 0x555670 | lrom_rf_apply_gain_index | 48 B gain row + 16 B bw row (ptrs 0x55c29c/0x55c298) | H |
| 0x5558ac | lrom_rf_rx_enable | | M |
| 0x5559fc | lrom_set_channel | resampler, LO, ADC, RX enable, ADC codes | H |
| 0x555cb0 | lrom_dfe_chain_setup | 0x401354 ratio, 0x401308[26:20] >1 GHz, 0x409470, 0x401d00[5:3], 0x40139c | H |
| 0x555f04/0x555f10 | lrom_loopback_disable/enable_default | | H |
| 0x555f18 | lrom_rf_set_dc_dac | sign-magnitude I/Q DC DAC codes | H |
| 0x555f7c | lrom_dc_servo_cfg_disable | 0x401334: [4:1]=5, b5, clear b0 | M |
| 0x555fa8 | lrom_dft_bin_magnitude | | M |
| 0x5560c8/0x55613c/0x556200 | lrom_iqcal_coef_update/tone_capture/search | | H |
| 0x556434 | lrom_rxcap_mean_iq | | H |
| 0x5565f0/0x55667c/0x55668c/0x556740/0x556be8 | lrom_dccal_* | | H |
| 0x556c6c | lrom_iqcal_run | | H |
| 0x557138 | lrom_pwr_struct_init_db | fills struct with db8_to_lin values | L |
| 0x5572cc | lrom_store_55c1d8 | | L |
| 0x5572d8 | lrom_msg_dccal | | H |
| 0x5572f8 | lrom_select_rf_tables | | H |
| 0x557364/0x557374 | lrom_msg_send_done_300a / lrom_msg_set_channel | | H |
| 0x5573ac/0x557494/0x5574e8/0x5575a0/0x5575f0 | lrom_fem_set_mode/gpio_oe_clear/drive_pins/mode_from_board/get_mode | GPIO7-12 via 0x1308 (OE), 0x1368 set, 0x136c clear; 0x403018, 0x200c fields | M |
| 0x5575f8/0x5576a4 | lrom_cfg_load_defaults/power_table | config IDs 0,1,2,3,4,5,6,7,0x2a,0x48-0x4a | M |
| 0x5577a0/0x5578c4 | lrom_adc_set_cal_codes_402000 / lrom_adc_set_loop_codes | | H |
| 0x557afc/0x557b18/0x557b38/0x557d38/0x557db4/0x557dbc/0x557dd4/0x557f5c | lrom_swtimer_* | 5 SW timers at 0x548038 (16 B), sorted list, HW compare 0x401b88/8c, enable 0x401b00 b1, IRQ 0x20 | H |
| 0x557b64/0x557b88/0x557b94 | lrom_evflags_clear/get/test_clear | event bits at 0x548000 | H |
| 0x557bb0 | lrom_enter_sleep | | M |
| 0x557bfc | lrom_fx_db8_to_lin_q16 | | H |
| 0x557f7c | lrom_irq30_clear_401a44 | IRQ 30 handler | M |
| 0x557f88 | lrom_runtime_init | | M |
| 0x55801c | lrom_hooks_install_defaults | | H |

Renamed (previous names were wrong):
- 0x5514d8 `lrom_hook_trace` -> **`lrom_hook_vsnprintf`** (slot 6 default = vsnprintf_core; RAM installs `j lrom_vsnprintf_core`).
- 0x550db8 `lrom_log_trace` -> **`lrom_snprintf_via_hook`** (signature buf, size, fmt, ...; forwards va_list to slot 6).

Reviewed and kept: the other 42 prior lrom_ names. Comment added on `lrom_rxcap_reset` 0x552014: it resets
the 0x409468 datapath, not the capture ring; and on `lrom_hwcmd_issue_1a05` (it is the per-hart fill engine).

Not named: 0x557248 (computes a db8_to_lin value from a 0x21-centred index and stores to an address Binja
lost; low evidence); junk "functions" inside rodata (0x553920, 0x554294, 0x55807c..0x5582xx stubs, etc.).

## Data tables and globals

| Addr | Name | Content |
|---|---|---|
| 0x5532e0 | lrom_clk_cpu_khz_jumptab | 7-entry switch for lrom_clk_cpu_khz |
| 0x5532fc / 0x553304 | lrom_str_abort / lrom_str_libc_rom_file | "abort", "common/libc/morse_libc_rom.c" |
| 0x553324 / 0x55334c / 0x553374 | lrom_str_digits_lower/upper, lrom_str_null | printf tables, "<NULL>" |
| 0x55337c | lrom_adc_config_bw_jumptab | 21 rel offsets, cases bw 1,2,4,8,20 |
| 0x5533d0 | lrom_bwparam_tab | 3 x 21-word arrays (0x5533d0, 0x553424, 0x553478) |
| 0x5534cc | lrom_adc_coef_tabs_common | 5 x 16 B ([bw1,2,4,8]), read by RAM 0x543f74 |
| 0x55351c / 0x5535ec / 0x5536bc | lrom_adc_coef_tabs_818_890mhz / above_890mhz / below_818mhz | 13 x 16 B each, values double per bw octave |
| 0x554214 | lrom___clz_tab | libgcc 256 B |
| 0x554314 | lrom_clk_sel_mhz_tab | {20,40,80,15,30,50} |
| 0x55431c | lrom_dfe_bw_mode_tab | 0x401308[26:20] / 0x40130c[1:0] per bw |
| 0x554330 | lrom_eh_frame_libgcc | CIE + 2 FDEs |
| 0x558050/0x55808c/0x5580a4 | lrom_str_hart_exit / assert_pc / unhandled_trap | printf formats |
| 0x55807c | lrom_mbox_addr_tab | {0x112000, 0x12e000, 0x51e000, 0x54e000} per hart |
| 0x5580e0 / 0x5580f4 | lrom_str_build_date / lrom_str_version | "2024-08-27 00:00", "rel_rom_b2_0_2_2024_Aug_27_debug" |
| 0x558174 | lrom_rf_bw_codes_402028_tab | {0x14,0x14,0x14,0x1e} |
| 0x558184 / 0x5581ac | lrom_iqcal_tone_i_tab / q_tab | 10-sample test tone (same shape as RAM 0x55cd04/0x55cd2c) |
| 0x5581d4 | lrom_iqcal_search_consts | 6 words |
| 0x5581ec | lrom_dccal_stage_list | bytes 1..12 (0x402044[27:23] candidates) |
| 0x5581fc / 0x558dac | lrom_rf_tab3_setA/B | 0x280 B each, pointer at AON 0x55c294; never read in ROM |
| 0x55847c / 0x55902c | lrom_rf_bw_tab_setA/B | 4 bw x 21 gain x 16 B (same layout as RAM 0x55d014) |
| 0x5589bc / 0x55956c | lrom_rf_gain_tab_setA/B | 21 x 48 B gain rows (RAM 0x55c254 uses 12 B rows) |
| 0x55995c | lrom_fem_board_jumptab | 24 entries, board type 0..0x17 |
| 0x5599bc | lrom_adc_loop_code_tabs | 12 tables x [3 rows][4 bw] words |
| 0x559c2c | lrom_adc_cal_code_tabs | 5 tables x [3][4] words |
| 0x559d1c | lrom_adc_clkdiv_tab | {3,2,1,0} (same as RAM 0x55ccb0) |
| 0x559d2c | lrom_main_msg_jumptab | 14 entries 0x3009..0x3016 |
| 0x559d64 | lrom_hook_defaults_tab | 0x100 B, see hook table |
| 0x559e68 / 0x559e6c | lrom_tab4_setA/B | 4-byte entries (0, {6,5,4,4}), pointer at AON 0x55c290 |

ROM image 2 RAM usage (not renamed, outside range): DMEM 0x548000 event flags, 0x548018 state block,
0x548038 SW timers, 0x548470 IQ capture buffer, gp = 0x548818, stack 0x54f680-0x54fe80, mailbox in 0x54e000;
AON 0x55c084-0x55c10c crash dump, 0x55c110 trace count, 0x55c114 trace ring (16 x 12 B), 0x55c204-0x55c26c
DC-cal results, 0x55c290-0x55c2a0 table pointers + board type, 0x55c3b0-b8 applied DC codes, 0x55c458/9/c misc.
These overlap the RAM image's AON content, so ROM firmware and RAM firmware cannot run together (verified
overlap; consequence assumed).

## Registers learned (beyond BRIEF)

| Addr | Field | Meaning | Evidence |
|---|---|---|---|
| 0x2d20 | [13:12] | platform (0 silicon?, 1, 2 FPGA-like, 3) ; [11:8] chip rev | set_chip_info, many platform branches |
| 0x1500 | b31 | UART TX busy, write byte | lrom_uart_puts |
| 0x1d00..0x1d44 | | shared semaphores/locks/flags (test-and-set, amoswap) | hwsem/shlock fns |
| 0x4100 / 0x4104 / 0x406000 / 0x406004 | b0 | doorbell to hart 0 / 1 / 2 / 3 | mbox_send, panic_broadcast |
| 0xd000 / 0xd100 / 0x408400 / 0x408500 | +0..+0x10 | per-hart fill engine (cmd 0x1a05) | crt0 |
| 0x404440.. / 0x404700 / 0x404704 | | intc enable bits / threshold / claim-complete | trap_entry_mei |
| 0x406040/44 | | 64-bit HW timer (trace timestamps) | trace_event |
| 0x401b00 b1, 0x401b40 b1, 0x401b88/8c | | compare timer enable / 64-bit compare (IRQ 0x20) | swtimer |
| 0x402050 | b26 / b27 | RX clock gate / loopback-TX path gate | lo_retune_rx / lo_retune_loopback |
| 0x405008 | b24 | paired with 0x402050 b26/b27 | same |
| 0x402018 | [29:24] b20 | DC DAC I magnitude / sign | lrom_rf_set_dc_dac |
| 0x40201c / 0x402018 | [5:0] / b21 | DC DAC Q magnitude / sign | same |
| 0x402044 | [27:23] | DC-cal stage (1..12) | dccal loop |
| 0x402078 | 21 bits | RF block enables (all set together) | rf_power_enable |
| 0x409400 | b2,b5 pulse; b3,b6 en; b7 mode; [9:8] | resampler control | resampler_* |
| 0x40902c | | snapshot decimation | rxcap_mean_iq |
| 0x409024 | [15:0] | level check > 0x6d60 before DC measurement | rxcap_mean_iq |
| 0x401374 / 0x401378 | | IQ coef (tan, 1/(g cos)) / DC correction | iqcal_run |
| 0x401900/04/08, 0x40197c..a0 | | DFE measurement start/len/results | dfe_meas_* |
| 0x405040[24:9], 0x40651c, 0x406540, 0x406524 | | aux ADC mux, start, busy, data | auxadc_* |
| 0x1308 / 0x1368 / 0x136c | | GPIO OE / set / clear (FEM pins 7..12) | fem_* |
| 0x408084 | | written with 0x548018 before sleep (resume ptr, inferred) | enter_sleep |

## Byte signatures for UPHY-ROM twin matching

First 16 bytes (memory order, 16-bit groups) of each lrom_ libc/runtime function, read from the ROM image the DB
was built from (spot-checked equal with bn_memory_read at 0x550042). Functions that contain `auipc`/`jal`
in their first 16 bytes (e.g. libc_assert_abort, panic_*, hook thunks) will differ in those immediates if
the UPHY copy sits at a different offset; mask those when matching.

| Addr | Name | First 16 bytes |
|---|---|---|
| 0x550000 | lrom_libc_assert_abort | `4111 8146 1736 0000 1306 862f 9305 3005` |
| 0x550020 | lrom_hwcmd_issue_1a05 | `0683 1928 9a80 4cc1 10c5 54c5 8962 9382` |
| 0x550038 | lrom_hwcmd_wait_idle | `8322 0501 e39e 02fe 8280 aa83 a146 6360` |
| 0x550042 | lrom_memcpy | `aa83 a146 6360 d60a 9376 3500 13f7 3500` |
| 0x5500fe | lrom_memset | `55ca aa87 a146 6361 d60a 93f6 3700 91ca` |
| 0x550534 | lrom_panic_halt | `4111 8146 0146 0145 06c6 9705 0000 ef00` |
| 0x550548 | lrom_clk_sel_2000_mhz | `8967 9c43 9183 9d8b 99cf fd17 93f7 f70f` |
| 0x550578 | lrom_clk_measure_with_freq_counter | `4111 06c6 1308 f5ff 9307 f03f 63e6 0711` |
| 0x5506b8 | lrom_hwsem_global_lock | `8967 9387 c7d2 8546 3687 2fa7 e708 6dff` |
| 0x5506dc | lrom_hwsem_lock | `8967 0a05 9387 07d3 3e95 0547 ba87 af27` |
| 0x5508c4 | lrom_shlock_1d14_acquire | `8967 9387 47d1 8546 3687 2fa7 e708 6dff` |
| 0x5509bc | lrom_irq_save | `7375 0430 2189 8280 0000 0000 19c1 f367` |
| 0x5509c8 | lrom_irq_restore | `19c1 f367 0430 8280 0000 0000 0146 19cd` |
| 0x550a2c | lrom_panic_bad_clock | `4111 8146 0146 0145 06c6 9705 0000 ef00` |
| 0x550a40 | lrom_clk_2108_khz | `8967 03a5 8710 5d47 0581 7d89 9306 f5ff` |
| 0x550a64 | lrom_clk_source_khz | `4111 06c6 8547 6303 f502 8947 630a f500` |
| 0x550adc | lrom_rdcycle64 | `f325 00b8 7325 00b0 f327 00b8 e39a f5fe` |
| 0x550af0 | lrom_hwtimer_read64 | `b767 4000 a843 ec43 8280 0000 0111 4ac8` |
| 0x550afc | lrom_delay_us64 | `0111 4ac8 4ec6 06ce 22cc 26ca aa89 2e89` |
| 0x550b64 | lrom_delay_ns64 | `0111 4ac8 4ec6 06ce 22cc 26ca aa89 2e89` |
| 0x550bd4 | lrom_delay_hwtimer_ticks | `b767 4000 b843 fc43 b765 4000 3308 e500` |
| 0x550c00 | lrom_hwtimer_disarm_compare | `b767 4000 7d56 fd56 90d7 d4d7 8280 0000` |
| 0x550c3c | lrom_clk_cpu_khz | `4111 22c4 3724 4000 0325 04d0 06c6 0d89` |
| 0x550ce4 | lrom_clk_cpu_cycles_per_us | `4111 06c6 913f b240 9307 803e 3355 f502` |
| 0x550d00 | lrom_popcount | `aa87 0145 99c7 13f7 1700 8583 3a95 e5ff` |
| 0x550d18 | lrom_strncmp | `2e96 09a8 03c7 0500 8347 f5ff 8505 6398` |
| 0x550d40 | lrom_strnlen | `8347 0500 91cf 89cd aa95 aa87 19a0 6386` |
| 0x550d6a | lrom_memcmp | `2a96 29a0 6360 f702 630c a600 0347 0500` |
| 0x550d94 | lrom_log_printf | `3971 1303 4102 2ed2 9a85 06ce 32d4 36d6` |
| 0x550db8 | lrom_snprintf_via_hook | `3971 1303 c102 36d6 9a86 06ce 3ad8 3eda` |
| 0x550dd8 | lrom_fmt_skip_atoi | `1441 2547 aa85 83c7 0600 9387 07fd 93f7` |
| 0x550e24 | lrom_fmt_number | `5971 a2d4 d2cc 86d6 a6d2 cad0 cece d6ca` |
| 0x550f5e | lrom_vsnprintf_core | `3171 5ecf 930b c101 22dd 26db 4ad9 4ed7` |
| 0x551170 | lrom_snprintf | `3971 1303 c102 36d6 9a86 06ce 3ad8 3eda` |
| 0x551190 | lrom_memmove | `aa87 638f a502 0dce 63fe a500 3307 b540` |
| 0x5511fc | lrom_memcmp_reverse | `1307 f6ff b307 e500 ba95 0dc2 7d15 29a0` |
| 0x551234 | lrom_load_be32_off2_a | `0348 2500 8347 5500 0346 3500 0347 0500` |
| 0x551264 | lrom_load_be32_off2 | `8347 2500 0347 3500 8346 5500 0345 4500` |
| 0x551284 | lrom_intc_reset_all | `3747 4000 2320 0744 b747 4000 2322 0744` |
| 0x5512b0 | lrom_intc_set_threshold | `b747 4000 23a0 a770 8280 0000 9357 5500` |
| 0x5512bc | lrom_intc_enable_irq | `9357 5500 3747 4000 1307 0744 8a07 ba97` |
| 0x5512dc | lrom_intc_disable_irq | `1357 5500 b747 4000 9387 0744 0a07 3e97` |
| 0x551300 | lrom_intc_disable_all | `b747 4000 23a0 0744 23a2 0744 8280 0000` |
| 0x551310 | lrom_intc_set_priority | `0a05 b747 4000 3e95 0cc1 8280 0000 0000` |
| 0x551320 | lrom_prng_xoroshiro64ss_next | `1c41 3787 379e 1307 b79b 3387 e702 4c41` |
| 0x551368 | lrom_prng_next_bounded | `1c41 3787 379e 1307 b79b 3387 e702 0328` |
| 0x5513bc | lrom_objtab_is_set | `5c41 63e9 b700 1c41 8a05 ae97 8843 3335` |
| 0x5513d8 | lrom_objtab_init | `9397 2500 63e6 f600 10c1 4cc1 0145 8280` |
| 0x5513f0 | lrom_objtab_lookup | `1dca 5c41 63e9 b702 1c41 1397 2500 ba97` |
| 0x55142e | lrom_objtab_register | `8347 1600 91cf 5c41 63ea b700 1c41 8a05` |
| 0x55145a | lrom_objtab_replace | `8347 1600 9dcb 5c41 63ed b700 1c41 1397` |
| 0x55149c | lrom_hook_vprintf | `17f3 ffff 0323 439e 0283 17f3 ffff 0323` |
| 0x5514ce | lrom_hook_fatal | `17f3 ffff 0323 639c 0283 17f3 ffff 0323` |
| 0x5514d8 | lrom_hook_vsnprintf | `17f3 ffff 0323 039c 0283 17f3 ffff 0323` |
| 0x552594 | lrom_fx_mul | `1357 f541 b347 a700 93d6 f541 998f 33c7` |
| 0x5528e8 | lrom_fx_div_q16 | `aa88 adc5 93d7 f541 1356 f541 33c7 b700` |
| 0x55295c | lrom_fx_sqrt | `9357 f541 33c7 a700 1d8f b706 1000 aa85` |
| 0x5529d8 | lrom_fx_sin_q16 | `b757 0600 4111 9387 e787 22c4 3364 f502` |
| 0x552a74 | lrom_fx_cos_q16 | `e567 9387 f721 3e95 b1bf 0000 4111 06c6` |
| 0x552a80 | lrom_fx_tan_q16 | `4111 06c6 22c4 26c2 aa84 b937 e567 9387` |
| 0x552aa8 | lrom_fx_atan2_q16 | `9356 f541 4111 33c7 a600 26c2 158f 06c6` |
| 0x552b48 | lrom_fx_acos_q16 | `4111 26c2 c164 06c6 22c4 3307 9500 b707` |
| 0x552b9c | lrom_fx_asin_q16 | `4111 26c2 c164 06c6 22c4 3307 9500 b707` |
| 0x552be4 | lrom___divdi3 | `2a83 2e8e 8148 63db 0500 b337 a000 330e` |
| 0x552f82 | lrom___udivdi3 | `aa88 2e83 3287 2a88 ae87 6396 0620 b745` |

Pairs with identical first 16 bytes (need more bytes to disambiguate): delay_us64/delay_ns64,
snprintf_via_hook/snprintf, panic_halt/panic_bad_clock, fx_acos/fx_asin.

## Open questions

- Who populates the vector table at 0x55c000 in ROM-only mode, and when is ROM image 2 actually run (cold
  boot before host loads RAM? recovery?). The RAM loader evidently replaces AON wholesale.
- Hook slots 1-4, 7, 13: RAM installs real functions for 1-4 (0x541774/84/8c/7c); their purpose is unknown
  (ROM defaults are return-0 stubs). Look at those RAM functions.
- `lrom_rf_tab3_set*` (0x280 B) and `lrom_tab4_set*` are selected but never read inside the ROM; maybe TX
  tables consumed by UPHY or by RAM.
- Platform encoding 0x2d20[13:12]: 2 enables extra FPGA-only register writes and hwtimer-tick delays (looks
  like an emulation platform); confirm which value the live chip reports (cheap readback).
- Aux ADC scaling constants (25.0, 3.5, 356/2.9) give units only by inference; a bench readback of msg 0x3012
  output against a known temperature would settle them.

## Notes for other regions (do not rename from here)

- LPHY RAM 0x5412c4 calls AON 0x55c08c, which installs the RAM hook table from **0x55c8a4** into 0x54fe80
  (slots listed above). RAM hook impls: 0x541614 vprintf, 0x541740 fatal, 0x541654 vsnprintf, 0x541f3c rand,
  0x541ed8 trace_event, 0x541f88 time_base, 0x541490 mbox_send, 0x541554 platform, 0x541ec8 state ptr.
- DMEM data symbol `lphy_ram_hook_trace_ptr` (0x54fe98) is actually the vsnprintf slot.
- RAM 0x543f74 reads ROM tables 0x5534cc..0x55350c; RAM AON 0x55c9a4..0x55ca3c = pointers to ROM ADC coef
  tables by frequency band (<=818, <=890, >890 MHz).
- RAM 0x546a76 uses `lrom___clz_tab` (RAM has its own 64-bit divide using the ROM table).
- UPHY ROM should contain the same image-1 library (morse_libc_rom.c) and likely a ROM image 2 for hart 2;
  look for the same crt0 pattern (`lw s1,0x11c(0x2000)`, mhartid, canary 0x66a5eea5) and an mbox table
  {0x112000, 0x12e000, 0x51e000, 0x54e000}. 0x51e000 is the hart-2 mailbox slot, which explains why the
  capture ring at 0x51e000 collided with live state in the bench notes (verified address match, consequence
  inferred).
- UART at 0x1500 (bit31 busy) is the "dead UART" the parked UPHY spun on.
