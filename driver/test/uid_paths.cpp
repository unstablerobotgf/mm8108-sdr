// Hardware test: uid via the fw host table and via OTP must agree. uid_paths <fw> <bcf>
// Clearing the host-table pointer 0x2d40 makes the next probe take the OTP path (digital reset + banks 26/27).
#include "mmsdr.h"
#include "mmsdr_debug.h"
#include <cstdio>
#include <cstring>
#include <string>

static std::string listUid() {
    mmsdr_devinfo di{}; di.struct_size = sizeof di;
    return mmsdr_list(&di, 1) >= 1 ? di.uid : "";
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: uid_paths <fw> <bcf>\n"); return 2; }
    mmsdr_open_args oa{}; oa.struct_size = sizeof oa; oa.firmware_path = argv[1]; oa.bcf_path = argv[2];
    mmsdr_dev* d;
    if (int e = mmsdr_open(&d, &oa)) { printf("open: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(nullptr)); return 1; }
    if (int e = mmsdr_boot(d)) { printf("boot: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(d)); return 1; }
    mmsdr_close(d);
    std::string a = listUid();                            // fw running: host table
    if (mmsdr_open(&d, &oa)) return 1;
    uint32_t ht = 0; mmsdr_peek32(d, 0x2d40, &ht);
    int pe = mmsdr_poke32(d, 0x2d40, 0);
    mmsdr_close(d);
    std::string b = listUid();                            // host table gone: OTP
    std::string c = listUid();                            // OTP again (no fw now)
    printf("host table @%#x -> uid %s\nOTP (poke rc %d)    -> uid %s\nOTP again          -> uid %s\n", ht, a.c_str(), pe, b.c_str(), c.c_str());
    bool ok = !a.empty() && a == b && b == c;
    printf("%s  uid identical on both paths\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
