# UPHY ROM, lower part (0x520000-0x52bfff), prefix `urom_`

Method: capstone sweep of the combined ELF (`rom/mm8108_phy.elf`) with a resync-on-invalid-opcode
disassembler, normalized-instruction diffing against every LPHY ROM function (exact or fuzzy
SequenceMatcher ratio), RAM->ROM call census (jal and auipc+jalr in UPHY RAM 0x500000-0x50fb9e),
then Binja decompile/xrefs for the non-trivial ones. Scripts are in the session scratchpad
(`scratchpad/urlo/`: mem.py, match.py, calls.py, summ.py, peek.py, addrref.py).

206 functions and 16 data symbols named, 15 comments. Nothing saved.

## Subsystem overview

| Range | Contents |
|---|---|
| 0x520000-0x524f80 | Shared "common" ROM library. Byte-for-byte or near twin of LPHY ROM 0x550000-0x552c00 (same order). libc (memcpy/memset/memmove/strncmp/snprintf/vsnprintf), DMA engines, shared spinlock block at 0x1d00, clocks, timers, mcycle delays, intc 0x404000, xoroshiro64** PRNG, object table, perf counters, hook trampolines, DFE/ADC/capture-ring register helpers, Q16 fixed-point math. |
| 0x521e04-0x523a8c | UPHY-only PHY code: RX sync / packet-detect configuration (0x409000-0x4090bc block), bandwidth detect + CFO estimate, ADC loop-code selection by band. |
| 0x524864-0x52aa00 | UPHY-only "coprocessor" (cop) driver. Uses RISC-V custom-1 opcode 0x2b (funct3=0, ~50 funct7 values) heavily from 0x526200 on. Struct fields (FFT size 0x20/0x40/0x80/0x100 at +0x24/+0x68, +0x6b0, +0x71c) say this is the OFDM baseband (FFT/equalizer/demod) engine. Mostly not named (see open questions). |
| 0x52aa02-0x52b0fc | libgcc `__divdi3` and `__udivdi3` (uses `__clz_tab` at 0x52cca4). |
| 0x52b100-0x52bfff | rodata (strings, jump tables, ADC code tables, unreferenced default tables). |

**Reset/trap/IRQ entry: not in this ROM (verified).** 0x520000 is libc `abort()` (calls hook05 fatal with
`common/libc/morse_libc_rom.c`, line 83, "abort"). No mtvec/mret/mcause anywhere in 0x520000-0x537fff.
The UPHY vector table is RAM 0x500000 (`j 0x502a60` reset, then a vectored table at 0x500040).

Every non-leaf ROM function starts with a stack probe `lui a5,0xfffff; add a5,sp; sw zero,-0x128(a5)`
(touches sp-4392). Useful signature for finding function starts; also why linear sweep in this ROM is clean.

## Hook table (RAM function pointers the ROM calls)

ROM stubs at 0x521c2c + 10*j do `auipc t1; lw t1, X(t1); jr t1`. Stub j maps to table index j for j<15 and
j+9 for j>=15. Table base **0x51fe80** (DMEM, 64 words). Installed by AON 0x53809c (copies 0x100 bytes from
0x539ea8); ROM default copy at 0x5372ec installed by 0x534fd0 (upper ROM). Roles below come from the RAM
targets in 0x539ea8 (verified by reading those functions):

| idx | slot | stub | RAM target | role |
|---|---|---|---|---|
| 0 | 0x51fe80 | 0x521c2c | 0x501e2c | vprintf |
| 1,2,4 | | | 0x501fa0/fb0/fa8 | `return 0` |
| 3 | | | 0x501fb8 | empty |
| 5 | 0x51fe94 | 0x521c5e | 0x501f68 | fatal(file_or_0, line_or_pc, ...) (24 ROM callers) |
| 6 | 0x51fe98 | 0x521c68 | 0x501e74 | trace |
| 7, 13 | | | 0 | unset in RAM table |
| 8 | 0x51fea0 | 0x521c7c | 0x5028bc | random in [a0,a1] via prng_next_bounded (state 0x538498) |
| 9 | 0x51fea4 | 0x521c86 | 0x5027c4 | event log ring (64 x 12 B at 0x538194, hwtimer stamped) |
| 10 | 0x51fea8 | 0x521c90 | 0x502908 | return 64-bit value at gp-0x428 (time) |
| 11 | 0x51feac | 0x521c9a | 0x501484 | message send (id, cmd, len<=0x38, buf) (inferred) |
| 12 | 0x51feb0 | 0x521ca4 | 0x50154c | return word gp-0x43c (state) |
| 14 | 0x51feb8 | 0x521cb8 | 0x5027bc | return perf ctx gp-0x760 |
| 24 | 0x51fee0 | 0x521cc2 | 0x50a0e4 | return 0x51667c (cop ctx, used by cop driver) |
| 25 | 0x51fee4 | 0x521ccc | 0x503ff8 | return 0x510668 |
| 26 | 0x51fee8 | 0x521cd6 | 0x502bcc | return gp-0x1e8 (rx ctx: CFO / per-bw words) |
| 27 | 0x51feec | 0x521ce0 | 0x502bd4 | return 0x5384ac (rx cfg; RAM stores 0x52b1d0 ptr into it) |
| 28 | 0x51fef0 | 0x521cea | 0x502d2c | rx stats snapshot (reads 0x40136c/0x401370 DC, 0x409020/30) |
| 29 | 0x51fef4 | 0x521cf4 | 0x502d86 | rx-sync event handler, switch on 0x40911c[7:5] |
| 30 | 0x51fef8 | 0x521cfe | 0x50369c | bool: apply CFO by retuning LO |
| 31 | 0x51fefc | 0x521d08 | 0x5011ac | **uphy_set_lo** |
| 32 | 0x51ff00 | 0x521d12 | 0x503fcc | return gp-0x748 |
| 33 | 0x51ff04 | 0x521d1c | 0x509f68 | list/queue init (0x51665c/0x51667c) |
| 34 | 0x51ff08 | 0x521d26 | 0x50a0c0 | append cmd word (0x19 or 0x1225,0) to buffer |
| 35 | 0x51ff0c | none | 0x50113c | no lower-ROM stub |

## Named functions

Confidence: high = exact twin of a named LPHY function or unambiguous code; medium = role clear from regs/callers;
low = structural name. "RAM n" = number of UPHY RAM call sites (my sweep).

### Common library (LPHY twins)

| addr | name | role / evidence | conf |
|---|---|---|---|
| 0x520000 | urom_abort | hook05(file, 83, "abort") | high |
| 0x520028 | urom_hwcmd_issue_1a05 | exact twin lrom 0x550020; base[1..3]=args, base[0]=0x1a05 | high |
| 0x520040 | urom_hwcmd_wait_idle | exact twin 0x550038 | high |
| 0x52004a | urom_memcpy | exact twin, RAM 34 | high |
| 0x520106 | urom_memset | exact twin, RAM 9 | high |
| 0x5201bc | urom_wait_2004_bit1_cycle | waits 0x2004 bit1 low then high, only if 0x2d20[13:12] in {0,2} | medium |
| 0x5201ec | urom_pulse_405020_bit0_synced | 0x405020 bit0 toggle around 0x2004 sync | medium |
| 0x520220 / 0x5203c4 | urom_panic_dma_bad_engine(2) | hook05(0, pc) on bad DMA engine id | high |
| 0x520a08 | urom_panic_halt | twin lrom_panic_halt | high |
| 0x520f40 | urom_panic_bad_clock | twin lrom_panic_bad_clock | high |
| 0x52023c | urom_dma_fifo_push3 | push 3 words to engine +0x20 FIFO after waiting +0x30 | low |
| 0x5202f0 | urom_dma_engine_config | engine +4/+8/+0xc, +0 = flags 0x1a01, +0x38=-1; engines 0xd000/0xd100/0x408400/0x408500 | medium |
| 0x5203e0 / 0x52040c | urom_dma_ch_(un)lock_checked | ch<4 else panic | high |
| 0x520438 | urom_dma_wait_idle | polls 0x40170c/0x40180c/0x1a0c/0x1b0c busy | medium |
| 0x5204f8 | urom_dma_ack_status | +0x38=1, +0x40=0 on engine 1 or 3; RAM 2 | medium |
| 0x520558 | urom_dma_ch_start | src/dst/len to ch regs, go bit24 | medium |
| 0x520630 | urom_dma_xfer2_wait | variant with bit25 mode, args swapped, waits busy | low |
| 0x520894 | urom_dma_ch_queue_desc | 3-word desc to 0x1b30/34 or 0x401830/34; RAM 1 | medium |
| 0x520940 | urom_dma_ch_start_wrap | wraps ch_start | medium |
| 0x520988 | urom_dma_xfer_wait | dispatch by engine id (0-3 or MMIO base) then wait | medium |
| 0x520a24 | urom_clk_root_sel_lookup | 0x2000[6:4] -> byte table 0x52cda4 | low |
| 0x520a64 | urom_clk_measure_with_freq_counter | 0.96 twin | high |
| 0x520b94 | urom_hwsem_init | memset(0x1d00,0,0x40) | high |
| 0x520bb8..0x520ec4 | urom_hwsem_<addr>_{lock,unlock,is_locked,trylock,inc,dec,read,set} | amoswap/amoadd on 0x1d00-0x1d30 | high |
| 0x520c1c / 0x520c34 | urom_dma_ch_lock / unlock | amoswap 0x1d1c[ch]; used by every DMA op | high |
| 0x520c48 | urom_hwsem_1d18_barrier | amoadd then spin | medium |
| 0x520c6c / 0x520d58 | urom_core_sync_handshake / wait | flags 0x401a40/44 and 0x1d40/44, counter 0x1d18 | low |
| 0x520ed0 / 0x520edc | urom_irq_save / restore | csrrci/csrrsi mstatus.MIE; RAM 18/21 | high |
| 0x520ee8 | urom_sysctl_cfg_20f8 | 0x1308/0x1304 masks, 0x20f8 fields, 0x405030 bit18 | low |
| 0x520f5c | urom_clk_pll_khz | 0x2108[5:1] * 8000 kHz | medium |
| 0x520f8c | urom_clk_source_khz | twin; src2 = 32000 kHz, src0 from 0x2098[7:6] | high |
| 0x52100c | urom_mcycle_read64 | | high |
| 0x521020 | urom_hwtimer_read64 | 0x406040/44 | high |
| 0x52102c | urom_delay_us64 | mcycle based; RAM 19 | high |
| 0x5210a4 | urom_delay_ns64 | as above with /1000 via udivdi3 | high |
| 0x521124 | urom_delay_hwtimer_ticks | twin; RAM 8 | high |
| 0x521150 | urom_hwtimer_disarm_compare | 0x406020/24 = -1 | high |
| 0x521160 | urom_sysctl_set_4108_by_chip | 0x4108[15:0] = 0x54 if 0x2d20[13:12]==1 else 0x20 | medium |
| 0x52118c | urom_clk_cpu_khz | 0x401d00 bit2 src, [11:9] div via jump table 0x52b100 | high |
| 0x521240 | urom_clk_cpu_hz | khz*1000 (LPHY twin divides, so not cycles_per_us) | high |
| 0x52125c / 0x521290 | urom_sysctl_ind_write_2124 / ind_cmd_2124 | command word to 0x2124, wait bit13 | low |
| 0x5212b0 | urom_sysctl_set_2134 | 0x2134[5:1] | low |
| 0x5212d8 | urom_popcount32 | | high |
| 0x5212f0 | urom_strncmp | | high |
| 0x521318 | urom_strnlen | | medium |
| 0x521342 | urom_memcmp | byte compare, odd return convention | low |
| 0x52136c | urom_log_printf | varargs -> hook00; RAM 8 | high |
| 0x521398 | urom_log_trace | varargs -> hook06 | high |
| 0x5213c0 | urom_fmt_parse_uint | decimal width parse | high |
| 0x52140c | urom_fmt_number | uses digit tables 0x52b144/0x52b16c | high |
| 0x52154e | urom_vsnprintf_core | 0.96 twin; "<NULL>" | high |
| 0x521768 | urom_snprintf | | high |
| 0x52178c | urom_memmove | | high |
| 0x521800 | urom_memcpy_backward | | medium |
| 0x521838 / 0x521868 | urom_load_be48 / load_be32_off2 | 6-byte big-endian (MAC address) loaders | medium |
| 0x521888 | urom_intc_reset_all | 0x404400/404/600 = 0 | high |
| 0x5218a8 / 0x5218c8 | urom_intc_enable_irq / disable_irq | bitmaps at 0x404400 | high |
| 0x5218ec | urom_intc_disable_all | | high |
| 0x5218fc | urom_intc_set_priority | 0x404000[irq] | high |
| 0x52190c | urom_perf_snapshot | hwtimer, mcycle, minstret into hook14 ctx | high |
| 0x52194c | urom_perf_cpu_busy_permille | mcycle delta vs timer delta | medium |
| 0x5219d4 | urom_perf_ipc_permille | minstret*1000/mcycle | high |
| 0x521a40 / 0x521a88 | urom_prng_xoroshiro64ss_next / next_bounded | exact twins | high |
| 0x521ad8 | urom_time_now64 | probe + tail to hwtimer_read64; RAM 56 (most called ROM fn) | high |
| 0x521ae8 | urom_hwtimer_busy_wait | spin until now-start >= a0 | high |
| 0x521b2c..0x521bd8 | urom_objtab_{is_set,init,lookup,register,replace} | id-indexed pointer table, -12/-17/0xffff8002 errors | high |
| 0x521c2c..0x521d26 | urom_hookNN_* | see hook table | high |
| 0x521d30 | urom_bits_append_parity4 | 4 parity bits over byte array appended at end | low |
| 0x52aa02 | urom___divdi3 | signed 64-bit div; 0.99 twin of lrom 0x552be4; RAM 3 | high |
| 0x52ada0 | urom___udivdi3 | RAM 9 | high |

### Fixed point (Q16)

| addr | name | evidence | conf |
|---|---|---|---|
| 0x5248f4 | urom_fx_mul | exact twin; byte-wise signed multiply (real code ends 0x524c46); RAM 23 | high |
| 0x524c48 | urom_fx_div_q16 | exact twin | high |
| 0x524cbc | urom_fx_sqrt | exact twin | high |
| 0x524d38 / 0x524ddc / 0x524df0 | urom_fx_sin/cos/tan_q16 | twins | high |
| 0x524e20 | urom_fx_atan2_q16 | constants pi/4 (0xc90f), 0.1963, 0.9817 = standard atan approx | medium |
| 0x524ed4 / 0x524f30 | urom_fx_asin_q16 / acos_q16 | x/sqrt(1-x^2) then atan2 | medium |
| 0x52201c | urom_fx_phase_to_hz | x*159154 (1e6/2pi) >> (16-k) | medium |

### PHY: RX sync, capture ring, resampler, ADC, DFE

| addr | name | evidence | conf |
|---|---|---|---|
| 0x521dd8 | urom_rx_pulse_409400_bit1 | n pulses of 0x409400 bit1 | high (structural) |
| 0x521e04 | urom_rx_chain_shutdown | clears 0x409000 b0, 0x405008, 0x409468 b5, 0x402050, 0x402004, 0x402000 b1/b2, 0x402048 b0, 0x401d04 b0 | medium |
| 0x521e9c | urom_rxsync_write_409084_nibbles | 8 nibbles -> 0x409084, 0x409080=0x1ff | high (structural) |
| 0x521f0c | urom_rx_phase_offsets_init | fills 9 words with k*pi*2^15 phase constants per bw | low |
| 0x522004 | urom_tbl_52b1c0_lookup_s8 | s8 table [a1*4+a0]; also read by RAM 0x50a65c | high (structural) |
| 0x522034 | urom_rxsync_detect_bw_cfo | see below; called from upper ROM 0x533760 | medium |
| 0x5226e4 | urom_rxsync_stop_notify | clears 0x409000 low bits + b0, tail-calls hook29; RAM 1 | medium |
| 0x522710 | urom_rxsync_arm | 0x409000 config from u16 table 0x52cdb0[bw], 0x409028=0x2492492, 0x40904c, 0x401320=0, 0x401308 b2, 0x409000\|=1, 0x409044=a0; RAM 1 | medium |
| 0x5227a0 | urom_rxsync_fill_40900c | n writes of (a0<<6 \| i<<10 \| a2<<17) to 0x40900c | high (structural) |
| 0x5227c4 | urom_rxsync_config | full 0x409000/04/10/18/1c/28/4c/84/400 setup per bw and primary-channel index; called from upper ROM 0x53056e | medium |
| 0x522fa0 | urom_rxsync_config_alt | same callees as above, no callers found | low |
| 0x523950 | urom_rxsync_blend_409010 | mix by 0x409010[11:4] via fx_mul; RAM 1 | low |
| 0x5239e0 | urom_rxsync_set_thresh_by_level | thresholds 0x3c/0x50/0x5a -> 0x409014[3:0], 0x409088 fields | low |
| 0x523a8c | urom_adc_select_code_rows_by_freq | 13 row pointers by f_LO (818 MHz, 890 MHz splits) | high |
| 0x523c28 | urom_adc_config_bw | 0x402004[20:18] div per bw (f>1e9 shifts it), 0x402000/04/08 code fields; matches LPHY 0x544380 notes | high |
| 0x523e40 / 0x523e54 | urom_adc_disable / adc_enable | 0x402000 b0-2, 0x402010 b28, 0x402004 b17 (clock enable) | medium |
| 0x523e88 | urom_adc_power_seq | 0x402004 b21/22/17, 0x402000 b1 | low |
| 0x523ec8 / 0x523f38 / 0x524100 | urom_resamp_reset_409400 / enable_409400 / set_409400_bit4 | 0x409400 control bits with 1-2 us delays | medium |
| 0x523f58 | urom_resamp_set_ratio | writes **0x409404** from (f_LO, bw) via udivdi3 | high |
| 0x52402c | urom_rxcap_ring_start | **0x409044 base, 0x409048 len, 0x40904c\|=1**, 0x409000&=~1, 0x401308\|=2 | high |
| 0x5241bc | urom_rxcap_ring_stop | inverse of above | high |
| 0x52419c | urom_rxcap_ring_is_idle | 0x40904c b0 and 0x401308 b1 | high |
| 0x5242d4 | urom_rxcap_reset | exact twin lrom_rxcap_reset; RAM 1 | high |
| 0x524618 | urom_rxcap_set_mode | exact twin; RAM 1 | high |
| 0x5241d8 | urom_rx_path_power_cfg | 0x402070, 0x409468, 0x405008, 0x402050, 0x401d00 by mode | low |
| 0x524058 | urom_adc_set_402028_402020 | 0x402028 b16, 0x402020 b28 | low |
| 0x524074 | urom_dfe_load_401908 | pulse 0x401900, 0x401908=a0, 0x401904\|=1 | low |
| 0x524098 | urom_dfe_read_40197c_block | copies 10 words 0x40197c..0x4019a0 | high (structural) |
| 0x5240c0 | urom_clkgen_cfg_405000 | 0x405000 field writes | low |
| 0x524114 / 0x524150 | urom_cfg8_init_from_52b258 / cfg8_init | fill 8-word struct from table / args | low |
| 0x524170 | urom_dfe_set_401384 | two 16-bit halves | high (structural) |
| 0x524398 | urom_dfe_flush_40135c | 0x401308 b18 set, n zeros to 0x40135c | medium |
| 0x52458c | urom_dfe_set_lo_ratio_401354 | 0.78 twin lrom_dfe_set_lo_ratio_401354 | high |
| 0x5245cc | urom_rx_409468_bit1_synced | 0x409468 b1 + 0x2004 sync | low |
| 0x524630 | urom_dfe_load_test_tone_pattern | exact twin | high |
| 0x524694 | urom_rx_pulse_409468_bit0 | | high (structural) |
| 0x5246bc | urom_dfe_write_401324_zeros | | high (structural) |
| 0x5246d4 / 0x5247f0 | urom_dfe_config_decimator / decimator_disable | exact twins; RAM 1 each | high |
| 0x524804 | urom_dfe_set_decim_by_bw | 0x40130c[1:0],b2 and 0x401308[26:20] from tables 0x52ce00/0x52ce04 | medium |
| 0x524884 | urom_rxsync_cfg_40907c | 0x40907c fields | low |
| 0x5248ac | urom_rxsync_read_409074 | splits 0x409074 into two 6-bit fields, clamps to 24/16 | low |

**0x522034 urom_rxsync_detect_bw_cfo (medium):** arg1[1] = operating BW (1/2/4/8 -> index 0-3). Reads detected
format from 0x409030[19:16] and status 0x409020, sets *arg3 = 1e6/2e6/4e6/8e6, programs 0x409000 (mask 0xffff1007),
0x40901c, 0x409018, 0x409028=0x7ffffff. Then accumulates a lag-1 complex autocorrelation over the 32-sample MMIO
window at **0x4090bc** (the same window RAM 0x50a6bc snapshots), converts phase to Hz (x 159154 >> k), and if
hook30 says so, retunes the LO via hook31 (uphy_set_lo). Writes the residual to **0x40131c** (DFE frequency
correction, inferred), clears 0x409050, sets 0x40904c b0, 0x409000 |= 0x1001, 0x401308 |= 6. Returns 0xbad when
the CFO is out of range. This is a candidate for the "LO blips ~-18 kHz" seen during 8 MHz episodes.

### Coprocessor (custom-1 opcode 0x2b) accessors

Decoded ISA facts (medium, from usage patterns): f7=0x4e = write cop register (rs1=value, rs2[31:24]=index,
low bits sub-index); f7=0x58 = read selected register to rd; f7=0x1a = load block from memory (rs1=ptr,
rs2=len/4 | flags); f7=0x1b = store block to memory; f7=0x0b = wait for completion (no operands);
f7=0x57 = set stride window around 1a/1b; f7=0x00 = sync/nop; f7=0x35/0x77/0x76 = start command then read
result; f3=7 f7=0 = control (rs1=3, rs2=0/1).

| addr | name | evidence | RAM | conf |
|---|---|---|---|---|
| 0x524864 / 0x527ca0 | urom_cop_cmd_77_wait(_b) | f7 35,77,76 sequence; called by rxsync_detect_bw_cfo | 3 / 1 | low |
| 0x527c4c | urom_cop_store_two_halves | two store_strided + wait | 1 | low |
| 0x527e1c | urom_cop_load_tbl_52cb64 | loads 8 words from 0x52cb64 | 2 | medium |
| 0x527e5c / 0x527e6c / 0x527e7c / 0x527e8c | urom_cop_wr_reg74 / reg4a / reg50 / reg3e | single f7=4e write, index base 0x74/0x4a/0x50 or fixed 0x3e | 7/3/11/2 | high (structural) |
| 0x528028 | urom_cop_ctl2b_1000 | f7=2b ctl | 2 | low |
| 0x528184 | urom_tbl_52cbac_lookup_s8 | idx 1..39 -> s8 table 0x52cbac | 2 | high (structural) |
| 0x5281c8 / 0x5281d4 | urom_cop_ctrl3_set1 / cop_reset_enable | 0x405030 b15, 0x405020 b0/b7 pulse, then ctrl(3,0) | 1/1 | low |
| 0x528218 / 0x52a44c | urom_cop_init_regs60 / init_regs32 | default register loads | 1 / via setup | low |
| 0x528264 / 0x528288 | urom_cop_setup_mode_a / _b | init_regs32 + load_tbl_52cc84 + reg4a triples | 1/2 | low |
| 0x5290f8 | urom_cop_rd_reg_da_db | select 0xda/0xdb, f7=58 read | 1 | medium |
| 0x529218 | urom_cop_wr_reg3a_3b | | 1 | high (structural) |
| 0x529ef4 | urom_cop_status_any | f7 2e/30 OR, bit0 | 1 | low |
| 0x52a2d0 / 0x52a330 | urom_cop_load_strided / store_strided | 57/1a(1b)/57 | 2/6 | medium |
| 0x52a3c0 | urom_fftsize_to_bwidx | 32/64/128/256 -> 0/1/2/3 | 1 | high |
| 0x52a488 | urom_cop_load_tbl_52cc84 | loads 8 words from 0x52cc84 | via setup | medium |

## Data / rodata

| addr | name | contents |
|---|---|---|
| 0x52b100 | urom_clk_cpu_div_jumptable | 7 cases for 0x401d00[11:9]: /3, /5, /2, /4, /8, x2/3 (one case passes through) |
| 0x52b11c | urom_str_abort | "abort" |
| 0x52b124 | urom_str_libc_file | "common/libc/morse_libc_rom.c" |
| 0x52b144 / 0x52b16c | urom_digits_lower / upper | 36-char digit sets |
| 0x52b194 | urom_str_null | "<NULL>" |
| 0x52b19c | urom_rxsync_detbw_jumptable | 9 cases on 0x409030[19:16] |
| 0x52b1c0 | urom_tbl_s8_4x4_52b1c0 | 16 s8; also read by RAM 0x50a65c |
| 0x52b1d0 | urom_tbl_rx_desc_9x4 | 9 x {4,4,0x30..0x3a,0}; pointer stored by RAM 0x502bde and upper ROM 0x52f414 |
| 0x52b1f4 | urom_fft_size_by_bw | {32,64,128,256}; read by RAM 0x50b840 and 0x5237b8 |
| 0x52b204 | urom_adc_bw_jumptable | 21 cases (bw 1..20) for urom_adc_config_bw |
| 0x52b258 | urom_tbl_cfg8_52b258 | source for cfg8_init_from_52b258 |
| 0x52b354 | urom_adc_field_tbls_by_bw | five 4-entry tables (0x52b354/64/74/84/94) for 0x402000/0x402004 fields |
| 0x52b3a4 | urom_adc_code_rows_band_a | 3 bands x 13 rows x 16 B (0x52b3a4-0x52b613) |
| 0x52baa4 | urom_adc_code_rows_band_b | second row set, stride 8 (0x52baa4-0x52bc09) |
| 0x52b614 | urom_rodata_unref_52b614 | 0x52b614-0x52baa3 and 0x52bc0a-0x52bfff: words like 1000,100,0xd5, 0xe25..0x27f, 1/3/2 ramps, 0x20/0x3f; no direct code refs found |

The "~517 junk functions" in this range are linear-sweep artifacts in 0x52b100-0x52bfff and in mid-function
alignment padding; I did not name them.

## Registers learned

| addr | field | meaning | evidence |
|---|---|---|---|
| 0x1d00-0x1d3f | words | shared spinlocks/counters (amoswap/amoadd) across cores | hwsem helpers |
| 0x1a00 / 0x1b00 / 0x401700 / 0x401800 | +0 ctrl ([23:0] len, b24 go, b25 mode), +4 src, +8 dst, +0xc busy, +0x10, +0x30/34 desc FIFO, +0x38/+0x40 status | DMA channels 0-3 | dma helpers |
| 0x2004 | b1 | toggling sync/heartbeat (waited on) | 0x5201bc |
| 0x2108 | [5:1] | PLL multiplier, x 8 MHz | clk_pll_khz |
| 0x2098 | [7:6] | clock source select | clk_source_khz |
| 0x2124 | cmd, b13 busy | indirect sysctl access | 0x52125c |
| 0x2d20 | [13:12] | chip variant used for gating (0x809 -> 0) | several |
| 0x401d00 | b2 src, [11:9] div | CPU clock | clk_cpu_khz |
| 0x404000 / 0x404400 / 0x404600 | | intc priority / enable bitmap / ? | intc |
| 0x406040/44, 0x406020/24 | | 64-bit timer, compare | timers |
| 0x409000 | b0 capture-disable, b12, b30/b31, [12:3] mode | rx sync/capture control | rxsync_config, ring start/stop |
| 0x409030 | [19:16] | detected format/BW | detect_bw_cfo |
| 0x409044/48/4c | base/len/ctrl b0 | capture ring | ring start/stop (verified on HW earlier) |
| 0x4090bc.. | 32 words | sample window used for CFO | detect_bw_cfo |
| 0x409400 | b1,b2,b3,b4,b5,b6,b7 | resampler control pulses/enables | resamp_* |
| 0x409404 | Q0.32 | resampler ratio | resamp_set_ratio |
| 0x40131c | | frequency-correction word (inferred) | detect_bw_cfo |
| 0x402004 | [20:18] div, b17 clk enable, b21/22 | ADC clock | adc_config_bw, adc_enable |

## Open questions

- The cop region 0x524f88-0x527b9c (packet/PSDU-looking code using memcpy/memset, hooks 24/25, no custom ops
  until 0x526200) and 0x528300-0x52aa00 (dense custom ops) remain unnamed beyond RAM-called accessors. A
  focused pass with a custom-1 decoder plugin in Binja would make these readable.
- Exact semantics of f7 values other than 4e/58/1a/1b/0b/57.
- 0x522fa0 has no callers found by Binja; may be reached through a pointer in the upper ROM.
- Who reads the unreferenced rodata 0x52b614-0x52baa3 / 0x52bc0a-0x52bfff.
- Whether hook30 (0x50369c) is true in normal RX, i.e. whether every packet's CFO retunes the LO through
  uphy_set_lo. If yes, this explains LO blips seen during capture episodes (assumed, not tested).

## Notes for other regions (not renamed by me)

- Upper ROM 0x533760 (in sub_53370c) calls urom_rxsync_detect_bw_cfo; 0x53056e (in sub_53052c) calls
  urom_rxsync_config. 0x534fd0 installs the ROM default hook table 0x5372ec into 0x51fe80.
- Tables in 0x52c000+ used from here: 0x52cb64 and 0x52cc84 (cop coefficient blocks, 8 words), 0x52cbac
  (39 x s8), 0x52cca4 (`__clz_tab` for udivdi3), 0x52cda4 (clock root byte table), 0x52cdb0 (u16 per-bw
  0x409000 config), 0x52ce00/0x52ce04 (decimator by bw).
- RAM: 0x53809c (AON) installs hook table from 0x539ea8. 0x5011ac uphy_set_lo is hook31. RAM 0x50a65c reads
  0x52b1c0; 0x50b840 reads 0x52b1f4; 0x502bde stores &0x52b1d0 into 0x5384ac.
- DMEM 0x51fe80-0x51ff7f is the UPHY hook table (suggest `uphy_hook_table`).
- LPHY ROM: lrom 0x550ce4 `lrom_clk_cpu_cycles_per_us` divides (name OK); many unnamed lrom functions are now
  identified by their urom twins (e.g. lrom 0x5506b8..0x5509b0 = hwsem helpers, 0x551b90.. = adc/resamp/ring
  helpers, 0x552be4 = __divdi3, 0x552aa8 = fx_atan2_q16).
