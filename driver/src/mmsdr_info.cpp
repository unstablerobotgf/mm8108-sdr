// List MM8108 modules and what libmmsdr exposes for them. mmsdr_info [-v] [--fw p] [--bcf p]
#include "mmsdr.h"
#include "mmsdr_debug.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    bool verbose = false; std::string fw, bcf;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-v") verbose = true; else if (a == "--fw" && i + 1 < argc) fw = argv[++i]; else if (a == "--bcf" && i + 1 < argc) bcf = argv[++i];
    }
    uint32_t v = mmsdr_api_version();
    printf("libmmsdr %s (API %u.%u)\n", mmsdr_version_string(), v >> 16, v & 0xffff);
    mmsdr_devinfo info[8]; for (auto& x : info) { memset(&x, 0, sizeof x); x.struct_size = sizeof x; }
    int n = mmsdr_list(info, 8);
    if (n < 0) { printf("list: %s\n", mmsdr_strerror(n)); return 1; }
    printf("%d device(s)\n", n);
    for (int i = 0; i < n && i < 8; i++)
        printf("  [%d] %s  uid %s  serial %s  chip %#x%s\n", i, info[i].usb_path, info[i].uid[0] ? info[i].uid : "?",
               info[i].serial, info[i].chip, info[i].busy ? "  (busy)" : "");
    if (!verbose || n <= 0) return 0;
    mmsdr_open_args oa{}; oa.struct_size = sizeof oa;
    oa.firmware_path = fw.empty() ? nullptr : fw.c_str(); oa.bcf_path = bcf.empty() ? nullptr : bcf.c_str();
    mmsdr_dev* d;
    if (int e = mmsdr_open(&d, &oa)) { printf("open: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(nullptr)); return 1; }
    mmsdr_range r; mmsdr_get_freq_range(d, &r); printf("freq %.0f-%.0f MHz, step %.3f Hz\n", r.min / 1e6, r.max / 1e6, r.step);
    for (double f : { 900e6, 1050e6 }) { mmsdr_get_rate_range(d, f, &r); printf("rate at %.0f MHz: %.2f-%.2f MS/s\n", f / 1e6, r.min / 1e6, r.max / 1e6); }
    int lo, hi; mmsdr_get_gain_range(d, &lo, &hi); printf("gain index %d-%d (nominal 3 dB/step)\n", lo, hi);
    mmsdr_setting_info s[16]; for (auto& x : s) { memset(&x, 0, sizeof x); x.struct_size = sizeof x; }
    int k = mmsdr_list_settings(d, s, 16);
    printf("settings:\n");
    for (int i = 0; i < k && i < 16; i++) {
        char val[64] = ""; mmsdr_read_setting(d, s[i].key, val, sizeof val);
        printf("  %-20s = %-8s default %-6s %s%s%s\n", s[i].key, val, s[i].default_value,
               s[i].options ? "[" : "", s[i].options ? s[i].options : "", s[i].options ? "]" : "");
    }
    char dl[2048]; mmsdr_debug_list(d, dl, sizeof dl); printf("debug keys:\n%s", dl);
    mmsdr_close(d);
    return 0;
}
