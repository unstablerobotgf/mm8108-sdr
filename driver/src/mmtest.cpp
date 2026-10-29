// Standalone check of libmmsdr: mmtest <fw> <bcf> <freq_hz> <secs> [out.cs16] [retune_hz]
// Raw CS16 (host DSP bypassed) so numbers compare with the pre-library driver. Experiments via MM_* env vars.
#include "mmsdr.h"
#include "mmsdr_debug.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <complex>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <Windows.h>

static mmsdr_dev* dev;
static uint32_t r32(uint32_t a) {
    uint32_t v = 0; int e = mmsdr_peek32(dev, a, &v);
    if (e) fprintf(stderr, "peek32 %06x: %s (%s)\n", a, mmsdr_strerror(e), mmsdr_last_error_detail(dev));
    return v;
}
static void w32(uint32_t a, uint32_t v) {
    int e = mmsdr_poke32(dev, a, v);
    if (e) fprintf(stderr, "poke32 %06x: %s (%s)\n", a, mmsdr_strerror(e), mmsdr_last_error_detail(dev));
}
static mmsdr_stats stats() { mmsdr_stats s{}; s.struct_size = sizeof s; mmsdr_get_stats(dev, &s); return s; }
static std::string retune(double f) {
    mmsdr_apply_result r{}; r.struct_size = sizeof r;
    int e = mmsdr_set_freq(dev, f, &r);
    return e ? std::string(mmsdr_strerror(e)) + ": " + mmsdr_last_error_detail(dev) : "";
}
static bool lastHop;

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: mmtest <fw> <bcf> <freq_hz> <secs> [out.cs16] [retune_hz]\n"); return 2; }
    if (getenv("MM_HIGH")) SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);   // libusb IOCP thread inherits the class
    if (getenv("MM_LOG")) mmsdr_set_log(nullptr, nullptr, MMSDR_LOG_DEBUG);
    FILE* out = argc > 5 ? fopen(argv[5], "wb") : nullptr;
    double f2 = argc > 6 ? atof(argv[6]) : 0, secs = atof(argv[4]);
    std::atomic<double> pw{ 0 }, si{ 0 }, sq{ 0 }; std::atomic<uint64_t> pn{ 0 };   // output power meter (total, incl. DC) + raw DC
    std::mutex capMtx; std::vector<int16_t> cap; size_t capWant = 0;  // contiguous I/Q capture for MM_LPF
    std::vector<int16_t> conv;                                        // CS8 -> int16 scale for the meters
    struct Ctx { std::function<void(const mmsdr_rx_block*)> fn; } ctx;
    ctx.fn = [&](const mmsdr_rx_block* b) {
        const int16_t* iq = (const int16_t*)b->samples; int n = (int)b->count;
        if (b->format == MMSDR_FMT_CS8) {
            conv.resize(2 * (size_t)n); const int8_t* p = (const int8_t*)b->samples;
            for (int i = 0; i < 2 * n; i++) conv[i] = (int16_t)(p[i] * 256);
            iq = conv.data();
        }
        if (out) fwrite(iq, 4, n, out);
        { std::lock_guard<std::mutex> l(capMtx); if (cap.size() < capWant) cap.insert(cap.end(), iq, iq + std::min<size_t>(2 * (size_t)n, capWant - cap.size())); }
        double p = 0; for (int i = 0; i < 2 * n; i++) p += (double)iq[i] * iq[i];
        double a = 0, c = 0; for (int i = 0; i < n; i++) { a += iq[2 * i]; c += iq[2 * i + 1]; }
        pw = pw + p; si = si + a; sq = sq + c; pn += n;
    };

    mmsdr_open_args oa{}; oa.struct_size = sizeof oa; oa.firmware_path = argv[1]; oa.bcf_path = argv[2];
    if (const char* p = getenv("MM_PPB")) oa.freq_correction_ppb = atoi(p);
    if (int e = mmsdr_open(&dev, &oa)) { fprintf(stderr, "open failed: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(nullptr)); return 1; }
    { mmsdr_devinfo di{}; di.struct_size = sizeof di; mmsdr_get_info(dev, &di); printf("device %s uid %s serial %s chip %#x\n", di.usb_path, di.uid, di.serial, di.chip); }
    mmsdr_debug_set(dev, "raw", "1");
    if (getenv("MM_DSP")) mmsdr_debug_set(dev, "raw", "0");   // library host DSP (DC tracker) on
    // former core env hooks are debug keys now
    for (auto kv : { std::pair<const char*, const char*>{ "MM_UNCLAMP", "unclamp" }, { "MM_NOKDCO", "nokdco" }, { "MM_NOHOP", "nohop" },
                     { "MM_BAND54", "band54" }, { "MM_CC", "cc" }, { "MM_CRASHBCAST", "crashbcast" }, { "MM_CH", "usb.ch" }, { "MM_K", "usb.k" },
                     { "MM_DEPTH", "usb.depth" }, { "MM_PRIO", "usb.prio" }, { "MM_WAIT_US", "usb.wait_us" } })
        if (const char* v = getenv(kv.first)) mmsdr_debug_set(dev, kv.second, *v ? v : "1");
    if (getenv("MM_WIDE")) {
        double w = atof(getenv("MM_WIDE")); mmsdr_apply_result r{}; r.struct_size = sizeof r;
        // rate range depends on the tune frequency: store it first
        mmsdr_set_freq(dev, atof(argv[3]), nullptr);
        if (int e = mmsdr_set_sample_rate(dev, w, &r)) fprintf(stderr, "set_sample_rate %.0f: %s (%s)\n", w, mmsdr_strerror(e), mmsdr_last_error_detail(dev));
    }
    if (getenv("MM_PACK")) mmsdr_set_format(dev, MMSDR_FMT_CS8, nullptr);
    if (getenv("MM_DCOFF")) mmsdr_set_dc_mode(dev, MMSDR_DC_OFF);        // raw analog offset, no fw servo
    if (getenv("MM_GENPARAM") || getenv("MM_CAL811D")) {
        // GET_SET_GENERIC_PARAM sweep before medium eval; host cal cmd 0x811D after it (UPHY still alive)
        mmsdr_debug_set_hook(dev, [](void*, mmsdr_dev* d, mmsdr_hook_point pt) {
            if (pt == MMSDR_HOOK_PRE_MEDIUM_EVAL && getenv("MM_GENPARAM"))
                for (uint32_t pid = 0; pid < 38; pid++) {
                    uint32_t v = 0; int st = mmsdr_generic_param(d, pid, 0, &v);
                    fprintf(stderr, "[param] %2u: status %d value %u (%#x)\n", pid, st, v, v);
                }
            if (pt == MMSDR_HOOK_TUNED && getenv("MM_CAL811D")) {
                auto dump = [&](const char* tag) {
                    fprintf(stderr, "[cal] %s dc 18=%08x 1c=%08x 24=%08x 2c=%08x 44=%08x  1334=%08x 1374=%08x 1378=%08x 510000=%08x\n", tag,
                            r32(0x402018), r32(0x40201c), r32(0x402024), r32(0x40202c), r32(0x402044), r32(0x401334), r32(0x401374), r32(0x401378), r32(0x510000));
                    uint8_t t[24]; mmsdr_peek(d, 0x55c254, t, 24);
                    fprintf(stderr, "[cal] %s gaintab[0..1]", tag); for (uint8_t b : t) fprintf(stderr, " %02x", b); fprintf(stderr, "\n");
                };
                uint32_t mask = (uint32_t)strtoul(getenv("MM_CAL811D"), nullptr, 0);
                dump("before");
                uint8_t pl[5] = {}; memcpy(pl, &mask, 4); pl[4] = 1; uint8_t rsp[64]; uint32_t rl = sizeof rsp;
                auto t0 = std::chrono::steady_clock::now(); int st = mmsdr_cmd(d, 0x811D, pl, 5, rsp, &rl, 5000);
                fprintf(stderr, "[cal] 0x811D mask %#x -> status %d in %.0f ms, resp %u B:", mask, st,
                        1e3 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), rl);
                for (uint32_t i = 0; i < std::min<uint32_t>(rl, 48); i++) fprintf(stderr, " %02x", rsp[i]);
                fprintf(stderr, "\n");
                dump("after ");
            }
        }, nullptr);
    }
    mmsdr_set_freq(dev, atof(argv[3]), nullptr);
    mmsdr_rx_args ra{}; ra.struct_size = sizeof ra; ra.user = &ctx;
    ra.callback = [](void* u, const mmsdr_rx_block* b) { ((Ctx*)u)->fn(b); };
    auto t0 = std::chrono::steady_clock::now();
    if (int e = mmsdr_rx_start(dev, &ra)) { fprintf(stderr, "retune failed: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(dev)); mmsdr_close(dev); return 1; }
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("tuned %.4f MHz in %.0f ms, fs %.4f MS/s (boosted %d libusb threads)\n", mmsdr_get_freq(dev) / 1e6, 1e3 * dt, mmsdr_get_sample_rate(dev) / 1e6, stats().boosted_threads); fflush(stdout);
    auto finish = [&](int code) { mmsdr_close(dev); if (out) fclose(out); return code; };
    if (getenv("MM_SWEEP138C")) {
        // RF band code 0x40138c[9:3] (lphy_rx_chain_channel_setup): step it, measure mean power; fw value re-measured last for drift
        const uint32_t R = 0x40138c, boot = r32(R), code0 = (boot >> 3) & 0x7f;
        printf("0x40138c boot = %08x, code[9:3] = %u (0x%02x)\n", boot, code0, code0 << 3);
        auto meas = [&](uint32_t code) {
            w32(R, (boot & ~(0x7fu << 3)) | code << 3);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            mmsdr_stats c0 = stats(); pw = 0; pn = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            mmsdr_stats c1 = stats();
            double db = 10 * log10(pw.load() / std::max<uint64_t>(pn.load(), 1) / (32768.0 * 32768.0) + 1e-30);
            printf("  code %3u (0x%03x): %6.2f dBFS  clipped %llu  rb %08x\n", code, code << 3, db, c1.clipped - c0.clipped, r32(R));
            fflush(stdout); return db;
        };
        if (getenv("MM_AB")) {
            // paired A/B against the boot code: traffic drift on live LTE cancels in the per-pair difference
            auto win = [&](uint32_t code) {
                w32(R, (boot & ~(0x7fu << 3)) | code << 3);
                std::this_thread::sleep_for(std::chrono::milliseconds(40)); pw = 0; pn = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return 10 * log10(pw.load() / std::max<uint64_t>(pn.load(), 1) / (32768.0 * 32768.0) + 1e-30);
            };
            for (uint32_t c : { 0u, 8u, 40u, 64u, 96u, 127u }) {
                double s = 0, s2 = 0; const int N = 8;
                for (int i = 0; i < N; i++) { double a = win(code0), b = win(c), a2 = win(code0), d = b - (a + a2) / 2; s += d; s2 += d * d; }
                double m = s / N, sd = sqrt(std::max(0.0, s2 / N - m * m));
                printf("  code %3u vs %u: %+6.2f dB  (sd %.2f, se %.2f)\n", c, code0, m, sd, sd / sqrt((double)N)); fflush(stdout);
            }
            w32(R, boot); return finish(0);
        }
        double ref = meas(code0);
        for (uint32_t c = 0; c < 128; c += 8) meas(c);
        printf("drift check: boot code %.2f -> %.2f dB\n", ref, meas(code0));
        return finish(0);
    }
    if (const char* po = getenv("MM_POKE")) {
        // write/readback/restore: MM_POKE=addr:val[,addr:val] (is the word backed by a real register?)
        for (const char* p = po; *p; ) {
            uint32_t a = strtoul(p, (char**)&p, 0); p++; uint32_t v = strtoul(p, (char**)&p, 0); if (*p == ',') p++;
            uint32_t o = r32(a); w32(a, v); uint32_t rb = r32(a); w32(a, o);
            printf("%06x: was %08x, wrote %08x, read back %08x, restored %08x\n", a, o, v, rb, r32(a));
        }
        retune(mmsdr_get_freq(dev)); return finish(0);
    }
    if (const char* pk = getenv("MM_PEEK")) {
        // read-only word dump after boot: MM_PEEK=addr:bytes[,addr:bytes] (library enforces <= 60 B MMIO windows + allowlist)
        for (const char* p = pk; *p; ) {
            uint32_t a = strtoul(p, (char**)&p, 0); p++; uint32_t n = strtoul(p, (char**)&p, 0); if (*p == ',') p++;
            for (uint32_t o = 0; o < n; o += 16) {
                printf("%08x:", a + o);
                for (uint32_t k = o; k < std::min(n, o + 16); k += 4) printf(" %08x", r32(a + k));
                printf("\n");
            }
        }
        return finish(0);
    }
    if (getenv("MM_RATIOCHK")) {
        // wide-mode race check: resampler ratio 0x409404 and UPHY panic marker 0x5380d4 (0xfee1dead = parked) over time
        const uint32_t want = (uint32_t)std::llround(mmsdr_get_freq(dev) * 2 / 128 / (2 * mmsdr_get_sample_rate(dev)) * 4294967296.0);
        for (int ms : { 0, 50, 200, 1000, 3000 }) {
            static int last = 0; std::this_thread::sleep_for(std::chrono::milliseconds(ms - last)); last = ms;
            uint32_t r = r32(0x409404); mmsdr_stats s = stats();
            printf("  +%4d ms: 0x409404 %08x (want %08x)%s  0x5380d4 %08x  samples %llu\n", ms, r, want, r == want ? "" : "  OVERWRITTEN",
                   r32(0x5380d4), s.samples);
        }
        return finish(0);
    }
    if (const char* lp = getenv("MM_LPF")) {
        // LPF code A/B: 4 sections 0x402038[27:20] + 0x40203c bytes 0-2 (fw bw8 idx0 = 8). Paired code-8/X/code-8 PSD
        // ratios cancel stimulus + traffic drift. MM_LPF=c1,c2,...; run with MM_WIDE=9.7e6.
        const int N = 1024, NB = 256; const double fs = mmsdr_get_sample_rate(dev);
        if (getenv("MM_GAIN")) mmsdr_set_gain(dev, atoi(getenv("MM_GAIN")));   // keep strong LTE out of clipping
        // fields under test: MM_ABF="addr:lsb:width,..." (default the 4 LPF sections); REF = exact boot words
        struct Fld { uint32_t a, lsb, w; }; std::vector<Fld> fl;
        const char* af = getenv("MM_ABF") ? getenv("MM_ABF") : "0x402038:20:8,0x40203c:0:8,0x40203c:8:8,0x40203c:16:8";
        for (const char* p = af; *p; ) { Fld x; x.a = strtoul(p, (char**)&p, 0); p++; x.lsb = strtoul(p, (char**)&p, 0); p++; x.w = strtoul(p, (char**)&p, 0); if (*p == ',') p++; fl.push_back(x); }
        std::vector<std::pair<uint32_t, uint32_t>> boot;
        for (auto& x : fl) if (std::none_of(boot.begin(), boot.end(), [&](auto& b) { return b.first == x.a; })) boot.push_back({ x.a, r32(x.a) });
        for (auto& b : boot) printf("boot %06x = %08x\n", b.first, b.second);
        printf("fs %.4f MS/s\n", fs / 1e6);
        const uint32_t REF = 0xffffffff;
        const bool strobe = getenv("MM_ABF_STROBE") != nullptr;      // uphy_rf_set_402000_codes load sequence
        auto setc = [&](uint32_t c) {
            if (strobe) { uint32_t v = r32(0x402004) | 0x20000; w32(0x402004, v); v &= ~0x200000u; w32(0x402004, v); w32(0x402004, v | 0x10000); }
            uint32_t v4 = 0;
            for (auto& b : boot) {
                uint32_t v = b.second;
                if (c != REF) for (auto& x : fl) if (x.a == b.first) { uint32_t m = ((1u << x.w) - 1) << x.lsb; v = (v & ~m) | (c << x.lsb & m); }
                if (strobe && b.first == 0x402004) { v4 = v; v = (v | 0x10000) & ~0x200000u; }
                w32(b.first, v);
            }
            if (strobe) {
                if (!v4) v4 = r32(0x402004) & ~0x10000u;
                w32(0x402004, v4 | 0x210000); std::this_thread::sleep_for(std::chrono::microseconds(30)); w32(0x402004, (v4 | 0x200000) & ~0x10000u);
            }
        };
        auto fft = [](std::vector<std::complex<double>>& a) {
            int n = (int)a.size();
            for (int i = 1, j = 0; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) std::swap(a[i], a[j]); }
            for (int len = 2; len <= n; len <<= 1) {
                std::complex<double> w(cos(-2 * M_PI / len), sin(-2 * M_PI / len));
                for (int i = 0; i < n; i += len) { std::complex<double> u(1); for (int k = 0; k < len / 2; k++, u *= w) {
                    auto x = a[i + k], y = a[i + k + len / 2] * u; a[i + k] = x + y; a[i + k + len / 2] = x - y; } }
            }
        };
        auto psd = [&](uint32_t c) {                                 // mean |X|^2 per bin, NB Hann-windowed blocks
            setc(c); std::this_thread::sleep_for(std::chrono::milliseconds(40));
            { std::lock_guard<std::mutex> l(capMtx); cap.clear(); capWant = (size_t)N * NB * 2; }
            for (int t = 0; ; t++) {                                 // stalled stream (a field can stop the sample clock)
                std::this_thread::sleep_for(std::chrono::milliseconds(5)); std::lock_guard<std::mutex> l(capMtx);
                if (cap.size() >= capWant) break;
                if (t > 600) { capWant = 0; printf("  STALL: no samples 3 s after code %u (samples %llu)\n", c, stats().samples); fflush(stdout); setc(REF); exit(3); }
            }
            std::vector<int16_t> s; { std::lock_guard<std::mutex> l(capMtx); s.swap(cap); capWant = 0; }   // FFT outside the IQ path lock
            std::vector<double> P(N, 0); std::vector<std::complex<double>> a(N);
            for (int b = 0; b < NB; b++) {
                for (int i = 0; i < N; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / N); size_t k = 2 * ((size_t)b * N + i); a[i] = { w * s[k], w * s[k + 1] }; }
                fft(a); for (int i = 0; i < N; i++) P[i] += std::norm(a[i]);
            }
            return P;
        };
        auto band = [&](const std::vector<double>& r, double off) {   // mean ratio over +-off +-125 kHz (both sides)
            double s = 0; int n = 0;
            for (int i = 0; i < N; i++) { double f = (i < N / 2 ? i : i - N) * fs / N; if (std::fabs(std::fabs(f) - off) <= 125e3 && std::fabs(f) > 60e3) { s += r[i]; n++; } }
            return 10 * log10(s / std::max(n, 1));
        };
        const double OFF[] = { 0.3e6, 1e6, 2e6, 3e6, 4e6, 4.6e6 };
        printf("code   0.3MHz   1MHz   2MHz   3MHz   4MHz  4.6MHz  (dB vs boot, mean of 8 pairs)  clipped\n");
        for (const char* p = lp; *p; ) {
            uint32_t c = strtoul(p, (char**)&p, 0); if (*p == ',') p++;
            std::vector<double> acc(6, 0); uint64_t cl0 = stats().clipped;
            setc(c); printf("     rb"); for (auto& b : boot) printf(" %06x=%08x", b.first, r32(b.first)); printf("\n");
            for (int k = 0; k < 8; k++) {
                auto a = psd(REF), b = psd(c), a2 = psd(REF); std::vector<double> r(N);
                for (int i = 0; i < N; i++) r[i] = b[i] / (0.5 * (a[i] + a2[i]) + 1e-30);
                for (int j = 0; j < 6; j++) acc[j] += band(r, OFF[j]) / 8;
            }
            printf("%4u ", c); for (double v : acc) printf(" %+6.2f", v); printf("   %llu   shape vs 1MHz:", stats().clipped - cl0);
            for (int j = 2; j < 6; j++) printf(" %+5.2f", acc[j] - acc[1]); printf("\n"); fflush(stdout);   // cancels common-mode drift
        }
        setc(REF); return finish(0);
    }
    if (const char* fsc = getenv("MM_FIELDSCAN")) {
        // LO divider hunt: MM_FIELDSCAN="addr:lsb:width,..." (fields set_lo writes constant). Per value: carrier dB over floor
        // at MM_REF_HZ offset, log-PSD correlation with stock, fs, ADPLL T. A divider: carrier gone, corr low, fs/T unchanged.
        const int N = 4096, NB = 48; const double fs0 = mmsdr_get_sample_rate(dev), ref = getenv("MM_REF_HZ") ? atof(getenv("MM_REF_HZ")) : -437.5e3;
        auto fft = [](std::vector<std::complex<double>>& a) {
            int n = (int)a.size();
            for (int i = 1, j = 0; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) std::swap(a[i], a[j]); }
            for (int len = 2; len <= n; len <<= 1) {
                std::complex<double> w(cos(-2 * M_PI / len), sin(-2 * M_PI / len));
                for (int i = 0; i < n; i += len) { std::complex<double> u(1); for (int k = 0; k < len / 2; k++, u *= w) {
                    auto x = a[i + k], y = a[i + k + len / 2] * u; a[i + k] = x + y; a[i + k + len / 2] = x - y; } }
            }
        };
        auto measure = [&](std::vector<double>& logP, double& carrier, double& fsm, int& T, double& tot) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            uint64_t s0 = stats().samples; auto t0 = std::chrono::steady_clock::now();
            { std::lock_guard<std::mutex> l(capMtx); cap.clear(); capWant = (size_t)N * NB * 2; }
            for (int t = 0; t < 400; t++) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); std::lock_guard<std::mutex> l(capMtx); if (cap.size() >= capWant) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            fsm = (stats().samples - s0) / std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::vector<int16_t> s; { std::lock_guard<std::mutex> l(capMtx); s.swap(cap); capWant = 0; }
            std::vector<double> P(N, 0); std::vector<std::complex<double>> a(N); tot = 0;
            int nb = (int)(s.size() / (2 * N));
            for (int b = 0; b < nb; b++) {
                for (int i = 0; i < N; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / N); size_t k = 2 * ((size_t)b * N + i); a[i] = { w * s[k], w * s[k + 1] }; tot += (double)s[k] * s[k] + (double)s[k + 1] * s[k + 1]; }
                fft(a); for (int i = 0; i < N; i++) P[i] += std::norm(a[i]);
            }
            tot = 10 * log10(tot / std::max<double>(1.0, (double)nb * N) / (32768.0 * 32768.0) + 1e-30);
            logP.assign(N, 0); std::vector<double> srt(P); std::nth_element(srt.begin(), srt.begin() + N / 2, srt.end()); double fl = srt[N / 2] + 1e-30;
            for (int i = 0; i < N; i++) logP[i] = 10 * log10(P[i] / fl + 1e-12);
            int c = (int)lround(ref / fs0 * N); c = (c % N + N) % N; double mx = 0;
            for (int d = -2; d <= 2; d++) mx = std::max(mx, P[((c + d) % N + N) % N]);
            carrier = 10 * log10(mx / fl);
            uint32_t r50 = r32(0x402050); w32(0x402050, (r50 & 0xcfffffff) | 0x20000000); T = (int)(r32(0x402068) & 0x1ff); w32(0x402050, r50);
        };
        auto corr = [&](const std::vector<double>& x, const std::vector<double>& y) {
            double mx = 0, my = 0; for (int i = 0; i < N; i++) { mx += x[i]; my += y[i]; } mx /= N; my /= N;
            double sxy = 0, sxx = 0, syy = 0; for (int i = 0; i < N; i++) { sxy += (x[i] - mx) * (y[i] - my); sxx += (x[i] - mx) * (x[i] - mx); syy += (y[i] - my) * (y[i] - my); }
            return sxy / sqrt(sxx * syy + 1e-30);
        };
        std::vector<double> base; double bc, bf, bt; int bT; measure(base, bc, bf, bT, bt);
        printf("stock: carrier %+5.1f dB  fs %.4f MS/s  T %d  total %.1f dBFS\n", bc, bf / 1e6, bT, bt); fflush(stdout);
        for (const char* p = fsc; *p; ) {
            uint32_t a = strtoul(p, (char**)&p, 0); p++; uint32_t lsb = strtoul(p, (char**)&p, 0); p++; uint32_t w = strtoul(p, (char**)&p, 0); if (*p == ',') p++;
            uint32_t orig = r32(a), m = ((1u << w) - 1) << lsb, cur = (orig & m) >> lsb;
            printf("%06x[%u:%u] stock %u (word %08x)\n", a, lsb + w - 1, lsb, cur, orig);
            for (uint32_t v = 0; v < (1u << std::min(w, 3u)) || (w > 3 && v < 8); v++) {
                uint32_t val = w > 3 ? (v * ((1u << w) - 1) / 7) : v;
                if (val == cur) continue;
                w32(a, (orig & ~m) | (val << lsb) & m);
                std::vector<double> lp; double c, f, t; int T; measure(lp, c, f, T, t);
                w32(a, orig); retune(mmsdr_get_freq(dev));           // re-acquire: a field that broke lock stays broken otherwise
                printf("   = %3u: carrier %+5.1f dB  corr %+.2f  fs %.4f MS/s  T %3d  total %6.1f dBFS%s\n", val, c, corr(base, lp), f / 1e6, T, t,
                       c < bc - 15 && corr(base, lp) < 0.5 && std::fabs(f / bf - 1) < 0.01 ? "   <-- band moved?" : "");
                fflush(stdout);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
        }
        std::vector<double> lp; double c, f, t; int T; measure(lp, c, f, T, t);
        printf("after restore: carrier %+5.1f dB  corr %+.2f  fs %.4f  T %d\n", c, corr(base, lp), f / 1e6, T);
        return finish(0);
    }
    if (getenv("MM_TSTEP")) {
        // hwtimer 0x406040 tick rate, then ADPLL tracking word T step response to a +5 MHz FCW offset (0x402060)
        auto hs = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        double h0 = hs(); uint32_t t0 = r32(0x406040); std::this_thread::sleep_for(std::chrono::milliseconds(500));
        double h1 = hs(); uint32_t t1 = r32(0x406040);
        printf("hwtimer 0x406040: %.4f MHz\n", (t1 - t0) / (h1 - h0) / 1e6);
        uint32_t c = r32(0x402050), r60 = r32(0x402060); w32(0x402050, (c & 0xcfffffff) | 0x20000000);
        for (int rep = 0; rep < 2; rep++) {
            uint32_t off = rep ? r60 : 0x500000; double ts = hs(); w32(0x402060, off);
            for (int i = 0; i < 25; i++) printf("  %s +%7.3f ms: T %3u\n", rep ? "off->0  " : "0->+5MHz", 1e3 * (hs() - ts), r32(0x402068) & 0x1ff);
        }
        w32(0x402050, (r32(0x402050) & 0xcfffffff) | (c & 0x30000000)); return finish(0);
    }
    if (getenv("MM_VCOMAP")) {
        // VCO lock map on the x2 path: ADPLL tracking word T (0x402068[8:0], sel 0x402050[29:28]=2, mid 0x100),
        // loop-alive nudge (0x402060 +-1 MHz like lphy_pll_kdco_cal), true VCO from delivered fs. MM_VCOMAP=lo:hi:step MHz
        double lo = 745, hi = 1500, st = 25; sscanf(getenv("MM_VCOMAP"), "%lf:%lf:%lf", &lo, &hi, &st);
        mmsdr_debug_set(dev, "unclamp", "1");
        auto sel = [&](bool on) { uint32_t c = r32(0x402050); w32(0x402050, (c & 0xcfffffff) | (on ? 0x20000000u : 0)); };
        auto fsMeas = [&]() {                                       // least-squares slope of sample count vs time, 1.5 s
            double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0; auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i <= 30; i++, n++) {
                double x = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), y = (double)stats().samples;
                sx += x; sy += y; sxx += x * x; sxy += x * y; std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return (n * sxy - sx * sy) / (n * sxx - sx * sx);
        };
        auto point = [&](double f) {
            std::string e = retune(f * 1e6);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            uint32_t r58 = r32(0x402058), r64 = r32(0x402064), r5c = r32(0x40205c), r54 = r32(0x402054), r60 = r32(0x402060);
            int cc = (r58 >> 10 & 0x7f) * ((r64 >> 4 & 1) ? -1 : 1);
            sel(true); int T = r32(0x402068) & 0x1ff;
            w32(0x402060, 0x100000); std::this_thread::sleep_for(std::chrono::milliseconds(2)); int tp = r32(0x402068) & 0x1ff;
            w32(0x402060, ~0x100000u); std::this_thread::sleep_for(std::chrono::milliseconds(2)); int tm = r32(0x402068) & 0x1ff;
            w32(0x402060, r60); sel(false);
            uint32_t ratio = r32(0x409404), div = r32(0x402004) >> 18 & 7;
            double fs = fsMeas(), vco = fs * 2 * (ratio / 4294967296.0) * (128 << div), want = 2 * mmsdr_get_freq(dev);
            printf("%7.1f MHz: T %3d (%+4d) nudge %+4d  cc %+4d  5c %05x  54[10:6] %2u  fs %.5f  VCO %8.2f / %8.2f MHz (%+.4f%%)%s%s\n",
                   f, T, T - 0x100, tp - tm, cc, r5c, r54 >> 6 & 0x1f, fs / 1e6, vco / 1e6, want / 1e6, 100 * (vco / want - 1),
                   T <= 0x95 ? "  RAIL(fw<=0x95)" : "", e.empty() ? "" : ("  err " + e).c_str());
            fflush(stdout); return std::fabs(vco / want - 1) < 1e-3;
        };
        point(900);                                                  // reference inside the verified window
        if (const char* cs = getenv("MM_CCSWEEP")) {
            // test: signed coarse bank 0x402058[16:10] (sign 0x402064 b4) at each listed LO (MHz, comma separated)
            for (const char* p = cs; *p; ) {
                double f = strtod(p, (char**)&p); if (*p == ',') p++;
                for (int v : { -127, -96, -64, -48, -32, -16, -8, 0, 16, 64 }) {
                    mmsdr_debug_set(dev, "cc", std::to_string(v).c_str());
                    printf("cc %4d ", v); if (!point(f)) { mmsdr_debug_set(dev, "cc", ""); printf("   recovery -> "); point(900); }
                }
            }
            return finish(0);
        }
        if (const char* bs = getenv("MM_BANDSWEEP")) {
            // test: band field 0x402054[10:6] = 16..31 at each listed LO (MHz, comma separated); stock 22
            for (const char* p = bs; *p; ) {
                double f = strtod(p, (char**)&p); if (*p == ',') p++;
                for (int v = 16; v < 32; v++) {
                    mmsdr_debug_set(dev, "band54", std::to_string(v).c_str());
                    printf("band %2d ", v); if (!point(f)) { mmsdr_debug_set(dev, "band54", ""); printf("   recovery -> "); point(900); }
                }
            }
            return finish(0);
        }
        for (double f = lo; st > 0 ? f <= hi + 1e-9 : f >= hi - 1e-9; f += st) if (!point(f)) { printf("   recovery -> "); point(900); }
        return finish(0);
    }
    mmsdr_stats p = stats(), a0 = p; auto ta = std::chrono::steady_clock::now();
    for (int s = 1; s <= (int)secs; s++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (f2 && s == (int)secs / 2) {
            auto t1 = std::chrono::steady_clock::now(); mmsdr_apply_result r{}; r.struct_size = sizeof r;
            int e = mmsdr_set_freq(dev, f2, &r);
            printf("retune -> %.4f MHz: %s (%s, %.1f ms)\n", mmsdr_get_freq(dev) / 1e6, e ? mmsdr_last_error_detail(dev) : "ok",
                   r.action == MMSDR_APPLY_HOP ? "hop" : "reboot", 1e3 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
            if (getenv("MM_RECAL")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                auto t2 = std::chrono::steady_clock::now(); int re = mmsdr_recal_dc(dev);
                printf("recalDc: %s (%.1f ms)\n", re ? mmsdr_last_error_detail(dev) : "ok", 1e3 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count());
            }
            pw = 0; si = 0; sq = 0; pn = 0;
            p = a0 = stats(); ta = std::chrono::steady_clock::now(); continue;
        }
        mmsdr_stats c = stats();
        { double nn = (double)std::max<uint64_t>(pn.load(), 1); printf("        dc I %+8.1f Q %+8.1f  |dc| %.1f dBFS\n", si.load() / nn, sq.load() / nn,
              20 * log10(std::hypot(si.load() / nn, sq.load() / nn) / 32768.0 + 1e-12)); si = 0; sq = 0; }
        printf("%3d s: %.4f MS/s  bad_ptr %llu  lapped %llu  usb_err %llu  overflow %llu  clipped %llu  peak %d  total %.1f dBFS  3004=%08x 1334=%08x 1374=%08x\n", s, (c.samples - p.samples) / 1e6,
               c.bad_ptr - p.bad_ptr, c.lapped - p.lapped, c.usb_err - p.usb_err, c.overflow - p.overflow, c.clipped - p.clipped, c.peak_abs,
               10 * log10(pw.exchange(0) / std::max<uint64_t>(pn.exchange(0), 1) / (32768.0 * 32768.0) + 1e-30), r32(0x403004), r32(0x401334), r32(0x401374));
        if (getenv("MM_PACK")) {                                // packer heartbeat: idle loops/s (low = saturated)
            static uint32_t hb0 = 0, sc0 = 0; uint32_t hb = r32(0x510f04), sc = r32(0x409050);
            printf("        packer idle loops %.2f M/s, hw source %.4f MS/s\n", (hb - hb0) / 1e6, (sc - sc0) / 1e6); hb0 = hb; sc0 = sc;
        }
        fflush(stdout); p = c;
    }
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - ta).count();
    printf("average since last tune: %.5f MS/s over %.1f s\n", (stats().samples - a0.samples) / el / 1e6, el);
    mmsdr_stats e = stats();
    printf("totals: hops %llu reboots %llu park_retries %llu\n", e.hops, e.reboots, e.park_retries);
    return finish(0);
}
