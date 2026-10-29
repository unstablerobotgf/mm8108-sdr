# YAPS host-interface client over the USB memory bridge (spec distilled from morse_driver yaps_hw.c/skbq.c/command.c).
import os, sys, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # run.sh uses python -I
from mmusb import MM

HOST_TBL_PTR = 0x2d40
SKB_HDR_LEN, SKB_OFFSET = 40, 26          # driver uses EXTRA_TX_OFFSET=26
POOL_CMD, POOL_CMD_RESP = 1, 5
CH_COMMAND = 0xFE
F_REQ, F_RESP, F_EVT = 1, 2, 4

def crc7(word):
    # crc7_be over the 4 bytes MSB-first, poly 0x12 left-aligned, result >> 1 (utils.h)
    c = 0
    for b in word.to_bytes(4, 'big'):
        c ^= b
        for _ in range(8): c = ((c << 1) ^ 0x12 if c & 0x80 else c << 1) & 0xff
    return c >> 1

def calc_padding(n): return (-n) % 4

def delim(size, pool, irq):
    d = (size & 0x3FFF) | (pool & 7) << 14 | calc_padding(size) << 17 | (1 if irq else 0) << 19
    return d | crc7(d & 0x1FFFFFF) << 25

class Yaps:
    def __init__(self, m=None, verbose=False):
        self.m = m or MM(); self.v = verbose; self.seq = int.from_bytes(os.urandom(2), 'little') % 0xfff  # fresh host_id per process; fw drops repeats
        ht = self.m.r32(HOST_TBL_PTR)
        if not ht or self.m.r32(ht) != 0xdeadbeef: raise SystemExit(f'firmware not running (host table 0x{ht:x})')
        ext = self.m.r32(ht + 0x18); n = (self.m.r32(ext) + 3) & ~3
        et = self.m.read(ext, n); self.mac = et[4:10]; self.yt = None; self.rx_csum = False
        i = 10
        while i + 4 <= len(et):
            tag, ln = struct.unpack_from('<HH', et, i)
            if ln == 0: break
            if tag == 3: self.yt = et[i+4:i+4+36]
            if tag == 2: self.rx_csum = bool(et[i+4])
            i += ln
        if self.yt is None: raise SystemExit('no YAPS TLV in extended host table')
        (self.flags, self.ysl, self.yds, self.sts) = struct.unpack_from('<B3xIII', self.yt, 0)
        self.cmd_q_size = self.yt[0x1B]; self.reserved = struct.unpack_from('<H', self.yt, 0x20)[0]
        if self.v: print(f'mac {self.mac.hex(":")} ysl 0x{self.ysl:x} yds 0x{self.yds:x} sts 0x{self.sts:x} '
                         f'cmd_q {self.cmd_q_size} reserved {self.reserved} rx_csum {self.rx_csum}')

    def status(self):
        t0 = time.time()
        while True:
            s = struct.unpack('<18I', self.m.read(self.sts, 72))
            if s[17] == 0: break
            if time.time() - t0 > 0.1: raise TimeoutError('status block stayed locked')
        if s[15]: raise RuntimeError('YAPS delimiter CRC failure latched on chip; reboot needed')
        return {'cmd_free': s[1], 'cmd_pkts': s[9], 'fc_pkts': s[12], 'rx_bytes': s[14], 'crc_fail': s[15]}

    def _parse(self, buf):
        out, i = [], 0
        while i + 4 <= len(buf):
            d, = struct.unpack_from('<I', buf, i)
            if d == 0: break
            size = d & 0x3FFF; pad = (d >> 17) & 3
            if crc7(d & 0x1FFFFFF) != d >> 25 or not size or pad != calc_padding(size):
                raise RuntimeError(f'bad RX delimiter 0x{d:08x} at {i}')
            n = size - self.reserved; out.append(((d >> 14) & 7, buf[i+4:i+4+n])); i += 4 + n + pad
        return out

    def send(self, msg_id, payload=b'', timeout=0.6, vif=0):
        self.seq = self.seq % 0xfff + 1; hid = self.seq << 4
        cmd = struct.pack('<HHHHHH', F_REQ, msg_id, len(payload), hid, vif, 0) + payload
        skb = struct.pack('<BBHBBH', 0xAA, CH_COMMAND, len(cmd), SKB_OFFSET, 0, 0) + bytes(32) + bytes(SKB_OFFSET) + cmd
        skb += bytes(-len(skb) % 4)
        need = -(-(len(skb) + self.reserved) // 256) + 2
        t0 = time.time()
        while True:
            st = self.status()
            if st['cmd_free'] >= need and self.cmd_q_size - st['cmd_pkts'] > 0: break
            if time.time() - t0 > timeout: raise TimeoutError(f'no cmd pool space {st}')
            time.sleep(0.002)
        d = delim(len(skb) + self.reserved, POOL_CMD, True)
        self.m.write(self.yds, struct.pack('<I', d) + skb)
        while time.time() - t0 < timeout:
            st = self.status()
            if st['rx_bytes']:
                for pool, pkt in self._parse(self.m.read(self.ysl, min(st['rx_bytes'], 32768))):
                    if pkt[0] != 0xAA: continue
                    ln, off = struct.unpack_from('<H', pkt, 2)[0], pkt[4]
                    r = pkt[SKB_HDR_LEN + off: SKB_HDR_LEN + off + ln]
                    fl, mid, rlen, rhid = struct.unpack_from('<HHHH', r, 0)
                    if self.v: print(f'  rx pool {pool} flags 0x{fl:x} id 0x{mid:04x} len {rlen} host_id 0x{rhid:x}')
                    if fl & F_RESP and mid == msg_id and (rhid & 0xfff0) == hid:
                        status, = struct.unpack_from('<i', r, 12)
                        return status, r[16:12 + rlen]
            time.sleep(0.002)
        raise TimeoutError(f'no response to 0x{msg_id:04x}')

if __name__ == '__main__':
    import sys
    y = Yaps(verbose=True); print('status', y.status())
    st, data = y.send(0x0002)  # GET_VERSION
    print(f'GET_VERSION status {st}: {data!r}')
