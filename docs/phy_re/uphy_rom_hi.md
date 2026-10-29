# UPHY ROM, upper part (0x52c000-0x537fff), prefix `urom_`

## Overview

The big finding: **0x52cf10-0x535000 is a complete, self-contained UPHY application baked into mask ROM**
(build strings at 0x535504 `2024-08-27 00:00` and 0x535518 `rel_rom_b2_0_2_2024_Aug_27_debug`). It has its own
crt0 (`urom_app_start` 0x52cf10: gp=0x510808, sp=0x51ee80 with stack canary 0x66a5eea5, per-hart init),
`main` (0x52d068), trap/IRQ layer, mailbox, PSM driver, AGC/DC/ADC/FEM setup, RX and TX paths and a host
command dispatcher. It is an older build of the same source tree as the UPHY RAM firmware (0x500000-0x50fb9e):
signature matching (registers, AON globals, ROM callees, constants) pairs most of its functions with RAM
functions, several at 0.8-1.0 similarity (table below, column "RAM twin").

**Verified (Binja call xrefs): UPHY RAM never calls anything in 0x52c000-0x537fff.** All 385 call refs into the range
come from inside the range; the only outside code refs are auipc halves of unrelated address computations. RAM calls
stop at 0x52ada0 (lower ROM library). So at runtime with the RAM image loaded, this region is **dead code**; it is
only the boot-ROM fallback app. Its value is as a rosetta stone: it is smaller, has real strings, and shows the
same register sequences the RAM app uses.

What the RAM firmware *does* share from this range is rodata: 0x52cca4 (`__clz_tab`, used by RAM 0x50fb62 and the
lower-ROM 64-bit divide) and the packet config tables at 0x52cdb8-0x52cdf8 (used by RAM 0x502bde and read by
`uphy_main_loop` 0x5001e8 at 0x52cdd0).

Range layout:
- 0x52c000-0x52cf0f: rodata belonging to the **lower ROM library** (0x520000-0x52bfff): u32 tables at 0x52c000,
  jump tables 0x52c09c/0x52c928 (auto), pointer table 0x52c9c0-0x52ca3c to u16 arrays, 8-word constant blocks
  0x52cb64/0x52cc84, `urom_clz_tab` 0x52cca4 (256 B), pkt cfg tables 0x52cdb8. Linear-sweep "functions" here
  (0x52c000.., 0x52cd24, 0x52cdb4, 0x52ce18 etc.) are junk.
- 0x52cf10-0x534fff: ROM app code (~150 real functions, 144 named).
- 0x535000-0x53741b: ROM app rodata (strings, mailbox table, PSM tables, AGC/ADC/DFE tables, param table,
  hook table, .data image). Junk functions 0x536714, 0x53730c, 0x537416 are inside these tables.
- 0x53741c-0x537fff: 0xdeadbeef fill.

## Architecture of the ROM app (all verified by decompile unless marked)

- **Boot**: `urom_app_start` -> `urom_app_main_wrapper` -> `urom_app_init` (reads chip id 0x2d20, `urom_hooks_init`
  copies 64 fn ptrs 0x5372ec -> DMEM 0x51fe80, `urom_trap_init` sets mtvec=0x538000 (AON), `urom_platform_init`
  registers IRQs, then waits for 0x401d00/0x2000 clock-ready bits) -> `urom_app_main_loop` -> `urom_app_exit`
  (prints "hart 2 exited with non-zero code", so UPHY = hart 2).
- **Hook table** 0x51fe80 (from 0x5372ec): +0 vprintf 0x52e96c, +0x14 assert 0x52e9cc, +0x24 trace 0x52f148,
  +0x2c mbox_send 0x52dfb0, +0x30 chip variant 0x52e07c, plus app callbacks at +0x60.. (set_lo 0x52dcd8 etc.).
  Same scheme as LPHY's 0x54fe80/94/98 hook pointers: the lower ROM library calls the app through it (inferred).
- **Mailbox**: `urom_mbox_send(hart, type, len<=0x38, payload)`: buffer = 0x535030[hart] = {0x112000, 0x12e000,
  0x51e000, 0x54e000} for hart 0 host, 1 MAC, 2 UPHY, 3 LPHY; doorbell = 0x4100, 0x4104, 0x406000, 0x406004 bit0.
  So each core's inbox sits in its own DMEM (UPHY 0x51e000, LPHY 0x54e000). Inbound: machine software IRQ ->
  `urom_isr_software` -> `urom_mbox_rx_handler` (reads 0x51e000, clears 0x406000 bit0) -> `urom_mbox_rx_dispatch`
  (0x300a = LPHY ack, else queue) -> main loop `urom_msgq_pop` -> `urom_msg_dispatch`.
- **Cross-core events**: own event word 0x510000 (amoor/amoand), LPHY event word 0x548000 + doorbell 0x401a44=1;
  IRQ 29 handler acks 0x401a40.
- **PSM (PHY state machine) block 0x408000**: current state 0x408000[10:7] (also [14:11], [19:15] cached in
  gp-0x3c0..), transition trigger 0x408038 = 1<<bit, error 0x408058 bit2, history 0x408068-0x408074 (printed as
  "PSM_ST_L/H PSM_TR_L/H"), tables loaded into 0x4080a0/0x4080c0/0x4080f0, shared struct ptr 0x408084, TX
  descriptor 0x40807c, RX descriptor 0x408078, sleep/retention ptr 0x408080. State 0xe = exit, 0xc = TX,
  1 = RX listen, 3/8 = idle-ish (inferred from main loop).
- **Main loop**: `urom_main_poll_events` (event 3 -> rx restart, event 0 -> TX request from 0x40807c, state 0xc ->
  `urom_tx_run`), `urom_rx_event_process` (event 1/2: packet detect), message handling, and a low-gain DC re-cal
  path that re-programs 0x401314/0x401334/0x403004/0x403028.
- **RX**: `urom_rx_restart` (big state reset, re-arms AGC, ring, 0x409000 bit0) -> IRQ12 `urom_isr12_rx_detect` ->
  `urom_rx_detect_handler` (lower-ROM 0x522034 sync search over ring desc 0x538568; 0xbad = fail) ->
  `urom_rx_start_demod` (lower-ROM 0x526b84/0x5237b8 CFO etc., HW block at low MMIO 0x3858-0x3ba0) ->
  `urom_rx_wait_samples` (0x409050 sample counter, 0x409064 threshold) -> `urom_rx_demod_block`.
- **TX**: `urom_tx_request` (descriptor at 0x40807c, `urom_tx_config_frame`), `urom_tx_run`, `urom_tx_feed_samples`
  (DMA to 0x40135c, the DFE TX sample input that LPHY also uses for its IQ-cal test tone), `urom_tx_finish`.
- **Host test commands** (msg type 3, cmd id u16 at payload+2): 0x800b, 0x800d (6 x u32 to AON 0x538648..),
  0x800e, 0x800f, 0x8017, 0x8019 (sets mode gp-0x474, 2/3 -> 0x408038=0x20000), 0x811c (MEDIUM_EVAL: stop rx,
  start 30-tick sw timer 3), 0x1002 (return 0x20 B from gp-0x22c), 0x100d, 0x200e (param dump), 0x200f (stats
  reset), 3 and 0x24. These match the test IDs in the memory notes (8108 UPHY has 800B, 811C).

## Function table

Confidence: H = decompiled and role clear; M = role clear from registers/callers but details inferred; L = structural.
"RAM twin" = best signature match in UPHY RAM (score 0..1, from my matcher; >= 0.75 is a strong twin).

| Address | Name | Role | Evidence | Conf | RAM twin |
|---|---|---|---|---|---|
| 0x52cf10 | urom_app_start | crt0: gp, sp, canary, per-hart data init | disasm | H | 0x500000 (0.84) |
| 0x52d018 | urom_app_main_wrapper | init, canary check, main, exit | disasm | H | |
| 0x52d068 | urom_app_main_loop | main loop until PSM state 0xe | decompile | H | 0x5001e8 (low score, structural) |
| 0x52d97a | urom_txa_402070_set_rx_bits | 0x402070[19:18]=0, bit13=1 | disasm | L | 0x503d38 (0.68) |
| 0x52d994 | urom_trx_settle_done | 1000-tick settle timer elapsed | disasm | M | |
| 0x52d9ee | urom_ana405_power_on | sets 0x405020/0x405030 enables | decompile | L | 0x50cb18 (0.85) |
| 0x52da6c | urom_ana405_cfg_405010 | 0x405010/14 fields | disasm | L | |
| 0x52daac | urom_ana405_mode1_seq | 0x405008/10/20/34 sequence | decompile | L | |
| 0x52db34 | urom_ana405_mode2_seq | 0x405020 [15:14], 0x40503c | disasm | L | |
| 0x52db70 | urom_tx_analog_enable | 0x402070/0x405020/0x405038 TX-on sequence | decompile | M | 0x503e50 (0.55) |
| 0x52dc34 | urom_trx_set_mode | analog mode 0/1/2 state machine | decompile | M | 0x503d84 (0.83) |
| 0x52dcd8 | urom_set_lo | synth word 0x40204c, ADPLL 0x402050/58/5c/60 | decompile | H | uphy_set_lo 0x5011ac (0.76) |
| 0x52ddc0 | urom_uart_puts | busy-wait UART at 0x1500 | decompile | H | 0x501290 (0.80) |
| 0x52de3c | urom_puts | puts wrapper | disasm | M | 0x501308 |
| 0x52de64 | urom_app_exit | prints exit code, notifies host, wfi | decompile | H | 0x501328 |
| 0x52de98 | urom_app_init | chip id, hooks, traps, platform init, clock wait | decompile | H | 0x501354 (0.75) |
| 0x52df80 | urom_assert_hart2 | assert mhartid==2 | disasm | M | |
| 0x52dfb0 | urom_mbox_send | mailbox send + doorbell | decompile | H | 0x501484 (0.56) |
| 0x52e06c | urom_set_chip_info | stores 0x2d20 fields | disasm | H | |
| 0x52e07c | urom_get_chip_variant | returns 0x2d20[13:12] | disasm | H | |
| 0x52e090 | urom_mbox_notify_host_ready | send {2} to hart 0 | disasm | M | |
| 0x52e0b0 | urom_mbox_notify_host_exit | send {2, code} to hart 0 | disasm | M | |
| 0x52e0d4 | urom_mbox_buf_ptr | 0x535030[hart] | disasm | H | |
| 0x52e0f8 | urom_timer_irq_disable | clear mie.MTIE | disasm | H | |
| 0x52e118 | urom_psm_goto | PSM transition via table 0x5351b0 | decompile | H | 0x5015dc (0.62) |
| 0x52e2ac | urom_psm_reset | 0x408000 bit0 pulse, cache state | decompile | M | 0x5017a4 (0.84) |
| 0x52e304 | urom_psm_init | load PSM tables, 0x408034/3c/40, ptr 0x408084 | decompile | H | 0x5017fc (0.58) |
| 0x52e874 | urom_psm_trigger | 0x408038=1<<n, check 0x408048 bit2 | decompile | M | |
| 0x52e8ac | urom_psm_print_history | prints 0x408068-74 | decompile | H | 0x501d74 (0.79) |
| 0x52e96c | urom_vprintf | vsnprintf (0x52154e) + puts | decompile | H | 0x501e7c (0.98) |
| 0x52e9cc | urom_assert_fail | "assertion pc:" + crash dump | decompile | H | |
| 0x52ea20 | urom_set_workbuf | stores buf ptr (size >= 1000) | decompile | L | |
| 0x52ea64 | urom_msgq_push | 64-B slot ring push | decompile | H | 0x50282c (0.71) |
| 0x52ead8 | urom_msgq_pop | 64-B slot ring pop | decompile | H | 0x502208 (0.97) |
| 0x52eb80 | urom_wait_irq_flag | wfi until pending-flag bit set | decompile | M | |
| 0x52ebf8 | urom_wait_irq_flag_wide | same, multi-word bitmap | decompile | M | |
| 0x52ec94 | urom_trap_init | mie=0, mtvec=0x538000, intc reset | decompile | H | 0x50235c (0.95) |
| 0x52ecc8 | urom_crash_dump_halt | regs -> AON 0x538084, 0xfee1dead | decompile | H | |
| 0x52edb0 | urom_isr_external | intc claim 0x404604, handler table | decompile | H | 0x50270c (0.82) |
| 0x52ee6c | urom_irq_register | handler table + intc prio/enable | decompile | H | |
| 0x52eec0 | urom_isr_timer | mcause 7 path | decompile | M | |
| 0x52ef2c | urom_mbox_rx_handler | read inbox, ack 0x406000 | decompile | H | |
| 0x52ef84 | urom_isr_software | mcause 3 path | decompile | M | |
| 0x52efec | urom_kick_all_harts | ring all doorbells (on assert) | decompile | M | 0x502670 (0.67) |
| 0x52f048 | urom_trap_unhandled | "Unhandled Trap" print | xref to 0x5354c8 | H | |
| 0x52f084 | urom_trap_dispatch | mcause dispatch | decompile | H | 0x50270c (0.83) |
| 0x52f148 | urom_trace_event | 32-entry trace ring in AON | decompile | H | 0x5027c4 |
| 0x52f1b0 | urom_param_tlv_pack | pack {id,len,data} from param table | decompile | M | 0x50282c (0.69) |
| 0x52f268 | urom_mbox_send_5000_to_mac | type 0x5000 {2} to hart 1 | disasm | M | |
| 0x52f294 | urom_trap_entry | save regs, call dispatch, mret | disasm | H | |
| 0x52f2f0 | urom_psm_load_tbl_4080f0 | 48-word masked load | decompile | M | 0x502ab8 (1.00) |
| 0x52f340 | urom_psm_load_tbl_4080c0 | table load | an. | M | 0x502b08 (1.00) |
| 0x52f364 | urom_psm_load_tbl_4080a0 | table load | an. | M | 0x502b2c (1.00) |
| 0x52f388 | urom_psm_set_event_time | 0x40802c/30 + 0x408034 bit8 | disasm | M | 0x502b50 (1.00) |
| 0x52f3b0 | urom_psm_start_at | 0x408024/28 start time, 0x408004 bit0 | decompile | M | 0x502b78 (0.77) |
| 0x52f414 | urom_init_pkt_cfg_from_52cdb8 | fills gp struct from 0x52cdb8 tables | decompile | L | 0x502bde |
| 0x52f540 | urom_stats_snapshot_rx | DC 0x40136c/70, 0x409020/30/14 -> AON | decompile | M | 0x502d2c (0.66) |
| 0x52f5a4 | urom_read_409108_by_sel | 0x40911c[7:5] selects 0x409108..18 | decompile | M | 0x502d86 |
| 0x52f680 | urom_rf_402078_set_all | all 21 bits of 0x402078 = arg | decompile | M | 0x50c784 (0.82) |
| 0x52f7c0 | urom_ana405_power_on_alt | 0x405020/30 + 0x409434 | an. | L | |
| 0x52f828 | urom_rf_cfg_402030_by_board | 0x402030/34/38 fields, board type 4 vs other | decompile | L | 0x50cab8 (0.83) |
| 0x52f9b0 | urom_agc_load_coef_tables | 5 x 32 B into 0x403040-0x4030c0 | decompile | M | 0x50c9c0 (0.77) |
| 0x52fa30 | urom_agc_load_gain_table | 21 gain idx x 3 words into 0x4030e0 | decompile | M | |
| 0x52fbec | urom_agc_load_step_table | 4 x 21 words into 0x403200-0x403350 | decompile | M | |
| 0x52fc60 | urom_agc_config | full AGC setup 0x403000-0x403034 | decompile | H | uphy_agc_config 0x5031d6 (0.62) |
| 0x52fe98 | urom_agc_fixed_gain | fixed-gain mode setup | decompile | M | uphy_agc_fixed_gain 0x503382 (0.43) |
| 0x52fff4 | urom_agc_clear_or_fixed | clear 0x403004[7:2]/0x403028 or fixed | decompile | L | |
| 0x530068 | urom_chip_variant_is_0 | variant==0 | decompile | H | |
| 0x5300d8 | urom_apply_dccal_codes | DC DAC codes by gain idx | decompile | H | 0x5036bc (0.46); cf. lphy_apply_dccal_codes |
| 0x53018c | urom_dc_servo_cfg | 0x401334 k=2, [10:8]=3 | disasm | M | 0x50c254 (0.95) |
| 0x5301c0 | urom_dc_servo_restart | 0x401334/0x401340/0x401314 sequence | decompile | M | |
| 0x530264 | urom_rx_cfg_409034 | 0x409034 fields, 0x401924 | disasm | L | |
| 0x5302a8 | urom_rx_set_thresholds_409038 | 0x409038[15:0], 0x409204-0x409224 | decompile | L | |
| 0x530330 | urom_rx_measure_409024_set_thresh | 9 x (0x40902c trigger, read 0x409024) | decompile | L | |
| 0x5303b4 | urom_txpwr_compute_scales | per-rate dB -> Q16 scale | decompile | L | |
| 0x53046c | urom_dfe_load_coefs_4094b4 | coef tables -> 0x4094b4, 0x409558 | decompile | M | |
| 0x53052c | urom_rxcap_ring_init | ring 0x5141d0 len 0x708 | decompile | H | |
| 0x5305a4 | urom_isr3_agc_psm28 | IRQ 3: AGC stop, PSM goto 0x28 | decompile | L | |
| 0x5306a0 | urom_agc_select_tables | table set A/B by board type | decompile | H | |
| 0x53070c | urom_lphy_call_sync | send to LPHY, wait 0x300a ack | decompile | H | |
| 0x530768 | urom_fem_init | GPIO 0x1308/0x200c pinmux by board 4/5/6 | decompile | M | uphy_fem_set_state 0x50ce18 (0.53) |
| 0x530850 | urom_fem_set_idle | GPIO set/clear 0x1368/0x136c | decompile | M | 0x50ce18 (0.41) |
| 0x5308c0 | urom_fem_set_state | FEM pins for TX/RX | decompile | M | 0x50ce18 (0.45) |
| 0x530990 | urom_adc_config_codes | 0x402000/0x402004 codes from 0x536e1c | decompile | M | 0x50cf84 (0.69) |
| 0x530abc | urom_adc_clear_codes | zero 0x402000/04 code fields | decompile | M | 0x50d0b0 (1.00) |
| 0x530b2c | urom_hwtimer_irq_register | IRQ 31 handler | disasm | M | 0x5038fc (0.95) |
| 0x530b4c | urom_hwtimer_stop | clear 0x401b00/0x401b40 bit0 | disasm | M | 0x503914 (1.00) |
| 0x530b68 | urom_hwtimer_enable | 0x401b00 bit0 | disasm | M | 0x503930 (1.00) |
| 0x530b88 | urom_hwtimer_arm | compare 0x401b80/84, enable | disasm | M | 0x503950 (0.74) |
| 0x530bb4 | urom_evt_clear_all | 0x510000 = 0 | disasm | H | |
| 0x530bc0 | urom_evt_set | amoor 0x510000 | disasm | H | |
| 0x530bd8 | urom_lphy_evt_set | amoor 0x548000, 0x401a44=1 | disasm | H | |
| 0x530bf4 | urom_evt_read | read 0x510000 | disasm | H | |
| 0x530c00 | urom_evt_test_clear | amoand 0x510000 | disasm | H | |
| 0x530c54 | urom_param_table_dump | msg 0x200e reply via 0x536f0c | decompile | H | 0x5041c4 (0.50) |
| 0x530ca4 | urom_stats_reset | memset AON 0x538378, 0x1b0 | disasm | H | 0x50420c |
| 0x530ccc | urom_enter_sleep | 0x408080=&0x510008, notify MAC, wfi | decompile | M | 0x50422c (0.65) |
| 0x530d7c | urom_fx_db_to_lin_q16 | dB to Q16 linear | decompile | H | 0x50e300 (1.00) |
| 0x530ec0 | urom_swtimer_isr | expire sw timers, pending mask | decompile | M | 0x504280 |
| 0x530f68 | urom_swtimer_start | 5-slot sorted timer list at 0x510030 | decompile | M | 0x504324 (0.59) |
| 0x5310f8 | urom_swtimer_init | register ISR, arm (-1) | disasm | M | |
| 0x531120 | urom_isr29_doorbell_ack | 0x401a40 = 0 | disasm | M | |
| 0x53112c | urom_rx_stop_or_agc_reset | 0x409000 b0 clear + PSM 1, or AGC reset | disasm | L | |
| 0x531264 | urom_frame_end_time | start + duration by rate/bw (consts 0x230/0x168/0xf0) | disasm | L | 0x50e1c0 |
| 0x531308 | urom_psm_408034_bit5 | 0x408034 bit5 = arg | disasm | L | 0x50476c (0.94) |
| 0x531324 | urom_pack_bits7 | 7 bytes -> bitfield | disasm | M | |
| 0x531372 | urom_ana405_set_mode | modes 1/2, caches 0x405008[8:4] | decompile | L | |
| 0x531400 | urom_rx_restart | full RX re-arm | decompile | M | 0x504e44 (0.39) |
| 0x5319e8 | urom_lphy_set_channel | LPHY msg 0x3012 | decompile | M | |
| 0x531a4c | urom_lphy_post_time | time -> 0x548010/14, LPHY event 1 | disasm | M | |
| 0x531a74 | urom_msg_set_channel | msg 0x200c | decompile | H | |
| 0x531adc | urom_msg_5002_channel_change | msg 0x5002, LPHY 0x3012/0x3009 | decompile | M | |
| 0x531bf2 | urom_msg_dispatch | message switch, jump table 0x537264 | decompile | H | 0x50583e (0.65) |
| 0x5321d6 | urom_mbox_rx_dispatch | 0x300a ack vs queue | decompile | H | |
| 0x532218 | urom_isr10_psm_2a | IRQ 10 tail: trace 0x2a, clear 0x408048 b1 | disasm | L | 0x506ec0 (0.57) |
| 0x53224c | urom_platform_init | regs reset, IRQ map, swtimer, PSM init | decompile | H | 0x506144 (0.59) |
| 0x5323d4 | urom_dccal_apply_tables | copy DC tables, AGC config | decompile | M | |
| 0x53245c | urom_dccal_request_lphy | LPHY msg 0x3016 DC cal | decompile | M | |
| 0x532544 | urom_agc_403014_default | 0x403014[7:0]=0xf7 | disasm | L | |
| 0x532560 | urom_phy_init | top PHY init | decompile | H | 0x506678 (0.36) |
| 0x532abe | urom_tx_set_dig_gain_409474 | 0x409474 = Q16 scale | decompile | L | |
| 0x532c24 | urom_tx_dma_chunk | lower-ROM DMA 0x520894 | decompile | L | 0x506f6c (0.64) |
| 0x532d7c | urom_tx_finish | stop TX DMA, FEM idle | decompile | L | 0x506fd8 (0.59) |
| 0x532e7c | urom_tx_feed_samples | TX sample gen + DMA | decompile | M | 0x5070f0 |
| 0x533124 | urom_tx_end_adc_off | rx disable, AGC off, ADC off | decompile | L | |
| 0x533190 | urom_tx_config_frame | TX frame setup (16 args) | callers | L | 0x5075f0 |
| 0x5333cc | urom_agc_gain_locked | read final AGC gain, re-run DC servo | decompile | L | |
| 0x533668 | urom_isr11_dc_servo_cfg | 0x401314[11:2] by variant | decompile | L | |
| 0x53370c | urom_rx_detect_handler | sync search, timing | decompile | M | 0x507dbc (0.44) |
| 0x53393c | urom_isr12_rx_detect | IRQ 12 wrapper | an. | M | |
| 0x533958 | urom_rx_arm_timeout_30ms | +30000 ticks timeout (units assumed us) | disasm | L | |
| 0x5339a4 | urom_psm_event_rx_timestamp | PSM state 1/6 handling, 0x40905c/60 | decompile | L | 0x5084c4 (0.51) |
| 0x533a84 | urom_isr13_psm_timer | IRQ 13: clear 0x408034 b1 | disasm | L | |
| 0x533a9c | urom_rx_disable | 0x40902c, 0x40904c b0, 0x409000 b0, 0x401308 b1 | disasm | M | 0x5085c8 (1.00) |
| 0x533ac4 | urom_adc_power | ADC enables in 0x402000/10/20/28/04 | decompile | M | 0x50c728 (1.00) |
| 0x533b20 | urom_rx_start_demod | demod setup, CFO | decompile | M | 0x5085f0 |
| 0x533e20 | urom_rx_wait_samples | wait on 0x409050 counter | decompile | M | 0x50889c (0.60) |
| 0x533ec4 | urom_rx_demod_block_wrap | wrapper | an. | L | |
| 0x533efc | urom_rx_demod_block | per-block demod, SIG decode, RX desc | decompile (partial) | M | 0x508940 (0.51) |
| 0x534744 | urom_agc_enable | 0x401904 b0, 0x403000 b5/b0 | decompile | M | 0x509280 (0.96) |
| 0x534774 | urom_rx_abort | stop DC servo/AGC/rx, pulse 0x409400 b5 | decompile | M | 0x50935c (0.81) |
| 0x53480c | urom_rx_arm_psm_timers | 0x408034 b1, event time | decompile | L | 0x5093c4 (0.84) |
| 0x534878 | urom_rx_event_process | rx events 1/2 | decompile | M | 0x509424 (0.50) |
| 0x534a18 | urom_tx_request | TX descriptor handling | decompile | M | |
| 0x534c14 | urom_tx_run | TX state 0xc loop | decompile | M | |
| 0x534e8a | urom_main_poll_events | main event poll | decompile | M | |
| 0x534f4c | urom_tlvbuf_init | buf 0x51667c..0x51669c | disasm | L | |
| 0x534f6c | urom_tlvbuf_append | append 0x19 / 0x1225 records | disasm | L | |
| 0x534fd0 | urom_hooks_init | copy 0x5372ec -> 0x51fe80 | decompile | H | |

Count: 144 functions named (plus 2 IRQ entry points 0x533a84 and 0x52f294 split out of merged blocks).
Not named (opaque or tiny): 0x52f650 (variant-dependent call into lower ROM 0x524114/0x524150), 0x530c1c/0x530c24
(info byte ptr gp-0x7e8 -> AON 0x5384d4/d8), 0x530d20 (pure bitfield check), 0x531344, 0x532b60/0x532b8c/0x532bae/
0x532bec/0x532c90/0x532fb0/0x532fd4/0x533084/0x533170, 0x534fc4 and the small leaves at 0x52db5e, 0x52e104.

## Data tables and globals

| Address | Name | Contents / indexed by |
|---|---|---|
| 0x52cca4 | urom_clz_tab | 256-B count-leading-zeros table (lower ROM __udivdi3, RAM 0x50fb62) |
| 0x52cdb8 | urom_pkt_cfg_tables_52cdb8 | two variants of byte/short tables, selected by chip variant (0x52f414, RAM 0x502bde) |
| 0x535004 | (string) | "hart 2 exited with non-zero code 0x%lx" |
| 0x535030 | urom_mbox_buf_table | 4 inbox ptrs indexed by hart |
| 0x535040/54 | (strings) | "phypsm history: ", "PSM_ST_L ... DCF_CF" |
| 0x5350ac/cc/f0 | urom_psm_tbl_4080c0/a0/f0 | PSM microtables loaded at init |
| 0x5351b0 | urom_psm_goto_table | 64 x 12 B {id, expected_state, trigger_bit}, index = PSM transition id |
| 0x5354b0/c8 | (strings) | "assertion pc:", "Unhandled Trap ..." |
| 0x535504/18 | urom_build_date_str / urom_version_str | build id |
| 0x535598 | jump_table_535598 | 5 cases of urom_read_409108_by_sel |
| 0x5355ac | urom_dfe_coef_409558 | 0x60 B coef -> 0x409558 |
| 0x53560c | urom_dfe_coef_4094b4 | 0xa0 B coef -> 0x4094b4 |
| 0x5356bc / 0x53626c | urom_agc_coef_tbl_a / _b | 4 modes x 0xa0 B -> 0x403040-0x4030c0 (5 x 32 B) |
| 0x53593c / 0x5364ec | urom_agc_step_tbl_a / _b | 4 x 21 x 16 B -> 0x403200-0x403350 |
| 0x535e7c / 0x536a2c | urom_agc_gain_tbl_a / _b | 21 gain idx x 0x30 B -> 0x4030e0 (3 words each) |
| 0x536e1c | urom_adc_code_tables | 5 arrays x 0x30 B, index (row*4 + bw)*4, -> 0x402000/0x402004 code fields |
| 0x536f0c | urom_param_table | 107 x {ptr, size<<16 \| id 0x3001..0x306b} into AON 0x538378.. |
| 0x537264 | jump_table_537264 | urom_msg_dispatch host-cmd switch (0x800b + n) |
| 0x5372a0 | (table) | small u32 table for 0x532c90 |
| 0x5372b0 | urom_bw_hz_table | 2e6, -1, 4e6, -1, -1, -1, 8e6 (Hz) used by urom_rx_start_demod |
| 0x5372ec | urom_default_hook_table | 64 fn ptrs -> 0x51fe80 |
| 0x5373f0..fc | (consts) | PSM masks 0xfffff/0x1fffff, AGC alt table ptrs |
| 0x537400 | urom_data_init_image | .data words: ptr 0x548000 (LPHY event word), byte tables |

AON (0x538000+, not in my range; names not applied): 0x538000 trap vector base (mtvec), 0x538084 crash dump,
0x538114 trace ring, 0x5382d4/0x5382e9/0x5382fe/0x538313/0x538328 DC-cal code tables (21 B each, by gain index),
0x538360-0x53836c AGC table ptrs, 0x538378-0x538527 stats/param block, 0x538558-0x538564 channel (freq, bw MHz,
bw idx, primary), 0x538568 I/Q ring descriptor, 0x538594 per-rate TX scale table.

## Registers learned (from ROM app; same code exists in RAM fw per twins)

| Address | Field | Meaning | Evidence |
|---|---|---|---|
| 0x408000 | [10:7] | PSM current state (0xe exit, 0xc TX, 1 RX) | main loop, urom_psm_goto |
| 0x408000 | [14:11], [19:15] | PSM sub-state fields cached with state | same |
| 0x408000 | bit0 | PSM enable/reset | urom_psm_reset |
| 0x408004 | bits 0,4,5,6 | PSM start/mode bits | urom_psm_start_at |
| 0x408024/28 | | PSM start time lo/hi | urom_psm_start_at |
| 0x40802c/30 | | PSM event time lo/hi (0x408034 bit8 arms) | urom_psm_set_event_time |
| 0x408038 | write 1<<n | PSM transition trigger | urom_psm_goto |
| 0x408048/0x408058 | bit2 | PSM error flag | assert paths |
| 0x408068-0x408074 | | PSM history (ST_L, ST_H, TR_L, TR_H) | string "PSM_ST_L.." |
| 0x408078 / 0x40807c / 0x408084 | | RX desc ptr / TX desc ptr / shared struct ptr | rx demod / tx request / psm init |
| 0x408080 | | ptr to retention/TX-timing struct | enter_sleep, tx_run |
| 0x4080a0/c0/f0 | | PSM microtables | psm_init |
| 0x406000 / 0x406004 | bit0 | doorbell (msip) UPHY / LPHY | mbox_send |
| 0x4100 / 0x4104 | bit0 | doorbell host / MAC | mbox_send |
| 0x406040 | | free-running timestamp (trace) | trace_event |
| 0x404604 | | intc claim/complete | isr_external |
| 0x401a40 / 0x401a44 | | doorbell ack (IRQ 29) / LPHY kick | isr29, lphy_evt_set |
| 0x401b00/40/80/84 | | HW timer enable x2, 64-bit compare | hwtimer_* (IRQ 31) |
| 0x40204c | | synth word | set_lo (matches lphy_set_lo) |
| 0x402018/1c/44 | | analog DC DAC codes (per gain index) | apply_dccal_codes |
| 0x402000/0x402004 | code fields [10:3],[18:11],[26:19] / [7:0],[15:8]; enables b16/17/21/22 | ADC loop codes, ADC power | adc_config_codes, adc_power |
| 0x402070/0x402074 | | TX analog chain control (inferred) | tx_analog_enable |
| 0x402078 | bits 0..20 | all set/cleared together (RF block enables, inferred) | rf_402078_set_all |
| 0x403004 | [1:0] AGC mode, [7:2] and [25:20] gain idx (by 0x403000 b5), [13:8] fixed gain | agc_config, gain_locked |
| 0x4030e0.. | | AGC gain table, 21 entries x 3 words | agc_load_gain_table |
| 0x403200-0x40334f | | AGC step table 4 x 21 | agc_load_step_table |
| 0x401904 | bit0 | AGC enable (with 0x403000 b0/b5) | agc_enable |
| 0x401314/0x401334/0x401340 | | DC servo control/restart | dc_servo_* |
| 0x40135c / 0x401324 | | DFE TX sample input (DMA target) | tx_feed_samples |
| 0x409000 | bit0 rx enable, [23:17] mode (0x60000/0xe0000 by variant), b27/b28 | rx_restart, platform_init |
| 0x40904c | bit0, [17:2] | rx enable 2 / RSSI-like value added to estimate | rx_event_process |
| 0x409050 / 0x409064 | | sample counter / wait threshold ([20:12]) | rx_wait_samples |
| 0x40905c/60 | | timestamp hi/lo at detect | psm_event_rx_timestamp |
| 0x409440-0x40944c | | 0x4f04f0, 0xc25c25, 0x101101, 0xfecfec (filter coefs, inferred) | rx_restart |
| 0x409474 | | TX digital gain Q16 | tx_set_dig_gain |
| 0x2d20 | [13:12] variant, [11:8], bit12/13 | chip id fields | app_init, platform_init |
| 0x1500 | | UART TX (bit31 busy) | uart_puts |
| 0x1308 / 0x1368 / 0x136c / 0x200c | | GPIO OE / set / clear / pinmux | fem_* |
| 0x3858-0x3ba0 | | low-MMIO demod/FEC accelerator regs (inferred) | rx_start_demod, tx_feed_samples |

## Open questions

- Whether any boot path ever runs this ROM app on production parts (it would if no RAM image is loaded). The
  reset vector and the handoff live in 0x520000-0x52bfff (other agent).
- Exact meaning of 0x405xxx (ana405_*): looks like a power/bias block with two modes and a 5-bit status at
  0x405008[8:4]; not confirmed.
- Timer tick units for 0x521ad8 (30000 for RX timeout, 1000 for TX settle): assumed microseconds.
- PSM state numbering beyond 1/0xc/0xe; the goto table 0x5351b0 could be decoded fully.
- Host test IDs 0x800b..0x8019 payload semantics (only 0x800d/0x8019/0x811c partly decoded).

## Notes for other regions (do not rename from here)

- Lower ROM (0x520000-0x52bfff) functions called by the ROM app: 0x52004a memcpy, 0x520106 memset, 0x521ad8 64-bit
  time read, 0x52102c / 0x521124 / 0x5210a4 delays, 0x52136c printf, 0x52154e vsnprintf, 0x520ed0/0x520edc
  irq save/restore, 0x5218a8/0x5218c8/0x5218fc/0x521888 intc, 0x52ada0 64-bit divide, 0x5248f4 Q16 multiply,
  0x5227c4 ring init (0x5141d0, 0x708, desc), 0x522710 ring take, 0x522034 RX sync search (returns 0xbad on fail),
  0x5237b8/0x523950 CFO estimate, 0x5239e0 DC/AGC helper, 0x526b84/0x5266dc demod, 0x526d58 TX sample gen,
  0x520894 DMA start, 0x527928/0x5278b8/0x5279b4/0x52799c buffer setup, 0x528xxx/0x529xxx demod helpers.
- UPHY RAM: twins listed above; notably 0x5001e8 main loop, 0x501290 uart puts, 0x501e7c vprintf, 0x502208 msgq pop,
  0x50235c trap init, 0x50270c ext ISR, 0x502ab8/0x502b08/0x502b2c PSM table loaders, 0x5038fc/0x503914/0x503930/
  0x503950 HW timer, 0x50c254 DC servo cfg, 0x50c728 ADC power, 0x50cf84/0x50d0b0 ADC codes, 0x50e300 dB to Q16,
  0x507dbc RX detect, 0x5085c8 rx disable, 0x509280 AGC enable, 0x50935c rx abort, 0x5093c4 rx arm timers.
  RAM 0x502bde uses ROM table 0x52cdb8 directly.
- LPHY: the ROM app sends LPHY messages 0x3009, 0x300c, 0x3010 (board/FEM type query), 0x3012 (set channel),
  0x3016 (DC cal with ring + 5 code tables); LPHY acks with 0x300a. Current RAM fw uses 0x3017 for DC cal.
- AON 0x538000 is the ROM app mtvec, so AON code at 0x538000 is the trap vector table (other agent).
