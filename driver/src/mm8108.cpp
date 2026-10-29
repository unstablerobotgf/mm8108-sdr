#include "mm8108.h"
#include "patches.h"
#include "packer_blob.h"
#include <libusb.h>
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

#pragma comment(lib, "winmm.lib")

// ============================================================================ chip addresses and constants
namespace {
// USB memory bridge
constexpr uint16_t VID = 0x325b, PID = 0x8100;
constexpr uint8_t EP_OUT = 0x02, EP_IN = 0x82;
constexpr uint32_t XFER = 16384;

// boot (boot.py) and identity
constexpr uint32_t AON_LATCH = 0x405020, AON_BASE = 0x2114, MSI = 0x4100, MANIFEST_PTR = 0x2d40, BOOT_ADDR = 0x2084;
constexpr uint32_t CHIP_ID = 0x2d20;
// OTP controller (morse_driver mm81xx_otp_read): power 0x4010/14/18, read enable 0x401c/20, bank 0x4034, ECC 0x4048
constexpr uint32_t OTP_CTRL = 0x400c, OTP_BANK = 0x4034, OTP_DATA = 0x4040, OTP_ECC = 0x4048;

// I/Q ring DMA (owner.py): the safe ring sits in the YAPS tc_tx pool, unused since we never TX
constexpr uint32_t CTRL = 0x40904c, BASE = 0x409044, LEN = 0x409048, PTR = 0x40906c;
constexpr uint32_t SAFE_BASE = 0x229000, SAFE_LEN = 30144;
constexpr uint32_t PANIC_MAGIC = 0x5380d4;                  // UPHY panic dump marker: 0xfee1dead once parked in the ASSERT
constexpr const char* PARK_ERR = "UPHY did not park after ring take";
// ASSERT_SITE and ASSERT_BCAST live in patches.h with the other patch sites

// packed mode (tools/packer.s), all in UPHY DMEM: cross-core loads/stores cost ~40 cycles, local ones ~1
constexpr uint32_t PK_CTRL = 0x510f00;                      // ring layout: PK_IN_* / PK_OUT_* in packer_blob.h
constexpr uint32_t PK_CAVE = 0x50fba0;                      // packer code cave below .uphy_imem's end

// RF/ADPLL block (LPHY set_lo), clocks and resampler
constexpr uint32_t RF = 0x402000, SYNTH = 0x40204c;
constexpr uint32_t CLKDIV = 0x402004;                       // [20:18] ADC = VCO / (128 << div), copied from the LPHY div[bw] table
constexpr uint32_t LPHY_DIV_BW8 = 0x55ccbc;                 // that table's 8 MHz entry
constexpr uint32_t RATIO = 0x409404;                        // f_ADC / (2 f_out) in Q0.32, valid in [0.5, 1); fw wraps above 1024 MHz
// DC servo: bit0 enable, [3:1] shift (fw 4 -> 80 kHz HPF), bit4 hold. Gain: four 6-bit reduction fields
constexpr uint32_t DC_CTRL = 0x401334, GAIN_REG = 0x403004;
constexpr uint32_t GAIN_FIELDS = 0x3fu << 2 | 0x3fu << 8 | 0x3fu << 14 | 0x3fu << 20;

// reader schedule (vstream2 / ringstream defaults, proven gapless at 8 MS/s)
constexpr double MARGIN = 300e-6, BACKDATE = 100e-6, SCHED_FS = 7.9996e6;
constexpr size_t SPSC = 1 << 22;
enum { K_DATA, K_PTR, K_CTL };                              // Slot::kind

const uint8_t NOP[4] = { 0x13, 0, 0, 0 };

// ============================================================================ small helpers
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
void pad4(std::vector<uint8_t>& b) { while (b.size() % 4) b.push_back(0xff); }
// fw RAM (morse_driver coredump regions): host-table pointers outside these are stale garbage, never dereferenced
bool inRam(uint32_t a) { return (a >= 0x100000 && a < 0x130000) || (a >= 0x200000 && a < 0x2b0000); }
// RISC-V jal x0, off
uint32_t jal0(int32_t off) {
    return ((off >> 20) & 1) << 31 | ((off >> 1) & 0x3ff) << 21 | ((off >> 11) & 1) << 20 | ((off >> 12) & 0xff) << 12 | 0x6f;
}

std::vector<uint8_t> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

std::set<DWORD> ownThreads() {
    std::set<DWORD> ids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == GetCurrentProcessId()) ids.insert(te.th32ThreadID);
    CloseHandle(snap);
    return ids;
}

std::string usbPath(libusb_device* d) {                     // "bus-port.port..."
    uint8_t ports[8]; int n = libusb_get_port_numbers(d, ports, 8);
    std::string p = std::to_string(libusb_get_bus_number(d));
    for (int i = 0; i < n; i++) p += (i ? "." : "-") + std::to_string(ports[i]);
    return p;
}

// ============================================================================ ELF and the patch table
struct Elf {                                                // minimal ELF32: program headers + named sections
    struct Ph { uint32_t type, off, va, pa, fsz, msz; };
    uint32_t entry = 0;
    std::vector<Ph> ph;
    std::map<std::string, std::pair<uint32_t, uint32_t>> sec;   // name -> (file offset, size)

    explicit Elf(const std::vector<uint8_t>& d) {
        if (d.size() < 0x34 || memcmp(d.data(), "\x7f" "ELF", 4)) throw std::runtime_error("not an ELF");
        entry = rd32(&d[0x18]);
        const uint32_t phoff = rd32(&d[0x1c]), shoff = rd32(&d[0x20]);
        const uint16_t phes = rd16(&d[0x2a]), phn = rd16(&d[0x2c]), shes = rd16(&d[0x2e]), shn = rd16(&d[0x30]), shstr = rd16(&d[0x32]);
        for (int i = 0; i < phn; i++) {
            const uint8_t* p = &d[phoff + i * phes];
            ph.push_back({ rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12), rd32(p + 16), rd32(p + 20) });
        }
        const uint32_t names = rd32(&d[shoff + shstr * shes + 16]);
        for (int i = 0; i < shn; i++) {
            const uint8_t* s = &d[shoff + i * shes];
            sec[std::string((const char*)&d[names + rd32(s)])] = { rd32(s + 16), rd32(s + 20) };
        }
    }
};

// word at physical address a inside a PT_LOAD segment of the image
uint32_t fwWord(const Elf& e, const std::vector<uint8_t>& d, uint32_t a) {
    for (auto& p : e.ph)
        if (p.type == 1 && p.pa <= a && a + 4 <= p.pa + p.fsz) return rd32(&d[p.off + a - p.pa]);
    char b[64]; snprintf(b, sizeof b, "patch site 0x%06x is outside the firmware image", a);
    throw MMError(MMSDR_ERR_FIRMWARE, b);
}

std::vector<uint32_t> patchSites() {                        // same order as gen_patches.py signs them
    std::vector<uint32_t> s;
    for (const MMPatch& p : RING_PATCHES) s.push_back(p.addr);
    s.push_back(CLAMP_SITE.win + CLAMP_SITE.off);
    for (const MMSite& d : DIV2_SITES) s.push_back(d.win + d.off);
    s.push_back(ASSERT_SITE);
    s.push_back(ASSERT_BCAST);
    return s;
}

uint64_t fnv64(const std::vector<uint32_t>& ws) {           // FNV-1a over the little-endian bytes, same as gen_patches.py
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t w : ws)
        for (int i = 0; i < 4; i++) { h ^= (w >> (8 * i)) & 0xff; h *= 0x100000001b3ull; }
    return h;
}

uint32_t patchedWord(const MMPatch& p, uint32_t stock) { return (stock & ~p.mask) | p.bits; }

// stock words at every patch site come from the user's firmware ELF, never from this repo; STOCK_SIG pins the image
std::map<uint32_t, uint32_t> stockWords(const Elf& e, const std::vector<uint8_t>& fw) {
    std::map<uint32_t, uint32_t> stock;
    std::vector<uint32_t> ws;
    for (uint32_t a : patchSites()) { uint32_t w = fwWord(e, fw, a); stock[a] = w; ws.push_back(w); }
    if (fnv64(ws) != STOCK_SIG) throw MMError(MMSDR_ERR_FIRMWARE, "firmware image does not match the patch table (built for mm8108b2-rl.bin)");
    return stock;
}

// fw_info TLVs: type 1 = BCF load address, type 3 = BCF area size
std::pair<uint32_t, uint32_t> bcfArea(const Elf& e, const std::vector<uint8_t>& fw) {
    const auto fi = e.sec.at(".fw_info");
    uint32_t addr = 0, size = 0;
    for (uint32_t i = 0; i + 4 <= fi.second;) {
        const uint16_t t = rd16(&fw[fi.first + i]), l = rd16(&fw[fi.first + i + 2]);
        if (t == 1) addr = rd32(&fw[fi.first + i + 4]);
        if (t == 3) size = rd32(&fw[fi.first + i + 4]);
        if (t == 0 && l == 0) break;
        i += 4 + l;
    }
    return { addr, size };
}

// .board_config + .regdom_<cc> out of the BCF ELF, each padded to 4 B
std::vector<uint8_t> bcfBlob(const std::vector<uint8_t>& bcf, const std::string& regdom) {
    const Elf b(bcf);
    std::vector<uint8_t> blob;
    for (const std::string& n : { std::string(".board_config"), ".regdom_" + regdom }) {
        auto it = b.sec.find(n);
        if (it == b.sec.end()) throw MMError(MMSDR_ERR_FIRMWARE, "BCF has no " + n + " section");
        std::vector<uint8_t> part(bcf.begin() + it->second.first, bcf.begin() + it->second.first + it->second.second);
        pad4(part);
        blob.insert(blob.end(), part.begin(), part.end());
    }
    return blob;
}

// ============================================================================ YAPS framing (yaps.py)
uint8_t crc7(uint32_t w) {                                  // crc7_be over the word MSB-first, poly 0x12 left-aligned (morse utils.h)
    uint8_t c = 0;
    for (int i = 3; i >= 0; i--) {
        c ^= (uint8_t)(w >> (8 * i));
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x12) : (uint8_t)(c << 1);
    }
    return c >> 1;
}
uint32_t padding(uint32_t n) { return (4 - n % 4) % 4; }
uint32_t delim(uint32_t size, uint32_t pool, bool irq) {
    uint32_t d = (size & 0x3fff) | (pool & 7) << 14 | padding(size) << 17 | (irq ? 1u : 0u) << 19;
    return d | (uint32_t)crc7(d & 0x1ffffff) << 25;
}

// walk the RX pool for the response to (id, hid); true when found, with fw status and payload
bool yapsFindResponse(const std::vector<uint8_t>& buf, int reserved, uint16_t id, uint16_t hid, int32_t& status, std::vector<uint8_t>* resp) {
    for (size_t i = 0; i + 4 <= buf.size();) {
        const uint32_t dd = rd32(&buf[i]);
        if (!dd) break;
        const uint32_t size = dd & 0x3fff, pad = (dd >> 17) & 3;
        if (crc7(dd & 0x1ffffff) != dd >> 25 || !size || pad != padding(size)) throw MMError(MMSDR_ERR_CMD, "bad YAPS RX delimiter");
        const uint32_t n = size - reserved;
        const uint8_t* pkt = &buf[i + 4];
        if (i + 4 + n <= buf.size() && pkt[0] == 0xAA) {   // skb: 0xAA 0xFE, len, hdr pad; command header after 40 + pad bytes
            const uint8_t* r = pkt + 40 + pkt[4];
            const uint16_t fl = rd16(r), mid = rd16(r + 2), rlen = rd16(r + 4), rhid = rd16(r + 6);
            if ((fl & 2) && mid == id && (rhid & 0xfff0) == hid) {
                memcpy(&status, r + 12, 4);
                if (resp) resp->assign(r + 16, r + 12 + rlen);
                return true;
            }
        }
        i += 4 + n + pad;
    }
    return false;
}
}   // namespace

// ============================================================================ reader data structures
struct MM8108::Slot {                                       // one IN transfer: ring data, pointer read or live register read
    MM8108* self;
    libusb_transfer* t;
    uint8_t buf[65536];                                     // up to 16384 words per data read
    int kind;                                               // K_DATA / K_PTR / K_CTL
    int64_t tag;                                            // K_DATA: ring word position of buf[0] relative to the run start
    uint32_t n;                                             // bytes requested
    volatile bool done;
    int status, len;
    CtlP ctl;                                               // K_CTL: receives the word or the failure
};

struct MM8108::OutX {                                       // OUT transfer user_data: completes a ctl write once the bytes are out
    MM8108* self;
    CtlP p;
};

struct MM8108::RingRun {                                    // reader schedule for one streaming run
    uint32_t base, ptrReg;                                  // ring being read and its fw write pointer
    int64_t len;                                            // ring length in words
    int spw;                                                // samples per word (2 when packed)
    int ch, k, depth, waitUs;                               // knobs snapshot: words per read, data reads per ptr read, reads in flight, event wait
    int64_t p0 = 0;                                         // ring word offset where this run started
    int64_t pMod = 0, pAbs = 0;                             // last fw pointer (words, modulo ring) and its unwrapped position
    int64_t next = 0;                                       // next word to request, relative to p0
    double tFix = 0;                                        // host time of the last pointer fix
    int sinceP = 0;                                         // data reads since the last pointer read
    std::vector<int64_t> pending;                           // tags of data reads completed since the last pointer read (lap check)
    bool valid(uint32_t v) const { return v >= base && v < base + 4 * len && (v - base) % 4 == 0; }
};

MM8108::MM8108(IQFn cb) : onIQ(std::move(cb)), spsc(SPSC) {}

MM8108::~MM8108() {
    stop(); close();
    for (Slot* s : freeSlots) { libusb_free_transfer(s->t); delete s; }
    for (Slot* s : zombies) { libusb_free_transfer(s->t); delete s; }
    for (libusb_transfer* t : freeOut) { delete (OutX*)t->user_data; delete[] t->buffer; libusb_free_transfer(t); }
    if (ctx) libusb_exit(ctx);
}

void MM8108::setFiles(std::string fw, std::string bcf, std::string country) {
    fwPath = std::move(fw); bcfPath = std::move(bcf); regdom = std::move(country);
}

MM8108::Stats MM8108::stats() const {
    return { nSamples, nBadPtr, nLapped, nUsbErr, nOverflow, nClipped, nHops, nReboots, nPark };
}

// ============================================================================ sync bridge (mmusb.py)
int MM8108::openUsb() {
    if (h) return 0;
    if (!ctx) {
        // libusb >= 1.0.24 completes transfers on its own IOCP thread; at normal priority it gets preempted -> late
        // completions -> ring laps. Threads that appear across libusb_init are libusb's: make them time-critical.
        const std::set<DWORD> before = ownThreads();
        if (libusb_init(&ctx)) { ctx = nullptr; err = "libusb_init failed"; return MMSDR_ERR_USB; }
        for (DWORD id : ownThreads()) {
            if (before.count(id)) continue;
            if (HANDLE th = OpenThread(THREAD_SET_INFORMATION, FALSE, id)) {
                SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL); CloseHandle(th); boosted++;
            }
        }
    }
    libusb_device** devs = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &devs);
    int code = MMSDR_ERR_NO_DEVICE;
    err = "MM8108 (325b:8100) not found";
    for (ssize_t i = 0; i < n && !h; i++) {
        libusb_device_descriptor d;
        if (libusb_get_device_descriptor(devs[i], &d) || d.idVendor != VID || d.idProduct != PID) continue;
        const std::string p = usbPath(devs[i]);
        if (!dev.path.empty() && p != dev.path) continue;
        libusb_device_handle* hh = nullptr;
        int r = libusb_open(devs[i], &hh);
        if (r) {
            code = r == LIBUSB_ERROR_ACCESS || r == LIBUSB_ERROR_BUSY ? MMSDR_ERR_ACCESS : MMSDR_ERR_USB;
            err = "open " + p + ": " + libusb_error_name(r) + " (WinUSB bound? another program using it?)";
            continue;
        }
        libusb_set_configuration(hh, 1);                    // WinUSB may refuse; harmless
        if ((r = libusb_claim_interface(hh, 0))) {
            libusb_close(hh); code = MMSDR_ERR_ACCESS; err = "claim " + p + ": " + libusb_error_name(r);
            continue;
        }
        h = hh; dev.path = p;                               // pinned: re-opens after boot find the same device
        char s[64] = "";
        if (d.iSerialNumber && libusb_get_string_descriptor_ascii(h, d.iSerialNumber, (unsigned char*)s, sizeof s) < 0) s[0] = 0;
        dev.serial = s;
    }
    if (devs) libusb_free_device_list(devs, 1);
    if (!h) return code;
    uint8_t tmp[XFER]; int got;
    libusb_bulk_transfer(h, EP_IN, tmp, XFER, &got, 20);    // drain stale IN data
    err.clear();
    return 0;
}

std::vector<MM8108::DevInfo> MM8108::list(const std::set<std::string>& skipPaths) {
    std::vector<DevInfo> out;
    libusb_context* c = nullptr;
    if (libusb_init(&c)) return out;
    libusb_device** devs = nullptr;
    const ssize_t n = libusb_get_device_list(c, &devs);
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_descriptor d;
        if (libusb_get_device_descriptor(devs[i], &d) || d.idVendor != VID || d.idProduct != PID) continue;
        DevInfo x; x.path = usbPath(devs[i]);
        out.push_back(x);
    }
    if (devs) libusb_free_device_list(devs, 1);
    libusb_exit(c);
    for (DevInfo& x : out) {                                // claim each briefly for serial/uid; never re-claim one this process streams
        if (skipPaths.count(x.path)) { x.busy = true; continue; }
        MM8108 probe(nullptr);
        if (probe.openDevice(x.path)) x.busy = true;
        else x = probe.info();
    }
    return out;
}

int MM8108::openDevice(const std::string& path) {
    stop(); close();
    dev = DevInfo(); dev.path = path;
    if (int r = openUsb()) return r;
    try { readIdentity(); }
    catch (const MMError& e) { err = e.what(); close(); return e.code; }
    catch (const std::exception& e) { err = e.what(); close(); return MMSDR_ERR_USB; }
    return 0;
}

void MM8108::close() {
    if (!h) return;
    libusb_release_interface(h, 0); libusb_close(h); h = nullptr;
}

std::vector<uint8_t> MM8108::read(uint32_t addr, uint32_t n) {
    std::vector<uint8_t> out(n);
    for (uint32_t off = 0; off < n;) {
        const uint32_t k = std::min(n - off, XFER);
        for (int attempt = 0;; attempt++) {                 // one retry after clearing both halts
            uint32_t cmd[3] = { 0x80, addr + off, k }; int x = 0;
            int r = libusb_bulk_transfer(h, EP_OUT, (uint8_t*)cmd, 12, &x, 250);
            if (!r) r = libusb_bulk_transfer(h, EP_IN, out.data() + off, k, &x, 250);
            if (!r && x == (int)k) break;
            if (attempt) throw MMError(MMSDR_ERR_USB, "USB read failed: " + std::string(libusb_error_name(r)));
            libusb_clear_halt(h, EP_OUT); libusb_clear_halt(h, EP_IN);
        }
        off += k;
    }
    return out;
}

void MM8108::write(uint32_t addr, const void* data, uint32_t n) {
    for (uint32_t off = 0; off < n; off += XFER) {
        const uint32_t k = std::min(n - off, XFER);
        uint32_t cmd[3] = { 0x00, addr + off, k }; int x = 0;
        int r = libusb_bulk_transfer(h, EP_OUT, (uint8_t*)cmd, 12, &x, 250);
        if (!r) r = libusb_bulk_transfer(h, EP_OUT, (uint8_t*)data + off, k, &x, 250);
        if (r) throw MMError(MMSDR_ERR_USB, "USB write failed: " + std::string(libusb_error_name(r)));
    }
}

uint32_t MM8108::sr32(uint32_t a) { return rd32(read(a, 4).data()); }
void MM8108::sw32(uint32_t a, uint32_t v) { write(a, &v, 4); }

// ============================================================================ identity
// uid = factory MAC (verified 2026-10-26): fw publishes it at ext host table +4; without fw, OTP bank 26 [31:16] + bank 27
void MM8108::readIdentity() {
    uint32_t first;
    try { first = sr32(CHIP_ID); }
    catch (const MMError&) { throw MMError(MMSDR_ERR_NEEDS_REPLUG, "no reads complete (chip wedged?): replug the module"); }
    dev.chipId = first;

    uint8_t mac[6] = {};
    const uint32_t ht = sr32(MANIFEST_PTR);
    if (inRam(ht) && sr32(ht) == 0xdeadbeef) {              // fw running: ext host table carries the MAC
        const uint32_t ext = sr32(ht + 0x18);
        if (inRam(ext)) { std::vector<uint8_t> et = read(ext, 12); memcpy(mac, &et[4], 6); }
    }
    else {
        digitalReset();
        readOtpIdentity(mac);
    }

    bool zero = true, ones = true;
    for (uint8_t b : mac) { zero &= b == 0; ones &= b == 0xff; }
    char s[16] = "";
    if (!zero && !ones) snprintf(s, sizeof s, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    dev.uid = s;
}

void MM8108::readOtpIdentity(uint8_t mac[6]) {
    auto bank = [&](uint32_t b) -> uint32_t {
        sw32(OTP_BANK, b); sw32(OTP_ECC, 1);                // 1 = ignore ECC, as the driver reads board type/country
        sw32(OTP_CTRL, (sr32(OTP_CTRL) & ~1u) | 1u);
        for (int i = 0; i < 5; i++) { sleepMs(1); if (!(sr32(OTP_CTRL) & 1)) return sr32(OTP_DATA); }
        throw MMError(MMSDR_ERR_TIMEOUT, "OTP bank " + std::to_string(b) + " read timeout");
    };
    sw32(0x4010, 1); sleepMs(1); sw32(0x4014, 1); sleepMs(1); sw32(0x4018, 1); sleepMs(1);   // power up
    sw32(0x401c, 0); sw32(0x4020, 1); sleepMs(1);                                             // read enable
    uint32_t b26 = 0, b27 = 0; std::string fail;
    try { b26 = bank(26); b27 = bank(27); } catch (const std::exception& e) { fail = e.what(); }
    sw32(0x4020, 0); sleepMs(1); sw32(0x4018, 0); sw32(0x4014, 0); sw32(0x4010, 0);           // read disable, power down
    if (!fail.empty()) throw MMError(MMSDR_ERR_TIMEOUT, fail);
    dev.board = (int)(b26 & 0xffff);
    mac[0] = (uint8_t)(b26 >> 16); mac[1] = (uint8_t)(b26 >> 24); memcpy(&mac[2], &b27, 4);
}

// ============================================================================ boot (boot.py)
void MM8108::aonLatch() {                                   // pulse AON_LATCH bit0 to apply the latched AON writes
    const uint32_t v = sr32(AON_LATCH);
    for (uint32_t x : { v & ~1u, v | 1u, v & ~1u }) { sw32(AON_LATCH, x); sleepMs(5); }
}

void MM8108::digitalReset() {
    // slow clock on, PMU ctrl off CPU, then MORSE_USB_RESET
    sw32(AON_LATCH, sr32(AON_LATCH) & ~0x40u); aonLatch(); sleepMs(2);
    sw32(AON_LATCH, sr32(AON_LATCH) & ~0x80u); aonLatch();
    uint32_t cmd[3] = { 0x02, 0, 0 }; int x;
    libusb_bulk_transfer(h, EP_OUT, (uint8_t*)cmd, 12, &x, 250);
    sleepMs(50);
}

void MM8108::boot() {
    std::vector<uint8_t> fw, bcf;
    try { fw = slurp(fwPath); bcf = slurp(bcfPath); Elf ef(fw), eb(bcf); }
    catch (const std::exception& e) { throw MMError(MMSDR_ERR_FIRMWARE, e.what()); }
    digitalReset();
    sw32(MANIFEST_PTR, 0);

    const Elf e(fw);
    stock = stockWords(e, fw);
    for (auto& p : e.ph) {                                  // load every PT_LOAD segment except flash (self-hosted only)
        if (p.pa == 0x400000 || p.pa == 0xC00000 || p.type != 1 || !p.msz || !p.fsz) continue;
        std::vector<uint8_t> seg(fw.begin() + p.off, fw.begin() + p.off + p.fsz);
        pad4(seg);
        write(p.pa, seg.data(), (uint32_t)seg.size());
    }
    if (e.entry) sw32(BOOT_ADDR, e.entry);

    const auto [baddr, bsize] = bcfArea(e, fw);
    const std::vector<uint8_t> blob = bcfBlob(bcf, regdom);
    if (!baddr || blob.size() > bsize) throw MMError(MMSDR_ERR_FIRMWARE, "BCF does not fit");
    write(baddr, blob.data(), (uint32_t)blob.size());

    sw32(AON_BASE, 0x8); sw32(AON_BASE + 4, 0);             // keep USB alive across the core reset
    aonLatch(); sw32(MSI, 1); sleepMs(60);
    for (double t0 = now(); now() - t0 < 0.55; sleepMs(5)) {
        try { if (sr32(MANIFEST_PTR)) return; } catch (...) {}
    }
    throw MMError(MMSDR_ERR_BOOT, "boot timeout (host table never set)");
}

void MM8108::bringUp() {
    if (int r = openUsb()) throw MMError(r, err);
    boot(); close();
    if (int r = openUsb()) throw MMError(r, "MM8108 lost after boot: " + err);
    yapsInit(); patchFirmware();
    fire(HOOK_FW_LOADED);
}

int MM8108::bootOnly() {
    stop();
    try { bringUp(); }
    catch (const MMError& e) { err = e.what(); ysl = 0; return e.code; }
    catch (const std::exception& e) { err = e.what(); ysl = 0; return MMSDR_ERR_USB; }
    err.clear();
    return 0;
}

// ============================================================================ YAPS (yaps.py)
void MM8108::yapsInit() {
    const uint32_t ht = sr32(MANIFEST_PTR);
    if (!ht || sr32(ht) != 0xdeadbeef) throw MMError(MMSDR_ERR_BOOT, "firmware not running");
    const uint32_t ext = sr32(ht + 0x18), n = (sr32(ext) + 3) & ~3u;
    const std::vector<uint8_t> et = read(ext, n);
    bool found = false;
    for (uint32_t i = 10; i + 4 <= et.size();) {            // TLV 3 = YAPS: RX pool, TX pool, status block, queue size, reserved
        const uint16_t tag = rd16(&et[i]), ln = rd16(&et[i + 2]);
        if (!ln) break;
        if (tag == 3) {
            const uint8_t* y = &et[i + 4];
            ysl = rd32(y + 4); yds = rd32(y + 8); sts = rd32(y + 12); cmdQSize = y[0x1b]; reserved = rd16(y + 0x20);
            found = true;
        }
        i += ln;
    }
    if (!found) throw MMError(MMSDR_ERR_BOOT, "no YAPS TLV");
    seq = (uint16_t)(GetTickCount64() % 0xfff);             // fw drops a repeated host_id
}

int MM8108::yapsSend(uint16_t id, const std::vector<uint8_t>& payload, double timeout, std::vector<uint8_t>* resp) {
    auto status = [&](uint32_t s[18]) {                     // status block; [17] lock, [15] CRC latch, [1] free pages, [9] queued, [14] RX bytes
        for (double t0 = now();;) {
            std::vector<uint8_t> v = read(sts, 72); memcpy(s, v.data(), 72);
            if (!s[17]) break;
            if (now() - t0 > 0.1) throw MMError(MMSDR_ERR_CMD, "YAPS status locked");
        }
        if (s[15]) throw MMError(MMSDR_ERR_CMD, "YAPS CRC failure latched");
    };
    // command header + payload, wrapped in an skb (0xAA 0xFE, len, 26 B header pad), padded to 4
    seq = seq % 0xfff + 1;
    const uint16_t hid = (uint16_t)(seq << 4);
    std::vector<uint8_t> cmd(12);
    const uint16_t ch[6] = { 1, id, (uint16_t)payload.size(), hid, 0, 0 };
    memcpy(cmd.data(), ch, 12); cmd.insert(cmd.end(), payload.begin(), payload.end());
    std::vector<uint8_t> skb(8 + 32 + 26, 0);
    skb[0] = 0xAA; skb[1] = 0xFE;
    const uint16_t cl = (uint16_t)cmd.size(); memcpy(&skb[2], &cl, 2); skb[4] = 26;
    skb.insert(skb.end(), cmd.begin(), cmd.end());
    while (skb.size() % 4) skb.push_back(0);

    // wait for TX pool pages and a queue slot
    const uint32_t need = (uint32_t)((skb.size() + reserved + 255) / 256 + 2);
    uint32_t s[18];
    const double t0 = now();
    for (;;) {
        status(s);
        if (s[1] >= need && cmdQSize - (int)s[9] > 0) break;
        if (now() - t0 > timeout) throw MMError(MMSDR_ERR_CMD, "no YAPS cmd space");
        sleepMs(2);
    }
    std::vector<uint8_t> msg(4);
    const uint32_t d = delim((uint32_t)skb.size() + reserved, 1, true);
    memcpy(msg.data(), &d, 4); msg.insert(msg.end(), skb.begin(), skb.end());
    write(yds, msg.data(), (uint32_t)msg.size());

    // poll the RX pool for our response
    while (now() - t0 < timeout) {
        status(s);
        if (s[14]) {
            const std::vector<uint8_t> buf = read(ysl, std::min<uint32_t>(s[14], 32768));
            int32_t st;
            if (yapsFindResponse(buf, reserved, id, hid, st, resp)) return st;
        }
        sleepMs(2);
    }
    throw MMError(MMSDR_ERR_CMD, "no YAPS response");
}

int MM8108::cmd(uint16_t id, const std::vector<uint8_t>& payload, double timeout, std::vector<uint8_t>* resp) {
    if (running || !ysl || !h) throw MMError(MMSDR_ERR_STATE, "raw commands need booted fw and no stream (use a boot hook)");
    return yapsSend(id, payload, timeout, resp);
}

// ============================================================================ firmware patches (radio.py / owner.py)
void MM8108::patchFirmware() {
    // NOP one 4 B instruction inside an 8 B read-modify-write window; accept already-patched
    auto nop = [&](const MMSite& s, const char* what) {
        std::vector<uint8_t> o = read(s.win, 8);
        if (!memcmp(&o[s.off], NOP, 4)) return;
        if (rd32(&o[s.off]) != stock.at(s.win + s.off)) throw MMError(MMSDR_ERR_FIRMWARE, std::string("unexpected ") + what + " encoding");
        memcpy(&o[s.off], NOP, 4);
        write(s.win, o.data(), 8);
    };
    nop(CLAMP_SITE, "clamp");
    for (const MMSite& s : DIV2_SITES) nop(s, "div2");
}

// ============================================================================ PLL hop (host replica of LPHY set_lo 0x542e60, mode 0 / chip 0x809 path)
void MM8108::setLo(double f) {
    auto rmw = [&](uint32_t off, uint32_t andm, uint32_t orm) { w32(RF + off, (r32(RF + off) & andm) | orm); };
    const uint64_t fi = (uint64_t)f, fx = (uint64_t)std::llround(f / xo());
    const uint32_t div = f > F_RESAMP_MAX ? 1 : 0;
    const uint32_t w = (uint32_t)(2 * ((fx * (1 << 24) + 16000000) / 32000000));   // x2: DIV2 patch forces the /2 path; fx: XO-corrected
    const bool kdcoOn = !knobs.nokdco;

    // clocks and band
    w32(RATIO, (uint32_t)(((fi << 22) >> div) / 1000000));
    rmw(CLKDIV - RF, ~(7u << 18), div << 18);               // ADC = VCO / (128 << div)
    rmw(0x68, ~0u, 0x180000); rmw(0x6c, ~0x70u, 0x30);
    if (f > 880e6) { rmw(0x54, 0xffff003f, 0x580); if (!kdcoOn && !r32(RF + 0x5c)) w32(RF + 0x5c, 0x7000); }   // fw keeps a nonzero 0x5c
    else { if (!kdcoOn) w32(RF + 0x5c, 0x19000); rmw(0x54, 0xffff003f, 0x3500); }
    if (kdcoOn) w32(RF + 0x5c, kdcoModel(f));               // acquire with the right DCO gain, not the last band's
    if (int b = knobs.band54; b >= 0 && (f > 1000e6 || f < FMIN)) rmw(0x54, 0xffff003f, ((uint32_t)b & 0x3ff) << 6);   // test: band [15:6]

    // frequency word and loop setup
    w32(SYNTH, w);
    rmw(0x48, ~0x10u, 0); rmw(0x68, 0x7fffffff, 0); rmw(0x64, ~0x7f0u, (w & 0xffffff) == 0 ? 0x20 : 0);
    rmw(0x58, 0xfff00000, 0x720000);
    if (int cc = knobs.cc; cc != CC_OFF && (f > 1000e6 || f < FMIN)) {   // test: signed coarse bank as uphy_pll_coarse_tune writes it
        rmw(0x64, ~0x10u, cc < 0 ? 0x10 : 0); rmw(0x58, 0xfffe03ff, (uint32_t)(std::abs(cc) & 0x7f) << 10);
    }
    rmw(0x50, 0xb0000003, 0x02001027);
    rmw(0x54, 0xe003fffd, 0x2200000); rmw(0x20, 0xffffe7ff, 0);

    // ADPLL reset + acquire; fw waits 1-20 us between steps, each RMW here is a USB round trip (>100 us)
    rmw(0x48, ~0u, 1); rmw(0x48, ~0u, 2);
    rmw(0x50, 0x7fffffff, 0); rmw(0x54, ~1u, 0); rmw(0x54, ~0xeu, 0); rmw(0x58, 0xff7fffff, 0);
    rmw(0x48, ~0u, 2); rmw(0x48, ~2u, 0);
    rmw(0x50, ~0u, 0x80000000); rmw(0x54, ~0u, 1);
    rmw(0x50, 0xfffe7fff, 0x8000); rmw(0x58, ~0u, 0x800000);
    rmw(0x54, ~0u, 2); rmw(0x54, ~0u, 4); rmw(0x54, ~0u, 8);
    rmw(0x50, ~0u, 1u << 26);                               // set later in SET_CHANNEL; sample clock stops without it
    sleepMs(1);
    if (kdcoOn) kdcoCal(f);
    synth = r32(SYNTH) * 32e6 / (1 << 24) / 2 * xo();
}

// 0x40205c vs LO, fit to host cal 770-1095 MHz (2026-10-25): K ~ 1/f^3 (LC-DCO gain ~ f^3), within ~2%.
uint32_t MM8108::kdcoModel(double f) {
    const double m = f / 1e6;
    return (uint32_t)std::clamp(2.0429e13 / (m * m * m), 0x1000 * 1.0, 0x1ffff * 1.0);
}

// Host replica of lphy_pll_kdco_cal 0x5431c8 (fw runs it on every channel change). Stale 0x40205c (e.g. the
// <=880 seed 0x19000) makes hops above ~2 GHz VCO lock at N-1 (LO 16 MHz low). Returns the new 0x40205c.
// fw also sets 0x401308 b0 / 0x40130c b3 here; while we stream those freeze T at 0x100, so they are skipped.
// T is the raw DCO code (identical across fw's 2x2 repeats), so one k = 1..10 sweep suffices (~20 USB ops).
uint32_t MM8108::kdcoCal(double f) {
    const uint32_t r50 = r32(RF + 0x50), model = kdcoModel(f);
    w32(RF + 0x50, (r50 & 0xcfffffff) | 0x20000000);        // 0x402068 = ADPLL tracking word (mid 0x100)
    const int t0 = (int)(r32(RF + 0x68) & 0x1ff);
    uint32_t acc = 0; int n = 0;
    if (std::abs(t0 - 0x100) <= 64) {                       // railed T = no lock: a cal would be garbage
        for (int k = 1; k <= 10; k++) {                     // FCW offset +-k MHz (0x100000 = 1 MHz)
            w32(RF + 0x60, (uint32_t)k << 20);  const int a = (int)(r32(RF + 0x68) & 0x1ff);
            w32(RF + 0x60, ~((uint32_t)k << 20)); const int b = (int)(r32(RF + 0x68) & 0x1ff);
            const int hzPerLsb = a != b ? 2000000 * k / (a - b) : 0;
            if (hzPerLsb > 0) { acc += ((uint32_t)(8000000 / hzPerLsb) << 8) & 0xffff; n++; }
        }
    }
    uint32_t kdco = n >= 5 ? acc / n : model;
    if (kdco < model / 2 || kdco > model * 2) kdco = model;  // implausible vs 1/f^3: trust the model
    w32(RF + 0x60, 0); w32(RF + 0x5c, kdco);
    w32(RF + 0x50, (r32(RF + 0x50) & 0xcfffffff) | (r50 & 0x30000000));
    return kdco;
}

void MM8108::applyRate() {
    const double adc = 2 * synth / 128, want = wideFs;
    if (want > FS && synth <= F_RESAMP_MAX) {
        const double r = std::clamp(adc / (2 * want), 0.5, 0.999);
        const uint32_t reg = (uint32_t)std::llround(r * 4294967296.0);
        w32(RATIO, reg);
        fsNow = adc / (2 * (reg / 4294967296.0));
    }
    else fsNow = FS;
}

// ============================================================================ retune (radio.retune)
int MM8108::retune(double f) {
    if (!knobs.unclamp) f = std::clamp(f, FMIN, FMAX);      // test only: VCO range mapping
    if (running && hopOn && packOn == packedNow && !knobs.nohop && hop(f)) return 0;
    lastHop = false;
    stop();
    try { bringUp(); tuneAndTakeRing(f); }
    catch (const std::exception& e) {
        const MMError* me = dynamic_cast<const MMError*>(&e);
        const int code = me ? me->code : MMSDR_ERR_USB;
        close(); ysl = 0;
        if (code == MMSDR_ERR_PARK && parkRetries++ < 2) { nPark++; return retune(f); }   // a fresh boot usually parks
        lastPark = parkRetries; parkRetries = 0; err = e.what();
        return code;
    }
    lastPark = parkRetries; parkRetries = 0; nReboots++;
    return startStream();
}

// register-only PLL hop while streaming; false = fall back to a reboot-retune
bool MM8108::hop(double f) {
    try { setLo(f); hopAt = writtenSamples(); applyRate(); applyDc(); }   // LO leakage moves with f: re-hold the DC servo
    catch (const std::exception&) { return false; }
    lastHop = true; lastPark = 0; nHops++; err.clear();
    return true;
}

// reboot path after bringUp(): SET_CHANNEL, medium eval, ring setter patch, take the I/Q ring, wait for UPHY to park
void MM8108::tuneAndTakeRing(double f) {
    // resampler ratio is fixed at 1024 MHz/VCO and bypasses below 0.5: above VCO 2048 halve the ADC clock instead
    if (f > F_RESAMP_MAX) sw32(LPHY_DIV_BW8, 1);
    // SET_CHANNEL rejects a few exact values (930.000 MHz: -32768): tune next door, then hop
    double fc = f;
    int st = setChannel(fc);
    if (st) { fc = f + 1e6 > FMAX ? f - 1e6 : f + 1e6; st = setChannel(fc); }
    if (st) throw MMError(MMSDR_ERR_CMD, "SET_CHANNEL status " + std::to_string(st));
    sleepMs(20);
    if (fc != f) setLo(f);
    else if (f > F_RESAMP_MAX) sw32(RATIO, (uint32_t)((((uint64_t)f << 22) >> 1) / 1000000));   // fw value wrapped
    synth = sr32(SYNTH) * 32e6 / (1 << 24) / 2 * xo();     // /2 path is forced
    fire(HOOK_PRE_MEDIUM_EVAL);                             // GET_SET_GENERIC_PARAM only works before this
    yapsSend(0x811C, { 1 }, 0.6);                           // MEDIUM_EVAL: no packet detect/AGC stalls
    fire(HOOK_TUNED);                                       // UPHY still alive (host cal cmds such as 0x811D)

    packedNow = packOn;
    if (packedNow) installPacker();
    // UPHY's ASSERT rings all doorbells with crash sem 0x1d00 held, which halts LPHY: NOP that jal so LPHY survives
    if (!knobs.crashbcast && sr32(ASSERT_BCAST) == stock.at(ASSERT_BCAST)) sw32(ASSERT_BCAST, 0x00000013);
    applyRingPatch();
    takeRing();
    waitParked();

    // XO correction: fw tuned the nominal word, and UPHY CFO-retunes until parked (a pre-park setLo is undone)
    if (ppb) setLo(f);
    applyRate();                                            // after MEDIUM_EVAL/ring setup: earlier ratio writes get overwritten
    for (int i = 0; i < 5; i++) {                           // belt and braces: reapply if anything still rewrites it
        const uint32_t r = sr32(RATIO); sleepMs(20);
        if (sr32(RATIO) == r) break;
        applyRate();
    }
    fire(HOOK_RING_TAKEN);
}

int MM8108::setChannel(double fc) {
    std::vector<uint8_t> ch(10, 0);
    const uint32_t fi = (uint32_t)fc; memcpy(ch.data(), &fi, 4);
    ch[4] = 8; ch[5] = 2; ch[8] = 1;                        // op_bw 8, pri_bw 2, idx 0, ..., 1, 0
    return yapsSend(0x0001, ch, 3.0);
}

// packed mode: hook the ASSERT that taking the ring trips, so UPHY runs the packer instead of spinning on the dead UART
void MM8108::installPacker() {
    write(PK_CAVE, PACKER_BLOB, sizeof PACKER_BLOB);
    if (read(PK_CAVE, sizeof PACKER_BLOB) != std::vector<uint8_t>(PACKER_BLOB, PACKER_BLOB + sizeof PACKER_BLOB))
        throw MMError(MMSDR_ERR_FIRMWARE, "packer cave readback mismatch");
    sw32(PK_CTRL, 0); sw32(PK_CTRL + 4, 0);
    std::vector<uint8_t> w = read(ASSERT_SITE - 2, 8);
    if (rd32(&w[2]) != stock.at(ASSERT_SITE)) throw MMError(MMSDR_ERR_FIRMWARE, "unexpected ASSERT entry encoding");
    const uint32_t j = jal0((int32_t)(PK_CAVE - ASSERT_SITE));
    memcpy(&w[2], &j, 4);
    write(ASSERT_SITE - 2, w.data(), 8);
}

// ring patch without the setter2 safety NOPs: the fw ASSERT then parks UPHY in puts(), so nothing else stalls the source
void MM8108::applyRingPatch() {
    for (const MMPatch& p : RING_PATCHES) {
        if (p.safety) continue;
        const uint32_t old = stock.at(p.addr), nw = patchedWord(p, old);
        uint32_t live = 0;
        for (int i = 0; i < 50; i++) {                      // LPHY IMEM words being fetched can read back junk
            live = sr32(p.addr);
            if (live == old || live == nw) break;
            sleepMs(10);
        }
        if (live == old) sw32(p.addr, nw);
    }
}

// point the ring DMA at our buffer (sync bridge; recalDc does the same live through w32)
void MM8108::takeRing() {
    if (!packedNow) sw32(PANIC_MAGIC, 0);                   // AON survives reboot: clear so the park check is fresh
    const uint32_t c = sr32(CTRL);
    sw32(CTRL, c & ~1u);
    sw32(BASE, packedNow ? PK_IN_BASE : SAFE_BASE); sw32(LEN, packedNow ? PK_IN_LEN : SAFE_LEN);
    sw32(CTRL, c | 1u);
}

// UPHY may still rewrite the ratio (and CFO-retune the LO) until its ring-take ASSERT parks it, or until it reaches the packer
void MM8108::waitParked() {
    const double t0 = now();
    if (!packedNow) {
        while (sr32(PANIC_MAGIC) != 0xfee1dead) {
            if (now() - t0 > 0.5) throw MMError(MMSDR_ERR_PARK, PARK_ERR);
            sleepMs(2);
        }
    }
    else {
        const uint32_t hb0 = sr32(PK_CTRL + 4);             // packer heartbeat
        while (sr32(PK_CTRL + 4) == hb0) {
            if (now() - t0 > 0.5) throw MMError(MMSDR_ERR_PARK, "packer did not start");
            sleepMs(2);
        }
    }
}

int MM8108::startStream() {
    stopReq = false; running = true; wpos = rpos = 0; hopAt = 0;
    { std::lock_guard<std::mutex> l(gapMtx); gaps.clear(); }
    try {
        reader = std::thread(&MM8108::readerLoop, this);
        output = std::thread(&MM8108::outputLoop, this);
        setGain(gain);                                      // also applies the DC mode
    }
    catch (const std::exception& e) {                       // never report failure with the threads left running
        const MMError* me = dynamic_cast<const MMError*>(&e);
        stop(); close(); ysl = 0; err = std::string("stream start: ") + e.what();
        return me ? me->code : MMSDR_ERR_USB;
    }
    err.clear();
    return 0;
}

void MM8108::stop() {
    if (!running) return;
    stopReq = true;
    if (reader.joinable()) reader.join();
    running = false; outCv.notify_all();
    if (output.joinable()) output.join();
}

uint64_t MM8108::takeGap(uint64_t wordPos) {
    std::lock_guard<std::mutex> l(gapMtx);
    uint64_t n = 0;
    while (!gaps.empty() && gaps.front().first <= wordPos) { n += gaps.front().second; gaps.pop_front(); }
    return n;
}

uint64_t MM8108::nextGap() {
    std::lock_guard<std::mutex> l(gapMtx);
    return gaps.empty() ? UINT64_MAX : gaps.front().first;
}

// ============================================================================ live register access, gain, DC
uint32_t MM8108::r32(uint32_t addr) {
    if (!running) return sr32(addr);
    auto p = std::make_shared<std::promise<uint32_t>>(); auto f = p->get_future();
    { std::lock_guard<std::mutex> l(ctlMtx); ctlQ.push_back({ false, addr, 0, p }); }
    if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready) throw MMError(MMSDR_ERR_TIMEOUT, "register read timeout");
    return f.get();
}

void MM8108::w32(uint32_t addr, uint32_t v) {
    if (!running) { sw32(addr, v); return; }
    auto p = std::make_shared<std::promise<uint32_t>>(); auto f = p->get_future();
    { std::lock_guard<std::mutex> l(ctlMtx); ctlQ.push_back({ true, addr, v, p }); }
    if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready) throw MMError(MMSDR_ERR_TIMEOUT, "register write timeout");
    f.get();                                                // rethrows a failed OUT transfer as MMSDR_ERR_USB
}

std::vector<uint8_t> MM8108::peek(uint32_t addr, uint32_t n) {
    if (!running) {
        if (!h) throw MMError(MMSDR_ERR_STATE, "device not open");
        return read(addr, n);
    }
    std::vector<uint8_t> out((n + 3) & ~3u);
    for (uint32_t o = 0; o < n; o += 4) { const uint32_t v = r32(addr + o); memcpy(&out[o], &v, 4); }
    out.resize(n);
    return out;
}

void MM8108::setGain(int g) {
    gain = std::clamp(g, 0, GAIN_MAX);
    if (!running) return;
    const uint32_t r = GAIN_MAX - gain, old = r32(GAIN_REG);
    w32(GAIN_REG, (old & ~GAIN_FIELDS) | r << 2 | r << 8 | r << 14 | r << 20);   // live field depends on AGC mode; [25:20] RO
    applyDc();                                              // gain stage is ahead of the servo: offset moves with gain
}

void MM8108::setDcMode(DcMode m) { dcMode = m; if (running) applyDc(); }

void MM8108::applyDc() {
    const uint32_t v = r32(DC_CTRL) & ~0x11u;
    if (dcMode == DC_OFF) { w32(DC_CTRL, v); return; }
    w32(DC_CTRL, v | 1); sleepMs(20);                       // converge with the fw servo, then freeze it
    if (dcMode == DC_HOLD) w32(DC_CTRL, v | 0x11);
    dcEp++;
}

std::string MM8108::recalDc() {
    // LPHY inbox (ch3): lock 0x1d3c, slot 0x54ff80 {type, len, payload}, doorbell 0x406004 b0 (MSIP).
    // UPHY is parked in the ring ASSERT, so nothing else contends for the lock.
    constexpr uint32_t MB_LOCK = 0x1d3c, MB_SLOT = 0x54ff80, MB_DOOR = 0x406004, MB_EVT = 0x510000;
    constexpr uint32_t CAL_BUF = 0x5141b0, CAL_FLAG = 0x5104cc, CAL_GIDX = 0x538524, CAL_OUT = 0x538ef4;
    if (!running) return "not streaming";
    if (r32(MB_LOCK)) return "LPHY inbox busy";
    if (r32(0x1d00) & 1) return "crash sem 0x1d00 held: LPHY is halted (or would halt on the doorbell)";

    // LPHY rxcap must own the ring during cal or it ASSERTs (0x545618): restore its stock setter words
    std::vector<const MMPatch*> lp;
    for (const MMPatch& p : RING_PATCHES) if (p.addr >= 0x540000 && !p.safety) lp.push_back(&p);
    for (const MMPatch* p : lp) w32(p->addr, stock.at(p->addr));

    // post 0x3017 and wait for LPHY to write !success into CAL_FLAG
    const uint32_t fl = r32(CAL_FLAG); w32(CAL_FLAG, (fl & ~0xffu) | 0xff);
    const uint32_t g = (r32(CAL_GIDX) >> 16) & 0xff;       // UPHY's current LPHY gain idx (byte 0x538526)
    const uint32_t msg[15] = { 0x3017, 0x34, CAL_BUF, 8, 0, 0, 0, 0,
                               CAL_OUT, CAL_OUT + 0x2a, CAL_OUT + 0x54, CAL_OUT + 0x69, CAL_OUT + 0x7e, g, CAL_FLAG };
    w32(MB_LOCK, 1);
    for (int i = 0; i < 15; i++) w32(MB_SLOT + 4 * i, msg[i]);
    w32(MB_DOOR, r32(MB_DOOR) | 1);
    std::string err;
    for (double t0 = now();;) {
        const uint32_t f = r32(CAL_FLAG) & 0xff;
        if (f != 0xff) { if (f) err = "LPHY DC cal reported failure (strong in-band signal?)"; break; }
        if (now() - t0 > 1.0) {
            char b[240];                                    // lock still 1 = not consumed; door b0 still set = MSIP not taken
            snprintf(b, sizeof b, "LPHY DC cal timeout: lock %x door %x evt %x | printlock 1d2c=%x crashlock 1d00=%x | uphy dump 5380d4=%x line %x file %x",
                     r32(MB_LOCK), r32(MB_DOOR), r32(MB_EVT), r32(0x1d2c), r32(0x1d00), r32(0x5380d4), r32(0x538164), r32(0x538168));
            err = b; break;
        }
        sleepMs(2);
    }
    w32(MB_EVT, r32(MB_EVT) & ~0x20u);                      // done flag meant for UPHY

    // re-patch the setters and take the ring back (cal left it at CAL_BUF), then re-hold the servo the cal re-enabled
    for (const MMPatch* p : lp) w32(p->addr, patchedWord(*p, stock.at(p->addr)));
    const uint32_t c = r32(CTRL);
    w32(CTRL, c & ~1u);
    w32(BASE, packedNow ? PK_IN_BASE : SAFE_BASE); w32(LEN, packedNow ? PK_IN_LEN : SAFE_LEN);
    w32(CTRL, c | 1u);
    applyDc();
    return err;
}

// ============================================================================ USB transfer plumbing
void LIBUSB_CALL MM8108::cbIn(libusb_transfer* t) {
    Slot* s = (Slot*)t->user_data;
    s->status = t->status; s->len = t->actual_length; s->done = true;
}

void LIBUSB_CALL MM8108::cbOut(libusb_transfer* t) {
    OutX* x = (OutX*)t->user_data; MM8108* self = x->self;
    if (t->status != LIBUSB_TRANSFER_COMPLETED) {
        self->nUsbErr++;
        if (x->p) x->p->set_exception(std::make_exception_ptr(MMError(MMSDR_ERR_USB, "register write failed")));
    }
    else if (x->p) x->p->set_value(0);                      // a ctl write succeeds only once its OUT transfer completed
    x->p.reset();
    self->freeOut.push_back(t); self->outInFlight--;
}

void MM8108::submitOut(const void* data, int n, CtlP done) {
    libusb_transfer* t;
    if (freeOut.empty()) { t = libusb_alloc_transfer(0); t->buffer = new uint8_t[16]; t->user_data = new OutX{ this, nullptr }; }
    else { t = freeOut.back(); freeOut.pop_back(); }
    memcpy(t->buffer, data, n);
    ((OutX*)t->user_data)->p = done;
    libusb_fill_bulk_transfer(t, h, EP_OUT, t->buffer, n, cbOut, t->user_data, 500);
    if (libusb_submit_transfer(t)) {
        nUsbErr++; ((OutX*)t->user_data)->p.reset(); freeOut.push_back(t);
        if (done) done->set_exception(std::make_exception_ptr(MMError(MMSDR_ERR_USB, "register write submit failed")));
    }
    else outInFlight++;
}

// queue an IN transfer, then the bridge read command that fills it; completions are consumed in fifo order
void MM8108::submitRead(uint32_t addr, uint32_t n, int kind, int64_t tag, CtlP ctl) {
    for (size_t i = 0; i < zombies.size();) {               // reap cancelled transfers that have since completed
        if (zombies[i]->done) { freeSlots.push_back(zombies[i]); zombies.erase(zombies.begin() + i); }
        else i++;
    }
    Slot* s;
    if (freeSlots.empty()) { s = new Slot(); s->self = this; s->t = libusb_alloc_transfer(0); }
    else { s = freeSlots.back(); freeSlots.pop_back(); }
    s->kind = kind; s->tag = tag; s->n = n; s->done = false; s->ctl = ctl; s->status = -1; s->len = 0;
    libusb_fill_bulk_transfer(s->t, h, EP_IN, s->buf, n, cbIn, s, 2000);
    if (libusb_submit_transfer(s->t)) { s->status = LIBUSB_TRANSFER_ERROR; s->done = true; }
    else { uint32_t cmd[3] = { 0x80, addr, n }; submitOut(cmd, 12); }
    fifo.push_back(s);
}

// ============================================================================ reader thread (ringstream.py)
void MM8108::readerLoop() {
    RingRun run;
    run.ch = std::clamp((int)knobs.ch, 256, 16384); run.k = std::max((int)knobs.k, 1);
    run.depth = std::max((int)knobs.depth, 1); run.waitUs = knobs.waitUs;
    run.base = packedNow ? PK_OUT_BASE : SAFE_BASE; run.ptrReg = packedNow ? PK_CTRL : PTR;   // packed: the packer's ring and pointer
    run.len = packedNow ? PK_OUT_WORDS : SAFE_LEN; run.spw = packedNow ? 2 : 1;
    SetThreadPriority(GetCurrentThread(), knobs.prio); timeBeginPeriod(1);
    try { const uint32_t v = sr32(run.ptrReg); run.p0 = run.valid(v) ? (int64_t)(v - run.base) / 4 : 0; } catch (...) { nUsbErr++; }
    run.pMod = run.p0; run.tFix = now();
    submitRead(run.ptrReg, 4, K_PTR, 0, nullptr);

    double tStop = 0;
    for (;;) {
        const bool stopping = stopReq;
        if (stopping && !tStop) tStop = now();
        if (stopping && (fifo.empty() || now() - tStop > 2.5)) break;
        if (!stopping) { scheduleReads(run); drainCtlQueue(); }
        timeval tv = { 0, fifo.empty() || fifo.front()->done ? 0 : run.waitUs };
        libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        while (!fifo.empty() && fifo.front()->done) {
            Slot* s = fifo.front(); fifo.pop_front();
            completeSlot(run, s);
            freeSlots.push_back(s);
        }
    }
    finishReader();
    timeEndPeriod(1);
}

// keep `depth` reads in flight, none reaching past where the fw pointer should be by now (minus MARGIN)
void MM8108::scheduleReads(RingRun& r) {
    const double fs = fsNow * (SCHED_FS / FS) / xo() / r.spw;   // ring words/s in host time; follows hops (wide mode) and ppb
    const int64_t lim = (int64_t)(r.pAbs + fs * (now() - r.tFix) - MARGIN * fs);
    while ((int)fifo.size() < r.depth && lim - r.next >= 256) {
        const int64_t at = (r.next + r.p0) % r.len;
        const int64_t n = std::min<int64_t>({ r.ch, lim - r.next, r.len - at });
        submitRead(r.base + 4 * (uint32_t)at, 4 * (uint32_t)n, K_DATA, r.next, nullptr);
        r.next += n;
        if (++r.sinceP >= r.k) { submitRead(r.ptrReg, 4, K_PTR, 0, nullptr); r.sinceP = 0; }
    }
}

void MM8108::drainCtlQueue() {
    std::deque<Ctl> q;
    { std::lock_guard<std::mutex> l(ctlMtx); q.swap(ctlQ); }
    for (Ctl& c : q) {
        if (c.write) { uint32_t cmd[3] = { 0x00, c.addr, 4 }; submitOut(cmd, 12); submitOut(&c.val, 4, c.p); }
        else submitRead(c.addr, 4, K_CTL, 0, c.p);
    }
}

void MM8108::completeSlot(RingRun& r, Slot* s) {
    const bool ok = s->status == LIBUSB_TRANSFER_COMPLETED && s->len == (int)s->n;
    if (s->kind == K_CTL) {
        if (ok) s->ctl->set_value(rd32(s->buf));
        else s->ctl->set_exception(std::make_exception_ptr(MMError(MMSDR_ERR_USB, "register read failed")));
        return;
    }
    if (!ok) { nUsbErr++; return; }
    if (s->kind == K_PTR) { onPointer(r, rd32(s->buf)); return; }
    r.pending.push_back(s->tag);
    pushWords(s->buf, s->n / 4);
    nSamples += (uint64_t)(s->n / 4) * r.spw;
}

// fw pointer fix: unwrap it, back-date the fix, and count reads the pointer has lapped since they were issued
void MM8108::onPointer(RingRun& r, uint32_t v) {
    if (!r.valid(v)) { nBadPtr++; return; }                 // glitched ptr reads happen; skip
    const int64_t pm = (v - r.base) / 4;
    r.pAbs += ((pm - r.pMod) % r.len + r.len) % r.len; r.pMod = pm; r.tFix = now() - BACKDATE;
    for (int64_t st : r.pending) if (r.pAbs - st >= r.len) nLapped++;
    r.pending.clear();
}

// reader -> output SPSC; when full drop the newest words and record the gap at word w for the output side
void MM8108::pushWords(const uint8_t* buf, uint32_t n) {
    const uint64_t w = wpos.load(std::memory_order_relaxed);
    if (w + n - rpos.load(std::memory_order_acquire) > SPSC) {
        nOverflow += n;
        std::lock_guard<std::mutex> l(gapMtx);
        if (!gaps.empty() && gaps.back().first == w) gaps.back().second += n;
        else gaps.push_back({ w, n });
        return;
    }
    for (uint32_t i = 0; i < n; i++) spsc[(w + i) & (SPSC - 1)] = rd32(buf + 4 * i);
    wpos.store(w + n, std::memory_order_release);
    outCv.notify_one();
}

// abandon anything still in flight (only after a stall): cancel, reap, then answer or park every slot before the handle goes away
void MM8108::finishReader() {
    for (Slot* s : fifo) if (!s->done) libusb_cancel_transfer(s->t);
    for (double t0 = now(); now() - t0 < 1.0;) {
        bool all = outInFlight == 0;
        for (Slot* s : fifo) all &= s->done;
        if (all) break;
        timeval tv = { 0, 1000 }; libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
    }
    for (Slot* s : fifo) {
        if (s->kind == K_CTL) {                             // a done slot stuck behind an undone one still owes its answer
            if (s->done && s->status == LIBUSB_TRANSFER_COMPLETED && s->len == (int)s->n) s->ctl->set_value(rd32(s->buf));
            else if (s->done) s->ctl->set_exception(std::make_exception_ptr(MMError(MMSDR_ERR_USB, "register read failed")));
            else s->ctl->set_exception(std::make_exception_ptr(MMError(MMSDR_ERR_STATE, "reader stopped")));
        }
        s->ctl.reset();
        (s->done ? freeSlots : zombies).push_back(s);       // an un-reaped transfer must not be refilled next run
    }
    fifo.clear();
    std::lock_guard<std::mutex> l(ctlMtx);
    for (Ctl& c : ctlQ) c.p->set_exception(std::make_exception_ptr(MMError(MMSDR_ERR_STATE, "reader stopped")));
    ctlQ.clear();
}

// ============================================================================ output thread
void MM8108::outputLoop() {
    // hand off 16k-sample blocks; a slow consumer only overflows the SPSC
    std::vector<int16_t> iq(2 * 16384);
    const int spw = packedNow ? 2 : 1, blockWords = 16384 / spw;
    while (running) {
        const uint64_t r = rpos.load(std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> l(outMtx);
            outCv.wait_for(l, std::chrono::milliseconds(20), [&] { return !running || wpos.load(std::memory_order_acquire) - r >= 16384; });
        }
        const uint64_t w = wpos.load(std::memory_order_acquire);
        // never hand out a block that straddles an SPSC drop: cut at the gap so onIQ's pos marks it exactly
        const uint64_t g = nextGap();
        const bool cut = g > r && g < w;
        const uint64_t avail = cut ? g - r : w - r;
        const int nw = (int)std::min<uint64_t>(avail, blockWords);
        if (nw < blockWords && running && !cut) continue;   // a short block only at a gap or at stop
        nClipped += packedNow ? unpackPacked(r, nw, iq.data()) : unpackNative(r, nw, iq.data());
        rpos.store(r + nw, std::memory_order_release);
        if (nw) onIQ(iq.data(), nw * spw, r * spw);
    }
}

// packer word = bytes I0 I1 Q0 Q1 (top 8 bits of each sample): back to int16 scale, Q negated; clip = 8-bit rails
uint64_t MM8108::unpackPacked(uint64_t r, int nw, int16_t* iq) {
    uint64_t clip = 0;
    for (int i = 0; i < nw; i++) {
        const uint32_t v = spsc[(r + i) & (SPSC - 1)];
        const int i0 = (int8_t)(v & 0xff), i1 = (int8_t)((v >> 8) & 0xff), q0 = (int8_t)((v >> 16) & 0xff), q1 = (int8_t)(v >> 24);
        iq[4 * i] = (int16_t)(i0 * 256); iq[4 * i + 1] = (int16_t)std::min(-q0 * 256, 32767);
        iq[4 * i + 2] = (int16_t)(i1 * 256); iq[4 * i + 3] = (int16_t)std::min(-q1 * 256, 32767);
        clip += (std::max(std::abs(i0), std::abs(q0)) >= 127) + (std::max(std::abs(i1), std::abs(q1)) >= 127);
    }
    return clip;
}

// native word = int16 I | Q << 16; the ring is spectrally inverted, so Q is negated; clip = int16 rails
uint64_t MM8108::unpackNative(uint64_t r, int n, int16_t* iq) {
    uint64_t clip = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t v = spsc[(r + i) & (SPSC - 1)];
        const int a = (int16_t)(v & 0xffff), b = (int16_t)(v >> 16);
        iq[2 * i] = (int16_t)a; iq[2 * i + 1] = (int16_t)-b;
        clip += std::max(std::abs(a), std::abs(b)) >= 32767;
    }
    return clip;
}
