// rtl_tcp server for the MM8108 (for gr-osmosdr "rtl_tcp=host:port" clients such as OP25).
// mm_rtltcp <fw> <bcf> [--port 1234] [--freq 855.35e6] [--shift 6] [--gain 16]
// 8 MS/s native; power-of-two rates below that are made by a halfband cascade. Samples go out as 8-bit offset I/Q.
#include "mmsdr.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

// ---------------------------------------------------------------- decimation
static std::vector<float> halfband(int n = 47) {
    std::vector<float> h(n); double s = 0;
    for (int i = 0; i < n; i++) {
        const double k = i - (n - 1) / 2.0, sinc = k == 0 ? 1.0 : sin(M_PI * 0.5 * k) / (M_PI * 0.5 * k);
        const double bw = 0.42 - 0.5 * cos(2 * M_PI * i / (n - 1)) + 0.08 * cos(4 * M_PI * i / (n - 1));   // Blackman
        h[i] = (float)(0.5 * sinc * bw); s += h[i];
    }
    for (float& v : h) v = (float)(v / s);
    return h;
}

struct Decim2 {                                           // stateful /2 FIR on interleaved float I/Q
    std::vector<float> h = halfband(), hist;
    bool phase = false;
    void run(std::vector<float>& x) {
        const size_t taps = h.size();
        std::vector<float> buf(hist); buf.insert(buf.end(), x.begin(), x.end());
        const size_t n = buf.size() / 2;
        std::vector<float> y;
        for (size_t i = taps - 1; i < n; i++) {
            if ((phase = !phase)) continue;
            float re = 0, im = 0;
            for (size_t k = 0; k < taps; k++) { re += h[k] * buf[2 * (i - k)]; im += h[k] * buf[2 * (i - k) + 1]; }
            y.push_back(re); y.push_back(im);
        }
        hist.assign(buf.end() - 2 * (taps - 1), buf.end());
        x.swap(y);
    }
};

// ---------------------------------------------------------------- state
static constexpr int GAIN_MAX = 16;
static mmsdr_dev* dev;
static std::atomic<SOCKET> client{ INVALID_SOCKET };
static std::atomic<int> decim{ 0 }, conn{ 0 };           // conn: bumps per client so onIQ resets its own filter state
static int shiftBits = 6;
static std::vector<Decim2> stages;                        // output thread only
static std::atomic<uint64_t> n8{ 0 }, clip8{ 0 };         // 8-bit output level / clamp meter
static std::atomic<double> pow8{ 0 };

static mmsdr_stats stats() { mmsdr_stats s{}; s.struct_size = sizeof s; mmsdr_get_stats(dev, &s); return s; }

// ---------------------------------------------------------------- output thread: decimate, 8-bit offset, send
static void onIQ(void*, const mmsdr_rx_block* b) {
    SOCKET c = client;
    if (c == INVALID_SOCKET) return;
    // DC residual: libmmsdr's hold-mode tracker (dc_mode hold, default) has already removed it
    const int16_t* iq = (const int16_t*)b->samples; const int n = (int)b->count;
    std::vector<float> x(2 * n);
    for (int i = 0; i < 2 * n; i++) x[i] = (float)iq[i];
    static int seen = 0;
    const int d = decim, g = conn;
    if ((int)stages.size() != d || g != seen) { stages.assign(d, Decim2()); seen = g; }
    for (auto& s : stages) s.run(x);

    const float sc = 1.0f / (float)(1 << shiftBits);
    std::vector<uint8_t> out(x.size());
    uint64_t cl = 0; double p = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const float v = x[i] * sc; const int q = (int)lrintf(v + 127.5f);
        cl += q < 0 || q > 255; p += v * v;
        out[i] = (uint8_t)std::clamp(q, 0, 255);
    }
    n8 += x.size() / 2; clip8 += cl; pow8 = pow8 + p;
    for (size_t off = 0; off < out.size();) {
        const int r = send(c, (const char*)out.data() + off, (int)(out.size() - off), 0);
        if (r <= 0) { client = INVALID_SOCKET; return; }
        off += r;
    }
}

// ---------------------------------------------------------------- rtl_tcp commands (5 B: op, u32 BE)
static void handleCommand(uint8_t op, uint32_t p) {
    switch (op) {
    case 0x01: {                                          // center frequency: PLL hop (~60 ms) while streaming
        const double f = std::clamp((double)p, 750e6, 1100e6);
        if (fabs(f - mmsdr_get_freq(dev)) <= 1) break;
        mmsdr_apply_result r{}; r.struct_size = sizeof r;
        const int e = mmsdr_set_freq(dev, f, &r);
        printf("tune %.4f MHz: %s, fs %.4f MS/s\n", f / 1e6, e ? mmsdr_last_error_detail(dev) : r.action == MMSDR_APPLY_HOP ? "hop" : "reboot",
               mmsdr_get_sample_rate(dev) / 1e6);
        if (mmsdr_get_sample_rate(dev) != 8e6) printf("  WARNING: fs is not the 8 MS/s the client assumes\n");
        break;
    }
    case 0x02: {                                          // sample rate: 8 MS/s / 2^k
        int k = 0;
        while (k < 5 && 8e6 / (1 << (k + 1)) >= p * 0.999) k++;
        decim = k;
        printf("rate %u requested -> %.0f S/s\n", p, 8e6 / (1 << k));
        if (fabs(8e6 / (1 << k) - p) > 1) printf("  WARNING: client assumes %u S/s; only 8 MS/s / 2^k are exact\n", p);
        break;
    }
    case 0x04: {                                          // gain in tenths of dB
        const int g = mmsdr_gain_idx_for_db(dev, (int32_t)p / 10.0);
        mmsdr_set_gain(dev, g);
        printf("gain %.1f dB -> %d/16\n", (int32_t)p / 10.0, g);
        break;
    }
    case 0x05: {                                          // ppm: XO correction moves LO and fs together (hop)
        const int e = mmsdr_set_freq_correction_ppb(dev, (int32_t)p * 1000, nullptr);
        printf("freq correction %d ppm: %s\n", (int32_t)p, e ? mmsdr_last_error_detail(dev) : "ok");
        break;
    }
    case 0x0d:                                            // gain index
        mmsdr_set_gain(dev, std::clamp((int)p, 0, GAIN_MAX));
        printf("gain index %u\n", p);
        break;
    default: break;                                       // gain mode, AGC, direct sampling...: not applicable
    }
    fflush(stdout);
}

// per-10 s line: reader counters since last time plus the 8-bit output meter
static void printStats(mmsdr_stats& last, ULONGLONG& tl) {
    const mmsdr_stats s = stats();
    const double dt = (GetTickCount64() - tl) / 1e3;
    const uint64_t n = n8.exchange(0), c = clip8.exchange(0);
    const double p = pow8.exchange(0);
    printf("  %.4f MS/s  laps %llu  bad_ptr %llu  usb_err %llu  overflow %llu  8-bit rms %.1f LSB  clamped %.4f%%\n",
           (s.samples - last.samples) / dt / 1e6, s.lapped - last.lapped, s.bad_ptr - last.bad_ptr, s.usb_err - last.usb_err,
           s.overflow - last.overflow, n ? sqrt(p / n) : 0.0, n ? 100.0 * c / (2.0 * n) : 0.0);
    fflush(stdout);
    last = s; tl = GetTickCount64();
}

// one client at a time: read 5 B commands until it hangs up
static void serveClient(SOCKET c, mmsdr_stats& last, ULONGLONG& tl) {
    int nd = 1, sb = 8 << 20;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd);
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char*)&sb, sizeof sb);
    DWORD to = 1000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    // dongle info: magic, tuner type (R820T), gain count
    uint8_t hdr[12] = { 'R', 'T', 'L', '0' }; uint32_t t = htonl(5), g = htonl(GAIN_MAX + 1);
    memcpy(hdr + 4, &t, 4); memcpy(hdr + 8, &g, 4);
    send(c, (const char*)hdr, 12, 0);
    printf("client connected\n"); fflush(stdout);
    conn++; client = c;

    uint8_t cmd[5]; int have = 0;
    while (client != INVALID_SOCKET) {
        const int r = recv(c, (char*)cmd + have, 5 - have, 0);
        if (r == 0 || (r < 0 && WSAGetLastError() != WSAETIMEDOUT)) break;
        if (r > 0 && (have += r) == 5) {
            have = 0;
            uint32_t p; memcpy(&p, cmd + 1, 4);
            handleCommand(cmd[0], ntohl(p));
        }
        if (GetTickCount64() - tl >= 10000) printStats(last, tl);
    }
    client = INVALID_SOCKET; closesocket(c);
    printf("client gone\n"); fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: mm_rtltcp <fw> <bcf> [--port 1234] [--freq 855.35e6] [--shift 6] [--gain 16]\n"); return 2; }
    int port = 1234, gain = GAIN_MAX; double freq = 855.35e6;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--port") port = atoi(argv[i + 1]);
        else if (k == "--freq") freq = atof(argv[i + 1]);
        else if (k == "--shift") shiftBits = atoi(argv[i + 1]);
        else if (k == "--gain") gain = atoi(argv[i + 1]);
    }

    mmsdr_open_args oa{}; oa.struct_size = sizeof oa; oa.firmware_path = argv[1]; oa.bcf_path = argv[2];
    if (int e = mmsdr_open(&dev, &oa)) { fprintf(stderr, "open failed: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(nullptr)); return 1; }
    mmsdr_set_gain(dev, std::clamp(gain, 0, GAIN_MAX));
    mmsdr_set_freq(dev, std::clamp(freq, 750e6, 1100e6), nullptr);
    mmsdr_rx_args ra{}; ra.struct_size = sizeof ra; ra.callback = onIQ;
    if (int e = mmsdr_rx_start(dev, &ra)) { fprintf(stderr, "retune failed: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(dev)); return 1; }
    printf("tuned %.4f MHz, %.4f MS/s, gain %d/16, 8-bit shift %d; listening on :%d\n",
           mmsdr_get_freq(dev) / 1e6, mmsdr_get_sample_rate(dev) / 1e6, gain, shiftBits, port);
    fflush(stdout);

    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET ls = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    DWORD v6only = 0, one = 1;
    setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof v6only);      // accept ::1 and 127.0.0.1
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in6 a{}; a.sin6_family = AF_INET6; a.sin6_port = htons((u_short)port); a.sin6_addr = in6addr_any;
    if (bind(ls, (sockaddr*)&a, sizeof a) || listen(ls, 1)) { fprintf(stderr, "bind/listen failed\n"); return 1; }

    mmsdr_stats last = stats(); ULONGLONG tl = GetTickCount64();
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c != INVALID_SOCKET) serveClient(c, last, tl);
    }
}
