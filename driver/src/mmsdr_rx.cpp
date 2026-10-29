// Capture to a file with the sync API (rtl_sdr style).
// mmsdr_rx [-d uid|path] [-f hz] [-s rate] [-g idx] [-n samples] [-F cs16|cs8|cf32] [-p ppb] [-a] [-i] [-e] [--fw p] [--bcf p] out
#include "mmsdr.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int usage() {
    fprintf(stderr, "usage: mmsdr_rx [-d uid|usb_path] [-f hz] [-s rate] [-g 0-16] [-n samples] [-F cs16|cs8|cf32] [-p ppb]\n"
                    "                [-a (agc)] [-i (iq corr)] [-e (eq)] [--fw path] [--bcf path] out|-\n");
    return 2;
}

int main(int argc, char** argv) {
    double f = 915e6, fs = 0; int gain = 16; uint64_t want = 8000000; int fmt = MMSDR_FMT_CS16; int32_t ppb = 0;
    bool agc = false, iq = false, eq = false; std::string sel, fw, bcf, out;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i]; auto next = [&] { return i + 1 < argc ? argv[++i] : (char*)""; };
        if (a == "-d") sel = next(); else if (a == "-f") f = atof(next()); else if (a == "-s") fs = atof(next());
        else if (a == "-g") gain = atoi(next()); else if (a == "-n") want = (uint64_t)atof(next()); else if (a == "-p") ppb = atoi(next());
        else if (a == "-F") { std::string v = next(); fmt = v == "cs8" ? MMSDR_FMT_CS8 : v == "cf32" ? MMSDR_FMT_CF32 : MMSDR_FMT_CS16; }
        else if (a == "-a") agc = true; else if (a == "-i") iq = true; else if (a == "-e") eq = true;
        else if (a == "--fw") fw = next(); else if (a == "--bcf") bcf = next();
        else if (a[0] == '-' && a.size() > 1) return usage(); else out = a;
    }
    if (out.empty()) return usage();
    mmsdr_open_args oa{}; oa.struct_size = sizeof oa; oa.select = sel.empty() ? nullptr : sel.c_str();
    oa.firmware_path = fw.empty() ? nullptr : fw.c_str(); oa.bcf_path = bcf.empty() ? nullptr : bcf.c_str(); oa.freq_correction_ppb = ppb;
    mmsdr_dev* d;
    if (int e = mmsdr_open(&d, &oa)) { fprintf(stderr, "open: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(nullptr)); return 1; }
    auto chk = [&](int e, const char* what) {
        if (e >= 0) return;
        fprintf(stderr, "%s: %s (%s)\n", what, mmsdr_strerror(e), mmsdr_last_error_detail(d));
        mmsdr_close(d); exit(1);
    };
    chk(mmsdr_set_freq(d, f, nullptr), "set_freq"); chk(mmsdr_set_sample_rate(d, fs, nullptr), "set_sample_rate");
    chk(mmsdr_set_gain(d, gain), "set_gain"); chk(mmsdr_set_format(d, (mmsdr_format)fmt, nullptr), "set_format");
    mmsdr_set_agc(d, agc ? MMSDR_AGC_CLIP_GUARD : MMSDR_AGC_OFF); mmsdr_set_iq_correction(d, iq); mmsdr_set_passband_eq(d, eq);
    FILE* fo = out == "-" ? stdout : fopen(out.c_str(), "wb");
    if (!fo) { fprintf(stderr, "cannot open %s\n", out.c_str()); mmsdr_close(d); return 1; }
    mmsdr_rx_args ra{}; ra.struct_size = sizeof ra;
    chk(mmsdr_rx_start(d, &ra), "rx_start");
    fprintf(stderr, "%.6f MHz, %.4f MS/s, gain %d, ppb %d\n", mmsdr_get_freq(d) / 1e6, mmsdr_get_sample_rate(d) / 1e6, gain, ppb);
    const size_t bps = fmt == MMSDR_FMT_CS8 ? 2 : fmt == MMSDR_FMT_CF32 ? 8 : 4;
    std::vector<uint8_t> buf(65536 * bps); uint64_t got = 0, flagged = 0, overflow = 0, next = 0; int reads = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (got < want) {
        mmsdr_meta m{}; m.struct_size = sizeof m;
        int n = mmsdr_rx_read(d, buf.data(), (uint32_t)std::min<uint64_t>(65536, want - got), &m, 1000);
        chk(n, "rx_read");
        if (!n) { fprintf(stderr, "rx_read timeout\n"); break; }
        reads++;
        if (m.sample_index != next && got) fprintf(stderr, "  index jump %llu -> %llu\n", next, m.sample_index);
        if (m.flags) { flagged++; if (m.flags & MMSDR_META_OVERFLOW) overflow++;
            fprintf(stderr, "  @%llu flags 0x%02x  %.6f MHz %.4f MS/s gain %d\n", m.sample_index, m.flags, m.freq_hz / 1e6, m.sample_rate / 1e6, m.gain_idx); }
        next = m.sample_index + n;
        fwrite(buf.data(), bps, n, fo); got += n;
    }
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    mmsdr_stats s{}; s.struct_size = sizeof s; mmsdr_get_stats(d, &s);
    fprintf(stderr, "%llu samples in %.2f s (%d reads, %llu flagged, %llu overflow); laps %llu usb_err %llu bad_ptr %llu\n",
            got, el, reads, flagged, overflow, s.lapped, s.usb_err, s.bad_ptr);
    mmsdr_rx_stop(d); mmsdr_close(d);
    if (fo != stdout) fclose(fo);
    return 0;
}
