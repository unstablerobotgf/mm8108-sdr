# BCF (tools/bcf/bcf_mf15457.bin): what the board config sets

ELF, 2510 B. Sections: `.board_config` at 0x11c680 (0x158 B), one `.regdom_XX` (AU/CA/EU/GB/JP/US, all linked at
0x11c7d8; the host maps the one it needs), `.board_desc`, `.chips`, `.build_ver` (text).
In the combined MAC DB (`rom/mm8108_mac.elf.bndb`) the AU regdom is the one mapped.

`.board_config` = `0xdeadbeef`, 4-byte check/ID `71 fb 30 aa`, header `0d 00 01 00 48 01 00 00`, then TLVs
`{u8 tag, u8 len, value}` ending in `3e 3e` padding. PHY side reads them through UPHY message 0x3010
(`lphy_bcf_get` / `_get_field`, which supply a default when a tag is absent). Values below are little-endian.

| Tag | Value in this BCF | Loader (LPHY) | Meaning (from loader; consumer named where traced) |
|:--|:--|:--|:--|
| 0x00 | 0x0000000f | load_misc -> cfg+0 | board/feature word (inferred) |
| 0x01 | 0x63 | load_txpwr -> +0xf2 (default 0x30) | TX power limit, byte |
| 0x02 | absent | load_txpwr -> +0xf0 (default 0x30) | TX power limit, byte |
| 0x04 | 07 07 07 07 | load_txpwr -> +0x121 (and +0x12b) | per-bw TX power table (1/2/4/8 MHz) |
| 0x05 | 0c 17 1b 19 | load_txpwr -> +0x125 | per-bw TX power table |
| 0x06 | 1f 26 | load_txpwr -> +0x129/+0x12a | TX pair |
| 0x07 | absent | load_misc -> +0xc (default 0x1b) | |
| 0x2e-0x3a | 0x2e=0x1800, 0x31=0x1000, 0x32=0x0800, 0x34=0x1000, 0x38=-48 | load_fem_params -> fem cfg +0x3c.. | FEM timing/levels |
| 0x48 | 0x40 | load_misc (default 0x40) | |
| 0x49, 0x4a | 0x118, 0x118 | load_misc (default 0xb4) | |
| 0x4b-0x58 | 4-byte groups (09.., 0b 10 15 16, 1d, ...) | load_txpwr -> +0x12c..+0x1c2 | per-bw TX power / PA tables |
| 0x60 | 0x1800 | load_fem_params -> +0x2c | FEM |
| 0x61 | 0x1f400000 | fields: fem +0x5c/+0x5b/+0x5e, misc flags, txpwr [29:22] = 0x7d max | packed flags + TX cap |
| 0x62 | 0x00808222 | load_txpwr fields [12:0], [20:13], [28:21], b29 | TX power params |
| 0x66 | 0x100 | load_fem_params -> +0x20, b8 -> +0x56 | FEM flag |
| 0x69 | 0x00000a44 | load_chan_overrides header: 4 packed u16 (tag 0x74), 10 range tags, 2 extra (0x75/0x76) | |
| 0x6a-0x73 | 8 B each | per-channel-range fields (4/1/5/7/4/2/2 bits) per bw mask | consumed by `uphy_bcf_chan_*` incl. `uphy_bcf_chan_clamp_power`: TX power per channel |
| 0x74 | 00 20 b2 26 91 22 f0 1b | 4 packed values via 0x546a4e -> +0xc8 | |
| 0x75, 0x76 | 3,3,5,5 / 0xf8,0xf8,0xf5,0xf5 (u16) | -> +0x138 | |
| 0x77 | 3f 50 .. | two dB/8 -> linear Q16 (0x7f = max, 0x80 = none) | TX limits (inferred) |

Conclusion (2026-10-25): every BCF tag the PHY reads goes to TX power, PA/FEM timing or regulatory tables.
Nothing reaches the RX analog filter (0x4020xx RX codes). The +-4 MHz RX rolloff is not set from the BCF.
