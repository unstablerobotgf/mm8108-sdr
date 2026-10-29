# UPHY coprocessor (RISC-V custom-1, opcode 0x2b)

Census (2026-10-26, scratchpad cop_census.py, linear sweep of UPHY RAM 0x500000-0x50fb9e and ROM 0x520000-0x52c000,
0x52cf10-0x535000): 485 instructions, 56 (funct3, funct7) kinds; all funct3 = 0 except two f3 = 7 control ops.
Listing tool: scratchpad coplist.py (capstone 5.0.7 in scratchpad/pylib + custom-1 decode).

| f7 | n | operands (constant tracking) | meaning | confidence |
|:--|:--|:--|:--|:--|
| 0x4e | 136 | rs1 value, rs2[31:24] reg index (0x2f/0x46/0x47/0x57..) | write engine register | high |
| 0x58 | 9 | rd | read selected register | medium |
| 0x38 | 47 | rs1 bit pos, rs2 mask, rd | field extract, >=2 MHz S1G SIG (48 b) | verified vs 802.11ah |
| 0x3b | 14 | same | field extract, 1 MHz S1G SIG (36 b) | verified vs 802.11ah |
| 0x39 | 3 | rs1 = 3, rs2 = 0, rd | before each SIG parse: CRC/valid | assumed |
| 0x1a / 0x1b | 23 / 27 | rs1 ptr, rs2 len/4 | flags | block load / store DMEM <-> engine | medium |
| 0x57 | 19 | | stride window open/close | medium |
| 0x0b | 11 | none | wait for completion | medium |
| 0x00 | 15 | none | sync | medium |
| 0x04, 0x11, 0x22 | | packed descriptors | FFT setup/launch (uphy_fft_run) | medium |
| 0x35 / 0x77 / 0x76 | | start cmd, read result | rx sync / BW+CFO detect | medium |
| others (~40) | 1-13 | | unknown; dense in ROM 0x528300-0x52aa00 (demod) | |

SIG field map, 0x38 (positions in the 48-bit S1G_SHORT SIG): 0 reserved, 2 UL ind, 5 w2 Nsts, 7 w9 ID, 16 SGI,
19 w4 MCS, 23 smoothing, 24 aggregation, 25 w9 length, 34 w2 ACK ind, 36/37 Doppler. 0x3b (S1G_1M): 0 w2 Nsts,
2 SGI, 7 w4 MCS, 11 aggregation, 12 w9 length, 21 w2 ACK ind, 23 smoothing, 24 Doppler.
Parse sites: RAM 0x50d34c, ROM 0x526260, 0x5264a2 (>=2 MHz), 0x5265c8 (1 MHz).

Capabilities: FFT/IFFT 32/64/128/256 (uphy_fft_run 0x50f7f4); HW SIG decode; spectrum measurement
(uphy_meas_spectrum_run: capture -> FFT -> accumulate, 31.25 kHz bins); rx sync; TX symbol generation into the TX
FIFO path; coefficient tables 0x52cb64 / 0x52cc84. Separate MMIO command engine at 0x3800 (descriptor ring 0x51665c,
cmds (len<<16)|0x2fd/0x2fe), not the ISA engine.
Next: Binja arch-hook plugin decoding custom-1 as intrinsics so the broken functions decompile.
