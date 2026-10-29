# MM8108 PHY reverse-engineering brief (shared by all region agents)

## Target
Morse Micro MM8108 (802.11ah HaLow) firmware. Two RISC-V (rv32gc) PHY cores:
- **UPHY** (upper PHY, 192 MHz): RAM code 0x500000-0x50fb9e (.uphy_imem), DMEM 0x510000-0x51ffff
  (0x510000-0x510394 initialised, rest bss/stack; live stack near 0x51e000), ROM 0x520000-0x537fff,
  always-on RAM 0x538000-0x53a850 (.uphy_aonmem, mixed code/rodata).
- **LPHY** (lower PHY / RF + calibration): RAM code 0x540000-0x546ab2 (.lphy_imem), DMEM 0x548000-0x54ffff,
  ROM 0x550000-0x559fff, always-on 0x55c000-0x55d280 (.lphy_aonmem, mostly rodata tables).
- PHY/RF/DFE registers live at 0x400000-0x40ffff (mapped as zero-filled RW so xrefs to registers work:
  use bn_xrefs_to on a register address to find every function touching it).
- No symbols, almost no strings. The ROMs carry `common/libc/morse_libc_rom.c`. Logging goes through
  hook pointers (ROM calls a RAM-installed function pointer), with numeric trace IDs, not format strings.

## Binja
- One combined DB: handle `1fd851abcb69f454:ELF` (file rom/mm8108_phy.elf.bndb). Every tool needs `binary`.
- Do NOT save the DB (the coordinator saves). Do NOT rename or comment anything outside your assigned range.
- Linear sweep produced junk "functions" inside data tables (many 2-8 byte `__noreturn` stubs). Do not name
  those; identify the table instead and define a data symbol at its start.

## Known facts (verified on hardware unless marked)
The region reports in this directory (index: `README.md`) hold the full notes. Highlights:
- lphy_set_lo 0x542e60: synth word = 2*round(f*2^24/32e6), 880 MHz band split, ADPLL reset sequence.
- 0x409404 resampler ratio = f_ADC/(2*f_out), Q0.32. 0x402004[20:18] ADC divider. 0x402050 bit26.
- AGC fixed-gain index 0x403004[13:8] (0 = max gain, about 3 dB/step).
- DC-offset cal: LPHY message 0x3017 -> 0x54666e lphy_dc_offset_cal, iterates 21 gain indices, table
  0x55c254 (21 x 12 B); applied codes go to 0x402018/1c/24/2c/44 (lphy_apply_dccal_codes 0x54201c).
- IQ cal coefficients at 0x401374 (tan(phi), 1/(g cos(phi))). 0x401354 = 2^36*32MHz/f_LO (a ratio, not an NCO).
- Capture ring: write pointer 0x40906c, 16-bit I/Q words. UPHY ring-take ASSERT entry at 0x50e502.
- FEM pin state machine at 0x50ce18 (GPIO11/12).
- LPHY message loop 0x5401e8 dispatches host/UPHY messages by ID (e.g. 0x3017).

## Naming conventions
- RAM functions: `uphy_<subsystem>_<verb>` / `lphy_<subsystem>_<verb>`. ROM functions: `urom_...` / `lrom_...`.
- Subsystem words to reuse: msg, agc, fem, lo/pll, adc, dfe, rxcap (capture ring), dccal, iqcal, gain, tx, rx,
  timer, irq/intc, log, hook, sched, mbox, dma, pkt, fft, fx (fixed-point math).
- Name from evidence only (registers touched, constants, callers/callees, data flow). If the role is only
  partly clear, use a descriptive-structural name such as `uphy_rmw_403xxx_field` or `lrom_table_lookup_u16`
  rather than a confident wrong one. Leave genuinely opaque functions unnamed.
- Data: `<core>_<what>` for globals/tables, e.g. `lphy_dccal_state`, `uphy_msg_handler_table`.
- Comments: 1 to 2 lines, facts only (registers, units, gotchas). Mark anything inferred with "(inferred)".
  No em-dashes anywhere (use colons, commas, parentheses).

## Method (suggested)
1. bn_function_list over your range; skip junk. Note sizes and call counts.
2. Leaves first (memcpy-like, register RMW helpers, math), then work up the call graph.
3. Use register xrefs: bn_xrefs_to on 0x40xxxx addresses names many hardware helpers quickly.
4. For dispatchers/jump tables, name the table and annotate each case's message ID.
5. bn_symbol_define (type FunctionSymbol for functions, DataSymbol default for data); bn_comment_set for comments.

## Deliverable
- Names and comments in the DB for your range.
- A report at `docs/phy_re/<region>.md`: subsystem overview, a table of
  every function you named (address, name, one-line role, evidence, confidence: high/medium/low), data
  tables/globals, registers learned (address, field, meaning, evidence), open questions, and
  "notes for other regions" (facts about out-of-range functions you saw, which you must not rename yourself).
- Final reply: under 300 words: counts named, the most important findings, and anything surprising.
