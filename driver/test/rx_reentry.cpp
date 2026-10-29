// Hardware test: setters from inside the RX callback. rx_reentry <fw> <bcf>
// Expect: set_freq/set_gain from the callback return QUEUED/0, blocking calls return STATE, and later blocks carry
// RETUNED (new freq in meta) and GAIN_CHANGED. Prints PASS/FAIL per check.
#include "mmsdr.h"
#include "mmsdr_debug.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

struct T {
    mmsdr_dev* d; std::atomic<int> blocks{ 0 };
    int queuedFreq = -99, gainRc = -99, stopRc = -99, peekRc = -99; mmsdr_apply action = MMSDR_APPLY_NONE;
    std::atomic<bool> sawRetune{ false }, sawGain{ false }; double retuneFreq = 0; int retuneGain = -1; uint64_t lastIdx = 0; std::atomic<int> idxErr{ 0 };
};

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: rx_reentry <fw> <bcf>\n"); return 2; }
    T t;
    mmsdr_open_args oa{}; oa.struct_size = sizeof oa; oa.firmware_path = argv[1]; oa.bcf_path = argv[2];
    if (int e = mmsdr_open(&t.d, &oa)) { printf("open: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(nullptr)); return 1; }
    mmsdr_set_freq(t.d, 851.975e6, nullptr); mmsdr_set_gain(t.d, 12);
    mmsdr_rx_args ra{}; ra.struct_size = sizeof ra; ra.user = &t;
    ra.callback = [](void* u, const mmsdr_rx_block* b) {
        T& t = *(T*)u; int n = ++t.blocks;
        if (n > 1 && b->meta->sample_index != t.lastIdx) t.idxErr++;
        t.lastIdx = b->meta->sample_index + b->count;
        if (n == 500) {                                   // ~1 s in
            mmsdr_apply_result r{}; r.struct_size = sizeof r;
            t.queuedFreq = mmsdr_set_freq(t.d, 870.5e6, &r); t.action = r.action;
            t.gainRc = mmsdr_set_gain(t.d, 9);
            t.stopRc = mmsdr_rx_stop(t.d);
            uint32_t v; t.peekRc = mmsdr_peek32(t.d, 0x402050, &v);
        }
        if (n > 500 && (b->meta->flags & MMSDR_META_RETUNED) && !t.sawRetune) { t.sawRetune = true; t.retuneFreq = b->meta->freq_hz; }
        if (n > 500 && (b->meta->flags & MMSDR_META_GAIN_CHANGED) && !t.sawGain) { t.sawGain = true; t.retuneGain = b->meta->gain_idx; }
    };
    if (int e = mmsdr_rx_start(t.d, &ra)) { printf("rx_start: %s (%s)\n", mmsdr_strerror(e), mmsdr_last_error_detail(t.d)); return 1; }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    // enumerate while streaming: our device must come back busy with its uid, served from the registry, never re-claimed
    mmsdr_devinfo li[4]; for (auto& x : li) { x = mmsdr_devinfo{}; x.struct_size = sizeof x; }
    int nl = mmsdr_list(li, 4); bool ours = false;
    for (int i = 0; i < nl && i < 4; i++) ours |= li[i].busy && li[i].uid[0];
    std::this_thread::sleep_for(std::chrono::seconds(1));
    int fails = 0;
    auto check = [&](bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; };
    check(nl >= 1 && ours, "mmsdr_list while streaming: own device busy with uid");
    check(mmsdr_rx_is_streaming(t.d) == 1, "still streaming after list");
    check(t.queuedFreq == 0 && t.action == MMSDR_APPLY_QUEUED, "set_freq in callback -> QUEUED");
    check(t.gainRc == 0, "set_gain in callback -> 0 (queued)");
    check(t.stopRc == MMSDR_ERR_STATE, "rx_stop in callback -> STATE");
    check(t.peekRc == MMSDR_ERR_STATE, "peek32 in callback -> STATE");
    check(t.sawRetune && t.retuneFreq > 870.49e6 && t.retuneFreq < 870.51e6, "later block flagged RETUNED with 870.5 MHz in meta");
    check(t.sawGain && mmsdr_get_gain(t.d) == 9, "GAIN_CHANGED seen, gain now 9");
    check(t.idxErr == 0, "sample_index contiguous across blocks (no overflow)");
    printf("blocks %d  meta freq %.6f MHz  meta gain %d  get_freq %.6f MHz\n", t.blocks.load(), t.retuneFreq / 1e6, t.retuneGain, mmsdr_get_freq(t.d) / 1e6);
    mmsdr_close(t.d);
    return fails ? 1 : 0;
}
