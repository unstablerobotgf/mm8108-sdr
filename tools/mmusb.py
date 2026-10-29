#!/usr/bin/env python3
# Minimal MM81xx USB memory bridge (protocol from morse_driver/usb.c): cmd {dir,addr,len} on bulk OUT, data on bulk IN/OUT.
import struct, sys, time
import usb.core, usb.util

VID, PID = 0x325b, 0x8100
DIR_WRITE, DIR_READ = 0x00, 0x80
MAX_XFER = 16 * 1024  # USB_MAX_TRANSFER_SIZE in usb.c

class MM:
    def __init__(self):
        backend = None
        if sys.platform == 'win32':   # native Windows: WinUSB-bound device + bundled libusb-1.0.dll
            import libusb_package; backend = libusb_package.get_libusb1_backend()
        self.dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
        if self.dev is None: raise SystemExit('MM81xx not found')
        try:
            if self.dev.is_kernel_driver_active(0): self.dev.detach_kernel_driver(0)
        except (NotImplementedError, usb.core.USBError): pass
        self.dev.set_configuration()
        intf = self.dev.get_active_configuration()[(0, 0)]
        ep = lambda d: usb.util.find_descriptor(intf, custom_match=lambda e:
                 usb.util.endpoint_direction(e.bEndpointAddress) == d and
                 usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)
        self.out, self.inp = ep(usb.util.ENDPOINT_OUT), ep(usb.util.ENDPOINT_IN)
        try: self.inp.read(MAX_XFER, 20)   # drain stale IN data left by a previous process
        except usb.core.USBError: pass

    def _read1(self, addr, k, timeout):
        self.out.write(struct.pack('<III', DIR_READ, addr, k), timeout)
        return bytes(self.inp.read(k, timeout))

    def read(self, addr, n, timeout=250):
        buf = bytearray()
        while len(buf) < n:
            k = min(n - len(buf), MAX_XFER)
            try: buf += self._read1(addr + len(buf), k, timeout)
            except usb.core.USBTimeoutError:   # one retry after clearing halts
                self.dev.clear_halt(self.out); self.dev.clear_halt(self.inp)
                buf += self._read1(addr + len(buf), k, timeout)
        return bytes(buf)

    def r32(self, addr): return struct.unpack('<I', self.read(addr, 4))[0]

    def write(self, addr, data, timeout=250):
        for off in range(0, len(data), MAX_XFER):
            chunk = data[off:off + MAX_XFER]
            self.out.write(struct.pack('<III', DIR_WRITE, addr + off, len(chunk)), timeout)
            self.out.write(chunk, timeout)

    def w32(self, addr, v): self.write(addr, struct.pack('<I', v))

if __name__ == '__main__':
    m = MM()
    regs = [('CHIP_ID', 0x00002d20), ('MANIFEST_PTR', 0x00002d40), ('SECURE_BOOT_STATUS', 0x0011fff8),
            ('SEC_MANIFEST_PTR', 0x0011fffc), ('APPS_BOOT_ADDR', 0x00002084)]
    if '--ring' in sys.argv:
        regs += [('RING_BASE', 0x409044), ('RING_LEN', 0x409048), ('RING_CTRL', 0x40904c), ('RING_WCNT', 0x409050)]
    for name, a in regs:
        try: print(f'{name:20} @{a:08x} = 0x{m.r32(a):08x}')
        except usb.core.USBError as e: print(f'{name:20} @{a:08x} = ERROR {e}')
