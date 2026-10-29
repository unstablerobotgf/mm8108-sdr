# UPHY RAM, lower half (0x500000-0x507fff)

Region owner notes for the MM8108 UPHY (hart 2, 192 MHz). 198 Binja functions in range; 166 now carry user names
(4 pre-existing). All names are user symbols in the combined DB `1fd851abcb69f454:ELF` (not saved).

Evidence basis: static decompile/disassembly in Binja plus the session listing `uphy8108.lst`. Nothing in this
report was bench-tested unless it repeats an item from the memory notes. Confidence column: high = register/constant
evidence is direct; medium = role clear from data flow; low = structural guess.

Decompiler gotcha: many globals live in AON (0x538xxx/0x539xxx) or DMEM and are zero in the static image, so
Binja constant-folds branches away (e.g. `uphy_agc_update` 0x5034e8 decompiles to an empty body). Read the
disassembly for anything that tests an AON flag.

## 1. Boot, vectors, main loop

### Vector table (0x500000, vectored mtvec, set by `uphy_trap_init` 0x50235c)
| slot | cause | target |
|---|---|---|
| +0x00 | exceptions | 0x502a60 `uphy_trap_entry` -> `uphy_trap_dispatch`(mcause, mepc) |
| +0x0c | MSIP (3) | 0x502610 `uphy_vec_msip_isr` -> `uphy_mbox_rx_isr` 0x5025b8 |
| +0x1c | MTIP (7) | 0x502558 `uphy_vec_mtip_isr` -> 0x5015d0 (just clears mie.MTIE) |
| +0x2c | MEIP (11) | 0x502468 `uphy_vec_meip_isr`: claim 0x404604, handler table gp-0x278 (0x510590, ids 1..36), complete 0x404604 |
| +0x4c | local irq 19 | 0x50195c `uphy_psm_event_isr` (status 0x408054) |
| +0x54 | local irq 21 | 0x5018c8 `uphy_psm_error_isr` (prints "phypsm history", asserts) |
| others | | 0x502a60 |
| +0x80 | reset | 0x500080 `uphy_reset_entry` |

Every ISR ORs `1 << (mcause & 0xf)` into the pending word at gp-0x470 (0x510398); the MEIP path also sets
bit `id` in the per-IRQ bitmap that follows (0x51039c+). `uphy_irq_wait` 0x5022a8 sleeps on those bits.

### Reset (0x500080)
Checks 0x211c bit0 and mhartid, zeroes all GPRs, gp = 0x510808, writes stack canary 0x66a5eea5 at
0x51ee80.. (4 words), sp = 0x51fe80, zero-fills .bss (0x510398 ..), calls `uphy_early_init` 0x501354,
verifies the canary (mismatch -> `uphy_hart_exit(-0x7f)`), calls `uphy_main_loop` 0x5001e8, then
`uphy_hart_exit` 0x501328 ("hart 2 exited with non-zero code").

### `uphy_early_init` 0x501354
Chip info from 0x2d20 (bits[13:12] variant -> gp-0x43f), 0x2000[22] / 0x401d00[15] toggles, ROM init
0x53809c, `uphy_trap_init`, hartid assert, ROM 0x521150/0x52190c, mstatus.FS, then either
`uphy_boot_init(*0x100000)` 0x506144 (normal) or boot-time store, enables mie MSIE|MEIE, sends hart0 msg 0
(0x501560) or calls 0x50a450 depending on 0x211c bit11, spins until 0x401d00 clock state ready.

### `uphy_boot_init` 0x506144
Stores board-config (BCF) pointer (gp-0x42c), reads BCF key 0x67 bitfields, programs 0x409000/0x40904c/0x401308,
caches chip info (`uphy_set_chip_info`), registers IRQs: 3 -> 0x502f1a, 10 -> nop 0x505780, 11 -> 0x507d14,
12 -> 0x5082ac (rx sync; memory notes "IRQ#12"), 13 -> 0x5084c4, 15 -> 0x507d04, 19 -> 0x508008,
29 -> 0x505f40, 31 -> `uphy_timer_isr` (via `uphy_timer_init`), then `uphy_psm_init` 0x5017fc.

### `uphy_main_loop` 0x5001e8 (139 blocks)
Calls `uphy_phy_init` 0x506678 then `uphy_mbox_send_ready` (hart0 msg 6). Loop condition: PSM state
byte 0x51057c != 0xe. Each pass: two calls into the upper half (0x5096d8 TX scheduler, 0x509424), then sw-timer
events (`uphy_evt_test_clear`, word 0x5103b0):

| event | armed by | action in main loop |
|---|---|---|
| 1 | `uphy_psm_goto` (+60 ms) | PSM timeout: if state in mask 0x1ffc: snapshot 0x408000 to AON, psm_goto(0) clear, `uphy_rx_return_idle`; state 0xc asserts |
| 2 | MEDIUM_EVAL cmd 0x811c (+30 us), TX done | state 1: `uphy_rx_meas_mode(1)`, 0x509db6 (measurement task), 0x509ee4, back to normal; noise stats |
| 3 | AGC (+15 ms) | state 1: TX/beacon prep (0x5027c4 trace 0xa0, 0x50bde2), `uphy_rx_return_idle`; else reschedule |
| 4 | `uphy_agc_check_rssi_overload` | clears overload latch 0x53853d |
| 5 | `uphy_lphy_req_3013` (+10 s) | periodic aux measurement (handled via 0x510000 flags) |
| 6 | AGC settled (+200 us), PSM | state 1/3/0xa: `uphy_agc_update(1,1)`; state 2: return idle |
| 7 | 0x201d stats | +1 ms re-arm, measurement + noise average to gp-0x763 |
| 8 | DC cal / MEVAL | state 1: apply cal results or start DC cal (`uphy_dccal_request_3017`) |
| 9 | periodic table 0x510008 | periodic tasks (10 s, 30 s, 60 s); flagged entries force psm_goto(0x33) + return idle |

Then pending msgs: `uphy_msgq_get` from queue gp-0x688 (0x510180) -> `uphy_msg_dispatch` 0x50583e (asserts on
error). Then with interrupts off, idles (wfi) when state in mask 0x50a and nothing pending, or runs
`uphy_rx_energy_check` 0x505364.

## 2. Message dispatch (`uphy_msg_dispatch` 0x50583e)
Message = {u32 id, u32 len, payload[<=0x38]}.

| id | source (inferred) | action |
|---|---|---|
| 3 | host cmd via MAC | cmd id = u16 at payload+2, see table below. Unknown cmds forwarded to LPHY: `send(3, 3, len, payload)` |
| 4 | response | forwarded to hart1: `send(1, 4, ...)` |
| 0x200c | MAC | `uphy_msg_200c_chan_config`: freq @+4, bw @+8 (1/2/4/8 -> 0..3), flags; then `uphy_rx_return_idle` which reports gain via 0x200d |
| 0x2013 | MAC | store 3 params at 0x53919c/e, 0x5391a0 |
| 0x201d | MAC | reply 0x201e (0x18 B: noise avg, counters) to hart1 |
| 0x2023 | MAC | flag gp-0x3a4=1, return idle if state 1 |
| 0x3015 | LPHY | copy 0x1c B to 0x513670, byte+0x14 -> 0x538ac0 |
| 0x3019 | LPHY | tx-power params -> 0x53907b/c, `uphy_txpwr_recalc` |
| 0x5000 | | assert |
| 0x5002 | MAC | `uphy_msg_5002_set_channel`: stop RX/PSM, store chan params, send LPHY 0x3009, wait up to 999 us for 0x510000 flag 7, ROM 0x5281c8, `uphy_power_sleep(0)` |

Host cmd ids (msg 3), reply built in place (`*s0=2`, len, seq copied from bytes 6..9), reply sent as
`send(1, 4, 0xc, {resp, req})`:

| cmd | action |
|---|---|
| 0x0003 | tx power params (bytes 0xc,0xd) -> `uphy_txpwr_recalc`, reply len 8 |
| 0x1002 | return 0x20 B from gp-0x1c0 (version/caps, inferred) |
| 0x100d | flag 0x538e68 = (arg>0) |
| 0x100e | flag 0x538e10, force re-init, return idle |
| 0x200e | stats TLV dump (`uphy_stats_get_tlv`, 0x8b entries from table 0x5399f4) |
| 0x200f | `uphy_stats_reset` |
| 0x800b | 32-bit param -> 0x538fb4 |
| 0x800e | arg==2 asserts, else forwarded |
| 0x800f | flag 0x53852b |
| 0x8017 | flag gp-0x3b0 |
| 0x8019 | mode -> 0x538e34 (2,3 write 0x408038=0x20000 = PSM trigger 17), return idle |
| 0x811c | MEDIUM_EVAL: flag 0x53894a, `uphy_meval_reset(0x538898)`, 0x509cca, schedule event 2 at +30 us |
| 0x811d | calibration runner: if payload+0x10: 0x408038=0x800000 then `uphy_cal_run_mask`; `uphy_cal_report_fill`; reply len 0x29+2n; return idle |
| 0x8023 | `uphy_cmd_8023_set_param` (u16 @0xc, u8 @0xe, s16 @0x10 optionally converted by 0x50c044) |
| other 0x800c..0x8018 | forwarded to LPHY |

## 3. Mailbox / inter-core messaging (priority 3)
- `uphy_mbox_send(dest, id, len, payload)` 0x501484: waits slot free (0x50a37c, upper half), writes
  {id,len,payload} to slot `*(0x5393d4 + 4*dest)` and rings a doorbell.
  Slots (from AON table 0x5393d4): dest0 0x11fc80, dest1 0x12ff80, dest2 0x51ff80 (own inbox), dest3 0x54ff80
  (LPHY DMEM). Doorbells: dest0 0x4100 b0, dest1 0x4104 b0, dest2 0x406000 b0, dest3 0x406004 b0.
  Dest index == hart id (UPHY prints "hart 2"); LPHY = dest 3. Dest 1 receives replies and 0x200d/0x201e/0x5000,
  so hart1 is the MAC core (inferred); dest0 receives boot status msgs 0, 1, 6 (host/app core, inferred).
- Inbound: LPHY/MAC ring 0x406000 b0 -> MSIP -> `uphy_mbox_rx_isr` clears b0, ROM 0x520c08(2) lock, reads slot 2,
  `uphy_msgq_post` into the 8-entry, 64 B queue at 0x510180 (buf 0x510194), released by 0x520bf4(2).
- Synchronous LPHY requests go through 0x50cdac (upper half): `0x50cdac(id, len, payload, gp-0x3dc, &0x510000)`.
  LPHY signals completion by setting a bit in the shared flag word at 0x510000 (`sub_545a24(&0x510000)` in
  the LPHY loop); UPHY tests with 0x50d14c(&0x510000, n) / clears with 0x50d134.

LPHY messages sent from this region:

| id | sender | payload |
|---|---|---|
| 0x3009 | `uphy_msg_5002_set_channel` 0x505654 | none (async `send(3,0x3009,0,0)`) |
| 0x300c | `uphy_lphy_req_300c` 0x504796 | 0x14 B (synth/PLL request, then reads 0x40205c, waits flag 9) |
| 0x300d | `uphy_lphy_req_300d` 0x506454, `uphy_phy_init` | 0xc B, buffer 0x539170 |
| 0x3010 | `uphy_lphy_send_config_3010` 0x505f4a | 4 B ptr to descriptor 0x538d90 |
| 0x3011 | `uphy_phy_init` | 8 B, RF table 0x538db0 (64 B) |
| 0x3013 | `uphy_lphy_req_3013` 0x505548 | 0xc B {0x538e28, 0x538e24, mode}; re-armed every 10 s (event 5) |
| 0x3014 | `uphy_phy_init` | 4 B ptr 0x538e9c (clock measure result) |
| **0x3017** | **`uphy_dccal_request_3017` 0x5064a8** | 0x34 B: ring 0x5141b0, result tables 0x538ef4.. |
| 0x301b | `uphy_lphy_req_301b_gain` 0x506610 | 0x10 B {0,0,0,0x538ea0} |

Paths that reach 0x3017: main-loop event 8 in state 1 when not yet calibrated (gp-0x35c == 0), and host cmd
0x811d with mask bit 3 via `uphy_cal_run_mask` 0x505784. Results are packed back into the RF gain table by
`uphy_dccal_apply_results` 0x506384 and applied by `uphy_apply_dccal_codes` 0x5036bc (0x402018/1c/24/2c/44),
the same layout as `lphy_apply_dccal_codes`. This is the host-reachable trigger for the DC-offset cal
(static only, not bench-tested).

## 4. PHY power-state machine (PSM, 0x408xxx)
| reg | meaning (evidence) |
|---|---|
| 0x408000 | b0 run/enable (toggled in restart); [5:3] mode (forced to 1 if 0); [10:7] current state (copied to main-loop state 0x51057c); [14:11] previous state; [19:15] extra field |
| 0x408004 | b0 timer A en; b4,b5,b6 config |
| 0x408008 | b0 timer B en; b4 |
| 0x408024/28 | timer A compare lo/hi (`uphy_psm_timer_a_start`) |
| 0x40802c/30 | timer B compare lo/hi (`uphy_psm_timer_b_start`) |
| 0x408034 | control; 0x181 written on RX restart; b4,b5 via 0x50476c; b7/b8 timer A/B arm |
| 0x408038 | write-1 trigger: `1<<n` requests transition n (psm_goto/psm_trigger); also used with 0x20000, 0x800000, 0x80, 2 |
| 0x40803c, 0x408040 | irq masks (0x3fffdf, 0x3fe7f at init) |
| 0x408048 b2, 0x408058 b2 | error/busy -> assert |
| 0x408054 | event status read by `uphy_psm_event_isr` |
| 0x408068..74 | history registers (printed on error) |
| 0x4080a0/c0/f0 | transition tables loaded at init (8, 8, 48 words; 4-bit fields) from AON 0x539404/0x5393e4/0x539428 |
| 0x408080 | written with 0x510008 before sleep (`uphy_power_sleep`) |
| 0x408084 | pointer to a 0x48 B shared state block (cleared at init) |

States seen in main loop: 1 (RX listen), 2, 3, 8 (measure), 0xa, 0xc (illegal timeout), 0xe (exit).
psm_goto ids: 0, 0x25 (rx sync fail), 0x33 (periodic cal).

## 5. AGC / gain / RX
- `uphy_rx_return_idle` 0x504e44: central "back to listen" path: stop PSM timers, FEM to RX
  (`uphy_fem_set_state(gp-0x7b8,1,0)` at 0x50ce18), optional LO re-program (`uphy_set_lo(0,0,1)` and
  `uphy_set_resamp_ratio` when `uphy_lo_needs_reprogram`), pending chan config (0x200c) -> report current
  gain index (0x403004 [7:2] or [25:20] by 0x403000 b5, clamp 21) to MAC via msg 0x200d, arm capture ring
  (`uphy_rxcap_arm(0x5141b0, ...)`), `uphy_agc_update(1,0)`, pulse 0x409000 b0 / 0x409004.
- `uphy_agc_update` 0x5034e8: if AGC block enabled (AON 0x538532): mode 0 -> `uphy_agc_clear_gain_fields`; else
  pick gain index (max of 0x538533 and 0x53856c, or >= 4 when overload latch) -> `uphy_agc_fixed_gain`,
  write 0x409088 [11:0]/[23:12] from ROM cfg table, re-arm event 3 at +15 ms.
- `uphy_rx_agc_settled` 0x50797c (reached only via j stub 0x502eec, caller not found statically): reads AGC
  status 0x403030/0x40301c[9:7], RSSI 0x401984/94/98, gain index, restarts DFE, sets DC servo 0x401334
  (k=2, enable, hold cleared) and 0x401340=1, schedules event 6 +200 us.
- `uphy_rx_sync_handler` 0x507dbc (from IRQ12 0x5082ac): reprograms ring 0x5141b0 len 0x708 (0x50c684), takes the
  I/Q snapshot 0x50a6bc; 0xbad result -> abort (`uphy_psm_goto(0x25)`, overload check). Patch site 0x507e1a
  from the memory notes is inside this function.
- `uphy_irq3_rx_reset_handler` 0x502f1a (IRQ 3): stops AGC, psm_goto(0), re-arms ring, `uphy_agc_fixed_gain(0)`.
- `uphy_rx_energy_check` 0x505364: RSSI 0x401934[10:3] vs threshold (0x3d, or 0x50bfa8 result >= 0x26) in states 1/8.
- `uphy_rssi_to_dbm` 0x507936: ceil(x/8) - 0x56 - offset(bw), floor -107.
- `uphy_agc_check_rssi_overload` 0x50402c: 0x401934[10:0] <= 0x180, halved, > 0x8e<<6 -> latch + event 4 (+15 ms).

## 6. LO / PLL / aux ADC / misc RF
- `uphy_set_lo` 0x5011ac (pre-named). Its second mode (arg3=0) writes a fine offset to 0x402060 =
  ((|off|/1000)<<20)/1000, after 0x402058 b31, 0x402050 b30, 0x40205c 0x7000 -> 0x19000 (10 us steps).
- `uphy_pll_read_402068` 0x5045e2: select 0x402050[29:28]=2, read 0x402068[8:0].
- `uphy_pll_coarse_tune` 0x5048e6: if 0x402068 reading <= 0x95 flags lock problem; signed 7-bit code in
  0x402058[16:10] with sign 0x402064 b4, 0x402058 b23 enable; retries via LPHY 0x300c; fallback toggles 0x402050[16:15].
- Aux ADC at 0x405040..0x40504c / 0x4065xx (start 0x40651c, busy 0x406540, result 0x406524):
  `uphy_auxadc_select_input` 0x500d38 (0x405040[24:9], 0x405044, 0x405048 one-hot channel),
  `uphy_auxadc_config` 0x500dd0, two-point cal `uphy_auxadc_calibrate` 0x500e50 (refs 0x80000/0x100000,
  gain = 0x19e0000/delta, offset 0x1900000), `uphy_auxadc_convert` 0x500ff4, `uphy_auxadc_measure` 0x501098.
  `uphy_temp_sensor_read` 0x50113c: channel (1,8) with 0x405010 b10, T = (raw*0x5555 - 0x717d16)>>16 -> gp-0x768.
  (0x71fd16 is also the LPHY 0x3010 default.)
- `uphy_rf402070_*` 0x503d38..0x503fa4: 3-state FSM (handler table 0x5399e8 in AON) driving 0x402070/0x402074 and
  0x405038 b24 with a 0x8fc (2300 us) settle. Purpose unknown; 0x402070 is also touched by LROM IQ cal.

## 7. TX path (lower priority, named for completeness)
`uphy_tx_setup_frame` 0x5075f0 (from upper-half scheduler 0x5096d8; encoder 0x52715c on state 0x516530, 64 seq bits
to 0x51368c), `uphy_tx_set_digital_gain` 0x506c4a (writes 0x409474, Q16 tables 0x538f88/0x538f90),
`uphy_tx_encode_chunk` 0x5070f0 (ping-pong 0x5136cc/0x513c1c guarded by 0x5eadbeef, DMA target 0x401324 or 0x40135c
by 0x2d20 b12), `uphy_tx_start_playback` 0x506fd8 (FEM state 4 = TX), `uphy_tx_done` 0x507268, `uphy_txpwr_recalc`
0x504cd8 (BCF channel table lookup by freq/100 kHz, `uphy_bcf_find_channel` 0x5029d4).

## 8. Infrastructure
Timers: sw timer list head gp-0x740 (0x5100c8), 16 B slots per event id at 0x5100d0; hw compare 0x401b80/84,
enable 0x401b00 b0, 0x401b40 b0, IRQ 31. Trace ring: `uphy_trace(id,val)` 64 x 12 B at AON 0x538194
{0x406040 timestamp, id, val}, index gp-0x45c. Asserts: `uphy_assert_fail(hash_str, line)` prints
"assertion %s:%d" on UART 0x1500 (bit31 = busy) and halts in `uphy_fatal_halt` 0x502384 (regs dumped to AON
0x5380d4, magic 0xfee1dead, then wfi loop). Doorbells to all cores rung first (`uphy_mbox_ring_all_doorbells`).

## Function table
| addr | name | role | evidence | conf |
|---|---|---|---|---|
| 0x500000 | uphy_vector_table | mtvec vector table | j targets per cause, mtvec write in 0x50235c | high |
| 0x500080 | uphy_reset_entry | reset/crt0 | gp/sp setup, canary, calls main | high |
| 0x5001e8 | uphy_main_loop | event/message loop | layout, msgq, evt bits | high |
| 0x500d38 | uphy_auxadc_select_input | aux ADC input mux | 0x405040/44/48 one-hot | medium |
| 0x500dd0 | uphy_auxadc_config | aux ADC config | 0x405050/0x406500/0x40504c | medium |
| 0x500e50 | uphy_auxadc_calibrate | two-point cal | refs, gain/offset math | medium |
| 0x500fc8 | uphy_auxadc_ensure_cal | cal if missing | checks cal words | medium |
| 0x500ff4 | uphy_auxadc_convert | one conversion + scaling | 0x40651c start, 0x406540 busy | medium |
| 0x501098 | uphy_auxadc_measure | measure channel | wraps select+convert | medium |
| 0x50113c | uphy_temp_sensor_read | temperature | 0x405010 b10, 0x5555 scale | medium |
| 0x501290 | uphy_uart_puts | UART tx | 0x1500 busy bit31 | high |
| 0x501308 | uphy_uart_puts_locked | locked puts | lock/unlock wrappers | high |
| 0x501328 | uphy_hart_exit | exit/park | string "hart 2 exited" | high |
| 0x501354 | uphy_early_init | early init | called from reset | high |
| 0x50145c | uphy_assert_hartid_2 | hartid check | mhartid == 2 | high |
| 0x501484 | uphy_mbox_send | inter-core send | slot table + doorbells | high |
| 0x50153c | uphy_set_chip_info | cache 0x2d20 fields | stores gp-0x438/43c/434 | high |
| 0x50154c | uphy_get_chip_variant | 0x2d20[13:12] getter | gp-0x43c | high |
| 0x501560 | uphy_mbox_send_hart0_msg0 | boot status | send(0,0,4,{2}) | medium |
| 0x50157c | uphy_mbox_send_ready | ready msg 6 | send(0,6,..) at main start | medium |
| 0x5015a0 | uphy_mbox_send_exit | exit msg 1 | from hart_exit | medium |
| 0x5015bc | uphy_mbox_slot | slot ptr | 0x5393d4[dest] | high |
| 0x5015d0 | uphy_mtimer_irq_disable | clear MTIE | csrc mie 0x80 | high |
| 0x5015dc | uphy_psm_goto | PSM transition | 0x408038, history assert | high |
| 0x5017a4 | uphy_psm_restart | toggle 0x408000 b0, snapshot | | medium |
| 0x5017fc | uphy_psm_init | PSM tables, masks, irqs 19/21 | | high |
| 0x5018c8 | uphy_psm_error_isr | PSM error ISR | vector +0x54, history print | high |
| 0x50195c | uphy_psm_event_isr | PSM event ISR | vector +0x4c, 0x408054 | high |
| 0x501ca8 | uphy_psm_trigger | raw trigger + snapshot | 0x408038 | high |
| 0x501d74 | uphy_psm_print_history | decode 0x408068.. | printf | high |
| 0x501e2c | uphy_vprintf | vsnprintf+puts | ROM 0x52154e | high |
| 0x501e7c | uphy_printf | varargs printf | | high |
| 0x501ed8 | uphy_trap_record | "trap %ld:0x%lx" to AON | string | high |
| 0x501f00 | uphy_assert_fail | assert(file hash, line) | "assertion %s:%d" | high |
| 0x501f68 | uphy_assert_fail_pc | assert by pc | string | high |
| 0x501fbc | uphy_bcf_get_tlv | BCF TLV lookup | ROM 0x521b60 on bcf+0x230 | medium |
| 0x502028 | uphy_bcf_get_value | BCF value <= 8 B | | medium |
| 0x5020c8 | uphy_bcf_get_bitfield | masked field | ctz 0x50fb62 | medium |
| 0x502194 | uphy_msgq_put | queue put | 64 B entries | high |
| 0x502208 | uphy_msgq_get | queue get | trace 0x19 | high |
| 0x502294 | uphy_msgq_is_empty | | count==0 | high |
| 0x5022a0 | uphy_msgq_count | | | high |
| 0x5022a8 | uphy_irq_wait | wait irq bit / any (0xff) | wfi loop | high |
| 0x50235c | uphy_trap_init | mtvec, clear MSIP | | high |
| 0x502384 | uphy_fatal_halt | reg dump + park | 0xfee1dead | high |
| 0x502468 | uphy_vec_meip_isr | external irq dispatch | 0x404604 claim | high |
| 0x50251c | uphy_irq_register | set handler, enable | gp-0x278 table | high |
| 0x502558 | uphy_vec_mtip_isr | machine timer | vector | high |
| 0x5025b8 | uphy_mbox_rx_isr | inbound mailbox | 0x406000 b0 clear, msgq post | high |
| 0x502610 | uphy_vec_msip_isr | soft irq vector | | high |
| 0x502670 | uphy_mbox_ring_all_doorbells | wake peers on crash | | high |
| 0x5026c8 | uphy_trap_unhandled | "Unhandled Trap" | string | high |
| 0x50270c | uphy_trap_dispatch | mcause dispatch | | high |
| 0x502a60 | uphy_trap_entry | exception entry | mret | high |
| 0x5027c4 | uphy_trace | trace ring append | 0x538194 | high |
| 0x5027fc | uphy_trace_snapshot_idx | | | medium |
| 0x502814 | uphy_trace_reset | | | medium |
| 0x50282c | uphy_tlv_pack | TLV serializer | | medium |
| 0x5028bc | uphy_rand_range | PRNG range | ROM 0x521a40/88 | medium |
| 0x5028fc | uphy_set_boot_time | | gp-0x428 | low |
| 0x502908 | uphy_get_boot_time | | | low |
| 0x502914 | uphy_mbox_send_sleep_5000 | msg 0x5000 to hart1 | before wfi sleep | medium |
| 0x502968 | uphy_bcf_chan_get_s8_7 | BCF chan byte 7 | | low |
| 0x50297c | uphy_bcf_chan_clamp_power | clamp to entry limit | 0xff = none | low |
| 0x5029ac | uphy_bcf_chan_get_s8_8 | byte 8, 100 = invalid | | low |
| 0x5029d4 | uphy_bcf_find_channel | freq (100 kHz units) + bw lookup | | medium |
| 0x502ab8 | uphy_psm_load_table_4080f0 | | | high |
| 0x502b08 | uphy_psm_load_table_4080c0 | | | high |
| 0x502b2c | uphy_psm_load_table_4080a0 | | | high |
| 0x502b50 | uphy_psm_timer_b_start | 0x40802c/30 | | high |
| 0x502b78 | uphy_psm_timer_a_start | 0x408024/28 | | high |
| 0x502bd4 | uphy_get_rom_cfg | ptr 0x5384ac | | low |
| 0x502bde | uphy_rom_cfg_init | ROM cfg tables by variant | | low |
| 0x502d2c | uphy_rx_stats_snapshot | DC meas 0x40136c/70 etc | | medium |
| 0x502d86 | uphy_rx_read_metric_sel | 0x409108..18 by 0x40911c[7:5] | | medium |
| 0x502dfc | uphy_rxcap_arm | ring 0x5141b0/0x708 via 0x50adec | | medium |
| 0x502ef4 | uphy_agc_check_window | threshold test | | low |
| 0x502f1a | uphy_irq3_rx_reset_handler | IRQ 3 | registration | medium |
| 0x5034e8 | uphy_agc_update | AGC gain re-apply | calls agc_fixed_gain | medium |
| 0x50369c | uphy_lo_needs_reprogram | | gp-0x420 | medium |
| 0x5036bc | uphy_apply_dccal_codes | DC DAC codes | same regs as LPHY | high |
| 0x50377c | uphy_dfe_write_401384 | 4 bytes | | low |
| 0x5038b8 | uphy_cal_table_ptrs_init | AON table ptrs | | medium |
| 0x5038fc | uphy_hwtimer_irq_register | IRQ 31 | | high |
| 0x503914 | uphy_hwtimer_stop | | | high |
| 0x503930 | uphy_hwtimer_enable | | | high |
| 0x503950 | uphy_hwtimer_arm | compare 0x401b80/84 | | high |
| 0x50397a | uphy_meval_reset | MEDIUM_EVAL stats reset | called by 0x811c | medium |
| 0x503a3c | uphy_meval_measure_noise | noise est | | low |
| 0x503b02 | uphy_meval_update | busy time / per-BSS RSSI (17) | 1 s window | medium |
| 0x503d38/84, 0x503ed8 | uphy_rf402070_fsm_st2/st1/st0 | FSM states | table 0x5399e8 | low |
| 0x503e50 | uphy_rf402070_enable | | | low |
| 0x503f38 | uphy_rf402070_settled | 2300 us check | | low |
| 0x503fa4 | uphy_rf402070_fsm_step | | | low |
| 0x504004 | uphy_agc_eval_level_0x37 | | | low |
| 0x50402c | uphy_agc_check_rssi_overload | | 0x401934 | medium |
| 0x5040c8 | uphy_periodic_next_due | | table 0x510008 | high |
| 0x5040f8 | uphy_periodic_init | | | high |
| 0x504144 | uphy_periodic_pick_due | | | high |
| 0x5041c4 | uphy_stats_get_tlv | cmd 0x200e | | medium |
| 0x50420c | uphy_stats_reset | cmd 0x200f | | medium |
| 0x50422c | uphy_power_sleep | notify + wfi | | medium |
| 0x504280 | uphy_timer_isr | sw timer expiry | | high |
| 0x504304 | uphy_evt_get_pending | | | high |
| 0x50430c | uphy_evt_test_clear | | | high |
| 0x504324 | uphy_timer_schedule | (t64, event) | | high |
| 0x504498 | uphy_timer_cancel | | | high |
| 0x504518 | uphy_timer_init | | | high |
| 0x504538 | uphy_cmd_8023_set_param | | | low |
| 0x504580 | uphy_rx_enable | 0x409000 b0 | | high |
| 0x50458e, 0x505f12 | uphy_dfe_set_401924_bw(_b) | 0x401924 b25/b24 | | low |
| 0x5045bc/d0 | uphy_set/clr_401d08_bit4 | | | high |
| 0x5045e2 | uphy_pll_read_402068 | | | medium |
| 0x504608 | uphy_cal_report_fill | 0x811d reply | | medium |
| 0x504738 | uphy_rx_pipeline_reset | 0x409000/04 pulses | | medium |
| 0x50476c | uphy_psm_set_408034_bits | | | high |
| 0x504796 | uphy_lphy_req_300c | | | high |
| 0x5048e6 | uphy_pll_coarse_tune | | | low |
| 0x504a20 | uphy_rx_restore_dfe | 0x409400, 0x409204..24 | | medium |
| 0x504b26 | uphy_rx_meas_mode | enter/leave measurement | | low |
| 0x504c46 | uphy_set_power_mode | | | low |
| 0x504c9a | uphy_meval_time_accum | | | low |
| 0x504cd8 | uphy_txpwr_recalc | | | medium |
| 0x504e44 | uphy_rx_return_idle | | | medium |
| 0x505364 | uphy_rx_energy_check | | | medium |
| 0x505548 | uphy_lphy_req_3013 | | | high |
| 0x5055cc | uphy_msg_200c_chan_config | | | medium |
| 0x505654 | uphy_msg_5002_set_channel | | | medium |
| 0x505752 | uphy_msgq_post | | | high |
| 0x505780 | uphy_irq10_nop | IRQ 10 | | high |
| 0x505784 | uphy_cal_run_mask | 0x811d | | high |
| 0x50583e | uphy_msg_dispatch | | | high |
| 0x505f40 | uphy_irq29_ack_401a40 | IRQ 29 | | medium |
| 0x505f4a | uphy_lphy_send_config_3010 | | | high |
| 0x506144 | uphy_boot_init | | | high |
| 0x506384 | uphy_dccal_apply_results | | | medium |
| 0x506454 | uphy_lphy_req_300d | | | high |
| 0x5064a8 | uphy_dccal_request_3017 | | | high |
| 0x506610 | uphy_lphy_req_301b_gain | | | high |
| 0x506678 | uphy_phy_init | | | high |
| 0x506c4a | uphy_tx_set_digital_gain | 0x409474 | | medium |
| 0x506e68 | uphy_chan_idx_fold | | | low |
| 0x506e94 | uphy_tx_state_defaults | | | low |
| 0x506ec0 | uphy_psm_timer_a_stop | | | low |
| 0x506f00..0x507268 | uphy_tx_buf_init, uphy_tx_ctx_init, uphy_tx_dma_push, uphy_tx_start_playback, uphy_tx_encode_chunk, uphy_tx_encode_all, uphy_tx_done | TX | | medium/low |
| 0x507398 | uphy_tx_cleanup | | | low |
| 0x507464 | uphy_tx_prepare_rate | | | low |
| 0x5074d6 | uphy_tx_gain_is_neg | | | low |
| 0x507548 | uphy_tx_prepare | | | low |
| 0x5075d8 | uphy_tx_prepare_traced | | | low |
| 0x5075f0 | uphy_tx_setup_frame | | | medium |
| 0x507936 | uphy_rssi_to_dbm | | | medium |
| 0x50797c | uphy_rx_agc_settled | | | medium |
| 0x507d04 | uphy_irq15_ack_409064 | IRQ 15 | | medium |
| 0x507d14 | uphy_irq11_handler | IRQ 11 | | low |
| 0x507dbc | uphy_rx_sync_handler | IRQ12 body | | medium |

Left unnamed (trivial or opaque): 0x501324/0x501554/0x50155c/0x502188/0x50218c/0x502190/0x5027f8/0x504142 (ret
stubs), 0x502954 (reads byte 0x1d), 0x502930, 0x502bcc, 0x503fcc, 0x503fd4, 0x503ff8, 0x50318e, 0x506c3c/44,
0x5037a4..0x503858 (thin wrappers into 0x50c4xx-0x50c5xx / 0x508dxx), j_ stubs, and 0x5024e0/0x502518
(junk splits inside the MEIP handler).

## Data / globals (DMEM, not symbolized since outside the assigned code range)
| addr | meaning |
|---|---|
| 0x510000 | LPHY completion flag word (bits 0..9) shared with LPHY |
| 0x510008 | periodic task table, 3 x 24 B, periods 10 s / 30 s / 60 s |
| 0x510050 | FEM config (from memory notes) |
| 0x5100c8 (gp-0x740) | sw timer list head; 0x5100d0 slots, 16 B per event |
| 0x510180 (gp-0x688) | msg queue {rd, wr, count, cap=8, buf=0x510194} |
| 0x510398 (gp-0x470) | irq pending word + per-IRQ bitmap |
| 0x5103b0 (gp-0x458) | sw event pending bits |
| 0x5103ac (gp-0x45c) | trace index |
| 0x5103cc (gp-0x43c) | chip variant (0x2d20[13:12]) |
| 0x5103dc (gp-0x42c) | BCF pointer |
| 0x510578 (gp-0x290) | PSM snapshot {prev, cur(0x51057c), extra} |
| 0x510590 (gp-0x278) | MEIP handler table, 36 entries |
| 0x5141b0 | capture ring, 0x708 samples |
| 0x513270 | 0x400 B TX/RX scratch ring |

## Registers learned
| reg | field | meaning | evidence |
|---|---|---|---|
| 0x406000 / 0x406004 | b0 | UPHY / LPHY doorbell (MSIP) | send + rx isr clear |
| 0x4100 / 0x4104 | b0 | hart0 / hart1 doorbell | send |
| 0x404604 | | PLIC-like claim/complete | MEIP handler |
| 0x401b00/40/80/84 | | UPHY sw timer compare, IRQ 31 | hwtimer funcs |
| 0x406040 | | free-running timestamp | trace ring |
| 0x1500 | b31 busy | UART tx | puts |
| 0x408000..0x4080f0 | | PSM (see section 4) | |
| 0x401334 | b0 en, [4:1] k, b5 hold | DC servo | AGC settled writes k=2 |
| 0x401934 | [10:0] | RSSI/power level | energy check, overload |
| 0x403004 | [7:2], [25:20] | active gain index (by 0x403000 b5) | 0x200d report |
| 0x40301c | [9:7] | AGC status | AGC settled |
| 0x402060 | | LO fine offset word (Q20 MHz-ish) | uphy_set_lo mode 0 |
| 0x402068 | [8:0] | PLL readback (0x402050[29:28]=2 select) | |
| 0x402058 | [16:10] + 0x402064 b4 sign, b23 en | PLL coarse code | |
| 0x405040..4c, 0x4065xx | | aux ADC | |
| 0x409474 | | TX digital gain (Q16, <= 0xffff) | |

## Open questions
- Exact role of hart0 vs hart1 (dest 0/1). Replies to host cmds go to dest 1, so the host path is via the MAC core.
- Who installs the j stub 0x502eec (-> `uphy_rx_agc_settled`); no static xref.
- Purpose of the 0x402070 3-state FSM and of LPHY msgs 0x300c/0x300d/0x3013 payloads.
- Main-loop state numbering vs PSM hardware states beyond 1/8/0xe.

## Notes for other regions
- Upper half: 0x50cdac = synchronous LPHY request (send + register completion on 0x510000); 0x50d14c/0x50d134/
  0x50d144 test/clear/read that flag word; 0x50a37c = mailbox slot wait; 0x50c684 ring setter, 0x50adec ring
  config, 0x50a6bc I/Q snapshot; 0x5096d8 TX scheduler (calls `uphy_tx_setup_frame`); 0x509db6/0x509ee4
  measurement task; 0x5082ac IRQ12; 0x5084c4 IRQ13; 0x508008 IRQ19; 0x50ce18 FEM state (already named
  `uphy_fem_set_state`); 0x50c620 named `uphy_set_resamp_ratio`.
- AON: 0x5393d4 mailbox slot pointer table (4 words); 0x5393e4/0x539404/0x539428 PSM transition tables;
  0x5394ec psm_goto table (12 B entries); 0x5399e8 rf402070 FSM handler table (3 ptrs); 0x5399f4 stats TLV table
  (0x8b entries); 0x538194 trace ring; 0x5380d4 crash register dump (magic 0xfee1dead); 0x538518 AGC state
  struct; 0x538898 MEDIUM_EVAL stats; 0x538ef4.. DC-cal result tables; 0x538d90 LPHY 0x3010 descriptor.
- LPHY loop (0x5401e8) case list: 0x3009 chan prep, 0x300c, 0x300d, 0x3010 (temp default 0x71fd16), 0x3011 (RF
  table ptrs), 0x3013 (aux meas, reply in payload), 0x3014 (freq-counter clock measure), 0x3017 DC cal, 0x301b
  (gain index + bw), host cmd 3 for 0x0019, 0x0024, 0x800e, 0x8119. Completion via `sub_545a24(&0x510000)`.
- UROM: 0x521ad8 = 64-bit time (us); 0x520ed0/0x520edc irq save/restore; 0x52004a memcpy; 0x520106 memset;
  0x52102c delay_us; 0x521124 delay; 0x52ada0 64-bit div; 0x5248f4 Q16 mul; 0x524c48 Q16 div; 0x52136c printf;
  0x521398 snprintf; 0x52154e vsnprintf; 0x5218a8/0x5218c8/0x5218fc irq enable/disable/priority;
  0x520c08/0x520bf4 mailbox lock/unlock (arg = hart); 0x520894 DMA copy.
