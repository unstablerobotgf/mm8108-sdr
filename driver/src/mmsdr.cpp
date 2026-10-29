// libmmsdr: C ABI over the MM8108 core. Device lifecycle, settings, host DSP, callback + sync streaming, debug access.
#define MMSDR_BUILD
#include "mmsdr.h"
#include "mmsdr_debug.h"
#include "mm8108.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#ifndef MMSDR_GIT
#define MMSDR_GIT "dev"
#endif

// ============================================================================ helpers
namespace {
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- logging and per-thread error detail
std::mutex logMtx;
mmsdr_log_cb logCb = nullptr;
void* logUser = nullptr;
std::atomic<int> logMax{ MMSDR_LOG_WARN };

void logf(mmsdr_log_level lv, const char* fmt, ...) {
    if ((int)lv > logMax) return;
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    mmsdr_log_cb cb; void* u;
    { std::lock_guard<std::mutex> l(logMtx); cb = logCb; u = logUser; }   // call outside the lock: the sink may call mmsdr_set_log
    if (cb) { cb(u, lv, b); return; }
    static const char* names[] = { "error", "warn", "info", "debug" };
    fprintf(stderr, "[mmsdr %s] %s\n", names[std::clamp((int)lv, 0, 3)], b);
}

thread_local const mmsdr_dev* errDev = nullptr;
thread_local std::string errMsg;
thread_local const mmsdr_dev* inCb = nullptr;              // set while the user callback runs (output thread)

int failNoDev(int code, const std::string& msg) { errDev = nullptr; errMsg = msg; logf(MMSDR_LOG_WARN, "%s", msg.c_str()); return code; }

// ---- ABI structs: every one starts with struct_size, tail fields may be absent in older callers
template <class T> bool sized(const T* p) { return p && p->struct_size >= sizeof(size_t); }
#define HAS(p, field) ((p)->struct_size >= offsetof(std::remove_cv_t<std::remove_pointer_t<decltype(p)>>, field) + sizeof((p)->field))

void fillRes(mmsdr_apply_result* r, mmsdr_apply a, double f, double fs, uint32_t retries, double el) {
    if (!r || r->struct_size < sizeof(size_t)) return;
    mmsdr_apply_result v{ r->struct_size, a, f, fs, retries, el };
    memcpy((char*)r + sizeof(size_t), (char*)&v + sizeof(size_t), std::min(r->struct_size, sizeof v) - sizeof(size_t));
}

size_t bytesPer(int fmt) { return fmt == MMSDR_FMT_CS8 ? 2 : fmt == MMSDR_FMT_CF32 ? 8 : 4; }

// ---- setting enums as strings
const char* DC_NAMES[] = { "hold", "servo", "off" };
const char* FMT_NAMES[] = { "cs16", "cs8", "cf32" };
int enumIndex(const std::string& v, std::initializer_list<const char*> names) {   // -1 when v is not one of names
    int i = 0;
    for (const char* n : names) { if (v == n) return i; i++; }
    return -1;
}
int parseBool(const char* v) {                             // 1, 0 or -1 when unrecognized
    const std::string s = v ? v : "";
    if (s == "1" || s == "true" || s == "on" || s == "yes") return 1;
    if (s == "0" || s == "false" || s == "off" || s == "no") return 0;
    return -1;
}

// ---- firmware file lookup
bool fileExists(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (f) fclose(f); return f != nullptr; }
std::string dllDir() {
    HMODULE m = nullptr; char b[MAX_PATH] = "";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&dllDir, &m);
    GetModuleFileNameA(m, b, sizeof b);
    const std::string s = b; const size_t k = s.find_last_of("\\/");
    return k == std::string::npos ? "." : s.substr(0, k);
}

// ---- passband equalizer
// Inverse of the analog rolloff (LTE slice swept through the passband; fixed in Hz, not in fs):
// H(f) = -0.3443 f^2 - 0.00178 f^4 dB (f in MHz, 0.47 dB rms). 31-tap linear-phase FIR, boost capped at 10 dB.
double rolloffDb(double offHz) { double f = offHz / 1e6; return -0.34429 * f * f - 0.001778 * f * f * f * f; }

struct RolloffEq {
    static constexpr int T = 31;
    float h[T];
    double fs = 0;                                         // rate the taps were designed for; 0 = not designed
    std::vector<float> work;

    void design(double rate) {
        const int M = 1024;
        std::vector<double> g(M); double tap[T], sum = 0;
        for (int i = 0; i < M; i++) g[i] = pow(10.0, std::min(-rolloffDb((i - M / 2) * rate / M), 10.0) / 20);
        for (int k = 0; k < T; k++) {                      // real-even response -> cosine series, Blackman window
            const int n = k - T / 2; double acc = 0;
            for (int i = 0; i < M; i++) acc += g[i] * cos(2 * M_PI * (i - M / 2) * n / M);
            const double w = 0.42 - 0.5 * cos(2 * M_PI * k / (T - 1)) + 0.08 * cos(4 * M_PI * k / (T - 1));
            tap[k] = acc / M * w; sum += tap[k];
        }
        for (int k = 0; k < T; k++) h[k] = (float)(tap[k] / sum);
        fs = rate; work.assign(2 * (T - 1), 0.0f);
    }
    void run(float* x, int n) {                            // interleaved I/Q in place; keeps T-1 samples of history
        const size_t hist = 2 * (T - 1);
        work.resize(hist + 2 * (size_t)n);
        memcpy(&work[hist], x, sizeof(float) * 2 * n);
        const float* w = work.data();
        for (int i = 0; i < n; i++) {
            float re = 0, im = 0; const float* p = w + 2 * i;
            for (int k = 0; k < T; k++) { re += h[k] * p[2 * k]; im += h[k] * p[2 * k + 1]; }
            x[2 * i] = re; x[2 * i + 1] = im;
        }
        memmove(work.data(), &work[2 * (size_t)n], sizeof(float) * hist);
    }
};
}   // namespace

// ============================================================================ device object
struct mmsdr_dev {
    std::unique_ptr<MM8108> core;
    std::recursive_mutex mtx;                              // serializes chip access; never taken on the output thread
    std::string fw, bcf;

    // ---- requested settings (written by setters, read when applied)
    std::atomic<double> freq{ 915e6 }, rateReq{ 0 };      // rateReq 0 = default 8 MS/s
    std::atomic<int> fmt{ MMSDR_FMT_CS16 }, userGain{ MM8108::GAIN_MAX }, agcMode{ MMSDR_AGC_OFF }, dcMode{ MMSDR_DC_HOLD };
    std::atomic<bool> iqOn{ false }, eqOn{ false }, hopOn{ true }, raw{ false }, iqReset{ false };
    std::atomic<double> curFreq{ 0 }, curFs{ MM8108::FS }; // readbacks after the last tune
    int prioKnob = 15;                                     // usb.prio debug knob; rx_start's realtime_priority overrides per run

    // ---- stream configuration (rx_start, under mtx)
    // streamOn is the API-level state: it stays true across a reboot-retune, unlike core->streaming()
    mmsdr_rx_cb cb = nullptr;
    void* cbUser = nullptr;
    uint32_t blockMax = 16384;
    std::atomic<bool> streamOn{ false }, closing{ false };
    std::atomic<uint32_t> fmtEpoch{ 0 }, gainEpoch{ 0 };   // bumped by LIVE format / gain changes; the output thread flags the next block
    std::atomic<int> agcPeak{ 0 }, statsPeak{ 0 };         // block peaks, taken by the AGC tick / get_stats
    std::atomic<float> irr{ 0 };                           // image rejection the IQ corrector measures

    // ---- output thread state (onBlock only)
    uint64_t sampleIndex = 0;
    uint32_t gainSeen = 0, fmtSeen = 0, dcEp = 0;
    double dcI = 0, dcQ = 0; int dcFast = 0;               // hold-mode residual DC tracker; dcFast = blocks of fast re-acquire left
    std::complex<double> iqB = 0;                          // blind IQ imbalance estimate
    RolloffEq eq;
    std::vector<float> fbuf; std::vector<int16_t> sbuf; std::vector<int8_t> bbuf;   // conversion buffers
    double metaFreq = 0, metaFs = MM8108::FS;              // what the samples being delivered were captured at

    // ---- hop mark (markMtx): the control thread posts where the new LO applies, the output thread flags that block
    std::mutex markMtx;
    bool hopPending = false; uint64_t hopMark = 0; double hopFreq = 0, hopFs = 0;

    // ---- sync mode (syncMtx): ring of format bytes plus meta marks at the sample where meta changes
    struct Mark { uint64_t at; mmsdr_meta m; bool reported; };
    std::mutex syncMtx;
    std::condition_variable syncCv;
    std::vector<uint8_t> ring;
    uint64_t cap = 0, rw = 0, rr = 0;                      // capacity, write and read positions in samples
    size_t bps = 4;
    std::deque<Mark> marks;
    mmsdr_meta lastMeta{};
    bool dropPending = false; uint32_t dropFlags = 0;      // a dropped block's flags ride on the next kept one
    int readers = 0;                                       // threads inside rx_read; close waits for zero

    // ---- control thread (qMtx): chip work queued from the callback, plus the AGC tick
    std::thread ctl;
    std::mutex qMtx;
    std::condition_variable qCv;
    bool quit = false;
    struct Q { bool tune = false, gain = false, dc = false; bool any() const { return tune || gain || dc; } } q;
    int agc = MM8108::GAIN_MAX, quiet = 0, sinceBack = 1000; uint64_t clipLast = 0;   // AGC state (control thread, under mtx)

    // ---- debug
    std::vector<std::pair<uint32_t, uint32_t>> allow;
    mmsdr_hook_cb hookCb = nullptr; void* hookUser = nullptr;

    int fail(int code, const std::string& msg) { errDev = this; errMsg = msg; logf(MMSDR_LOG_WARN, "%s", msg.c_str()); return code; }
    void post(bool tune, bool gain, bool dc) {
        { std::lock_guard<std::mutex> l(qMtx); q.tune |= tune; q.gain |= gain; q.dc |= dc; }
        qCv.notify_all();
    }

    // output thread
    void onBlock(const int16_t* iq, int n, uint64_t pos);
    uint32_t blockFlags(uint64_t pos, int spw);
    bool measure(const int16_t* iq, int n, int rail, double& meanI, double& meanQ);
    void trackDc(double meanI, double meanQ, bool rawOut);
    const void* convertFloat(const int16_t* iq, int n, int f, bool iqc, bool eqc);
    void correctIq(float* x, int n);
    const void* convertInt(const int16_t* iq, int n, int f);
    void deliver(const void* out, int n, int f, uint32_t flags);
    void pushSync(const void* data, int n, const mmsdr_meta& m);

    // control path
    int retune(mmsdr_apply_result* res);
    void ctlLoop();
    void agcTick();
};

// ---- open-device registry: usb_path -> dev, so mmsdr_list reports our own devices without re-claiming them
namespace {
std::mutex regMtx;
std::map<std::string, mmsdr_dev*> openDevs;

std::set<std::string> openPaths() {
    std::lock_guard<std::mutex> l(regMtx);
    std::set<std::string> s;
    for (auto& kv : openDevs) s.insert(kv.first);
    return s;
}
}   // namespace

// ============================================================================ output thread: host DSP, meta, delivery
void mmsdr_dev::onBlock(const int16_t* iq, int n, uint64_t pos) {
    const bool packed = core->packed();
    uint32_t flags = blockFlags(pos, packed ? 2 : 1);
    double meanI = 0, meanQ = 0;
    if (measure(iq, n, packed ? 127 * 256 : 32767, meanI, meanQ)) flags |= MMSDR_META_CLIPPED;

    const bool rawOut = raw, iqc = iqOn && !rawOut, eqc = eqOn && !rawOut;
    const int f = fmt;
    trackDc(meanI, meanQ, rawOut);
    const void* out = (iqc || eqc || f == MMSDR_FMT_CF32) ? convertFloat(iq, n, f, iqc, eqc) : convertInt(iq, n, f);
    deliver(out, n, f, flags);
    sampleIndex += n;
}

// meta flags for the block starting at core sample position pos; also moves metaFreq/metaFs at a hop
uint32_t mmsdr_dev::blockFlags(uint64_t pos, int spw) {
    uint32_t flags = 0;
    if (pos == 0) {                                        // first block of a run (start or reboot-retune)
        flags |= MMSDR_META_DISCONTINUITY | MMSDR_META_RETUNED;
        metaFreq = core->synthHz(); metaFs = core->sampleRate();
        std::lock_guard<std::mutex> l(markMtx); hopPending = false;
    }
    else {
        std::lock_guard<std::mutex> l(markMtx);
        if (hopPending && pos >= hopMark) {
            flags |= MMSDR_META_DISCONTINUITY | MMSDR_META_RETUNED;
            metaFreq = hopFreq; metaFs = hopFs; hopPending = false;
        }
    }
    // SPSC drops are positioned: the core cuts blocks at the gap, so a gap taken here sits right before sample 0
    if (uint64_t gw = core->takeGap(pos / spw)) { flags |= MMSDR_META_OVERFLOW | MMSDR_META_DISCONTINUITY; sampleIndex += gw * spw; }
    if (uint32_t g = gainEpoch; g != gainSeen) { flags |= MMSDR_META_GAIN_CHANGED; gainSeen = g; }
    if (uint32_t e = fmtEpoch; e != fmtSeen) { flags |= MMSDR_META_RETUNED; fmtSeen = e; }
    return flags;
}

// raw stats on the chip's own samples, before any host DSP: block means, peak, clip at the rail
bool mmsdr_dev::measure(const int16_t* iq, int n, int rail, double& meanI, double& meanQ) {
    int pk = 0; bool clip = false; double mi = 0, mq = 0;
    for (int i = 0; i < n; i++) {
        const int a = iq[2 * i], b = iq[2 * i + 1];
        mi += a; mq += b;
        const int m = std::max(std::abs(a), std::abs(b));
        pk = std::max(pk, m); clip |= m >= rail;
    }
    for (int p = agcPeak.load(); pk > p && !agcPeak.compare_exchange_weak(p, pk);) {}
    for (int p = statsPeak.load(); pk > p && !statsPeak.compare_exchange_weak(p, pk);) {}
    meanI = mi / n; meanQ = mq / n;
    return clip;
}

// Hold mode: block-mean DC tracker (tau ~0.2 s) removes what the hw hold leaves. A hold freezes the 80 kHz servo's
// instantaneous (~2 us) estimate, i.e. a random offset, so re-acquire fast for ~20 ms after each one
void mmsdr_dev::trackDc(double meanI, double meanQ, bool rawOut) {
    const uint32_t ep = core->dcEpoch();
    if (ep != dcEp) { dcEp = ep; dcFast = 10; }
    const double a = dcFast > 0 ? (dcFast--, 0.3) : 0.01;
    if (dcMode == MMSDR_DC_HOLD && !rawOut) { dcI += (meanI - dcI) * a; dcQ += (meanQ - dcQ) * a; }
    else dcI = dcQ = 0;
}

// float path: DC removal, optional IQ correction and EQ, then the requested format
const void* mmsdr_dev::convertFloat(const int16_t* iq, int n, int f, bool iqc, bool eqc) {
    fbuf.resize(2 * (size_t)n);
    const float s = 1.0f / 32768.0f, oi = (float)dcI, oq = (float)dcQ;
    for (int i = 0; i < n; i++) { fbuf[2 * i] = (iq[2 * i] - oi) * s; fbuf[2 * i + 1] = (iq[2 * i + 1] - oq) * s; }
    if (iqReset.exchange(false)) iqB = 0;
    if (iqc) correctIq(fbuf.data(), n);
    else irr = 0;
    if (eqc) { if (metaFs != eq.fs) eq.design(metaFs); eq.run(fbuf.data(), n); }

    if (f == MMSDR_FMT_CF32) return fbuf.data();
    if (f == MMSDR_FMT_CS16) {
        sbuf.resize(2 * (size_t)n);
        for (size_t i = 0; i < 2 * (size_t)n; i++) sbuf[i] = (int16_t)std::clamp(lrintf(fbuf[i] * 32768.0f), -32768L, 32767L);
        return sbuf.data();
    }
    bbuf.resize(2 * (size_t)n);
    for (size_t i = 0; i < 2 * (size_t)n; i++) bbuf[i] = (int8_t)std::clamp(lrintf(fbuf[i] * 128.0f), -128L, 127L);
    return bbuf.data();
}

// blind IQ imbalance: y = x - b conj(x), b driven until y is proper (E[y^2] = 0), which nulls the image
void mmsdr_dev::correctIq(float* x, int n) {
    const float br = (float)iqB.real(), bi = (float)iqB.imag();
    double e2r = 0, e2i = 0, p = 0;
    for (int i = 0; i < n; i++) {
        const float xr = x[2 * i], xi = x[2 * i + 1];
        const float yr = xr - (br * xr + bi * xi), yi = xi - (bi * xr - br * xi);   // b * conj(x)
        x[2 * i] = yr; x[2 * i + 1] = yi;
        e2r += yr * yr - yi * yi; e2i += 2 * yr * yi; p += yr * yr + yi * yi;
    }
    if (p > 0) iqB += 0.05 * std::complex<double>(e2r, e2i) / (2 * p);
    irr = std::abs(iqB) > 0 ? (float)(-20 * log10(std::abs(iqB))) : 99.0f;
}

// integer path: DC removal only, int16 or int8 (packer words are the top 8 bits)
const void* mmsdr_dev::convertInt(const int16_t* iq, int n, int f) {
    irr = 0;
    const int oi = (int)lrint(dcI), oq = (int)lrint(dcQ);
    if (f == MMSDR_FMT_CS16) {
        sbuf.resize(2 * (size_t)n);
        for (int i = 0; i < n; i++) {
            sbuf[2 * i] = (int16_t)std::clamp(iq[2 * i] - oi, -32768, 32767);
            sbuf[2 * i + 1] = (int16_t)std::clamp(iq[2 * i + 1] - oq, -32768, 32767);
        }
        return sbuf.data();
    }
    bbuf.resize(2 * (size_t)n);
    for (int i = 0; i < n; i++) {
        bbuf[2 * i] = (int8_t)std::clamp((int)lrint((iq[2 * i] - oi) / 256.0), -128, 127);
        bbuf[2 * i + 1] = (int8_t)std::clamp((int)lrint((iq[2 * i + 1] - oq) / 256.0), -128, 127);
    }
    return bbuf.data();
}

// callback mode: split into block_samples pieces, flags only on the first; sync mode: into the ring
void mmsdr_dev::deliver(const void* out, int n, int f, uint32_t flags) {
    mmsdr_meta m{};
    m.struct_size = sizeof m; m.sample_index = sampleIndex; m.flags = flags; m.freq_hz = metaFreq; m.sample_rate = metaFs;
    m.gain_idx = core->getGain(); m.gain_db = 3.0f * m.gain_idx; m.dbfs_to_dbm = NAN;
    if (!cb) { pushSync(out, n, m); return; }
    const size_t bp = bytesPer(f);
    for (int off = 0; off < n; off += (int)blockMax) {
        const int k = std::min<int>((int)blockMax, n - off);
        mmsdr_rx_block blk{ (const uint8_t*)out + off * bp, (uint32_t)k, (mmsdr_format)f, &m };
        inCb = this; cb(cbUser, &blk); inCb = nullptr;
        m.sample_index += k; m.flags = 0;
    }
}

void mmsdr_dev::pushSync(const void* data, int n, const mmsdr_meta& m) {
    std::lock_guard<std::mutex> l(syncMtx);
    if (rw - rr + n > cap) { dropPending = true; dropFlags |= m.flags; return; }   // drop newest; next kept block carries the flags
    mmsdr_meta mm = m;
    if (dropPending) { mm.flags |= MMSDR_META_OVERFLOW | MMSDR_META_DISCONTINUITY | dropFlags; dropPending = false; dropFlags = 0; }
    // a new mark whenever meta changes, flags are set, or the index jumped (core overflow)
    const bool changed = mm.flags || mm.freq_hz != lastMeta.freq_hz || mm.sample_rate != lastMeta.sample_rate || mm.gain_idx != lastMeta.gain_idx;
    if (marks.empty() || changed) marks.push_back({ rw, mm, false });
    else if (mm.sample_index != marks.back().m.sample_index + (rw - marks.back().at)) marks.push_back({ rw, mm, false });
    lastMeta = mm;
    const uint8_t* s = (const uint8_t*)data;
    for (uint64_t k = 0; k < (uint64_t)n;) {
        const uint64_t at = (rw + k) % cap, run = std::min<uint64_t>(n - k, cap - at);
        memcpy(&ring[at * bps], s + k * bps, run * bps);
        k += run;
    }
    rw += n;
    syncCv.notify_all();
}

// ============================================================================ control path
int mmsdr_dev::retune(mmsdr_apply_result* res) {
    // mtx held. Applies freq, rate, format, ppb as stored.
    const double t0 = now();
    core->setWide(rateReq); core->setPacked(fmt == MMSDR_FMT_CS8); core->setHopEnabled(hopOn);
    if (!core->streaming()) { fillRes(res, MMSDR_APPLY_NONE, freq, rateReq > 0 ? (double)rateReq : MM8108::FS, 0, 0); return 0; }
    if (int r = core->retune(freq)) {
        streamOn = false;
        { std::lock_guard<std::mutex> l(syncMtx); syncCv.notify_all(); }
        return fail(r, "retune: " + core->error());
    }
    curFreq = core->synthHz(); curFs = core->sampleRate();
    const bool hop = core->lastTuneWasHop();
    if (hop) {                                             // hopMark was sampled right after the LO write
        std::lock_guard<std::mutex> l(markMtx);
        hopPending = true; hopMark = core->hopSample(); hopFreq = curFreq; hopFs = curFs;
    }
    const double el = now() - t0;
    logf(MMSDR_LOG_INFO, "%s %.6f MHz, %.4f MS/s (%.0f ms)", hop ? "hopped to" : "booted at", curFreq / 1e6, curFs / 1e6, 1e3 * el);
    fillRes(res, hop ? MMSDR_APPLY_HOP : MMSDR_APPLY_REBOOT, curFreq, curFs, core->lastRetries(), el);
    return 0;
}

// auto gain: int16 clipping at the rails sprays broadband junk (strong 869+ MHz cellular at full gain). Every step
// re-holds the DC servo, so keep it calm: back off 2 steps (~6 dB) on >= 8 clips per 250 ms tick, creep up 1 step
// only after 5 s below -12 dBFS peak and >= 15 s since the last back-off; never above the set index
void mmsdr_dev::agcTick() {
    const int pk = agcPeak.exchange(0);
    const uint64_t cl = core->stats().clipped, dcl = cl - clipLast;
    clipLast = cl;
    if (agcMode == MMSDR_AGC_OFF) return;
    sinceBack++;
    if (dcl >= 8 && agc > 0) {
        agc = std::max(0, agc - 2); quiet = 0; sinceBack = 0;
        core->setGain(agc); gainEpoch++; agcPeak = 0;
    }
    else if (pk < 8192 && agc < userGain && ++quiet >= 20 && sinceBack >= 60) {
        agc++; quiet = 0;
        core->setGain(agc); gainEpoch++; agcPeak = 0;
    }
    else if (pk >= 8192) quiet = 0;
}

void mmsdr_dev::ctlLoop() {
    for (;;) {
        Q job;
        {
            std::unique_lock<std::mutex> l(qMtx);
            qCv.wait_for(l, std::chrono::milliseconds(250), [&] { return quit || q.any(); });
            if (quit) return;
            job = q; q = Q();
        }
        std::lock_guard<std::recursive_mutex> g(mtx);
        if (!core->streaming()) continue;
        try {
            if (!job.any()) { agcTick(); continue; }
            if (job.tune) retune(nullptr);
            if (job.dc) core->setDcMode((MM8108::DcMode)(int)dcMode);
            if (job.gain) { agc = userGain; quiet = 0; core->setGain(userGain); gainEpoch++; agcPeak = 0; }
        }
        catch (const std::exception& e) { logf(MMSDR_LOG_WARN, "control thread: %s", e.what()); }
    }
}

// ============================================================================ C ABI (linkage comes from the extern "C" declarations in mmsdr.h)

// ---- version, errors, logging
uint32_t mmsdr_api_version(void) { return (MMSDR_API_MAJOR << 16) | MMSDR_API_MINOR; }
const char* mmsdr_version_string(void) { return "0.1.0+" MMSDR_GIT; }

const char* mmsdr_strerror(int e) {
    switch (e) {
    case MMSDR_OK: return "ok";
    case MMSDR_ERR_INVALID: return "invalid argument";
    case MMSDR_ERR_NO_DEVICE: return "no device";
    case MMSDR_ERR_ACCESS: return "device busy or access denied";
    case MMSDR_ERR_USB: return "USB transfer failed";
    case MMSDR_ERR_NEEDS_REPLUG: return "chip wedged: replug the module";
    case MMSDR_ERR_FIRMWARE: return "firmware or board config problem";
    case MMSDR_ERR_BOOT: return "boot failed";
    case MMSDR_ERR_CMD: return "firmware command failed";
    case MMSDR_ERR_PARK: return "UPHY did not park";
    case MMSDR_ERR_TIMEOUT: return "timeout";
    case MMSDR_ERR_RANGE: return "out of range";
    case MMSDR_ERR_STATE: return "wrong state";
    case MMSDR_ERR_UNSUPPORTED: return "not supported in this build";
    case MMSDR_ERR_POLICY: return "blocked by debug access policy";
    case MMSDR_ERR_NOMEM: return "out of memory";
    case MMSDR_ERR_OVERFLOW: return "overflow";
    default: return "unknown error";
    }
}

const char* mmsdr_last_error_detail(mmsdr_dev* dev) { return errDev == dev ? errMsg.c_str() : ""; }

void mmsdr_set_log(mmsdr_log_cb cb, void* user, mmsdr_log_level max_level) {
    std::lock_guard<std::mutex> l(logMtx);
    logCb = cb; logUser = user; logMax = max_level;
}

// ---- discovery and open
static void fillInfo(mmsdr_devinfo* o, const MM8108::DevInfo& d, bool busy) {
    if (!o || !sized(o)) return;
    mmsdr_devinfo v{};
    v.struct_size = std::min(o->struct_size, sizeof v);
    snprintf(v.serial, sizeof v.serial, "%s", d.serial.c_str());
    snprintf(v.uid, sizeof v.uid, "%s", d.uid.c_str());
    snprintf(v.usb_path, sizeof v.usb_path, "%s", d.path.c_str());
    v.vid = 0x325b; v.pid = 0x8100;
    v.chip = (d.chipId & 0xffff) == 0x809 ? MMSDR_CHIP_MM8108B2 : MMSDR_CHIP_UNKNOWN;
    v.busy = busy;
    memcpy(o, &v, v.struct_size);
}

int mmsdr_list(mmsdr_devinfo* out, int max) {
    if (max > 0 && !out) return MMSDR_ERR_INVALID;
    const std::vector<MM8108::DevInfo> v = MM8108::list(openPaths());
    for (int i = 0; i < (int)v.size() && i < max; i++) {
        MM8108::DevInfo d = v[i];
        const bool busy = d.busy;
        if (busy) {                                        // ours: serve the registry's copy (a reboot rewrites DevInfo under mtx)
            std::lock_guard<std::mutex> l(regMtx);
            auto it = openDevs.find(d.path);
            if (it != openDevs.end()) { std::lock_guard<std::recursive_mutex> g(it->second->mtx); d = it->second->core->info(); }
        }
        fillInfo(&out[i], d, busy);
    }
    return (int)v.size();
}

int mmsdr_open(mmsdr_dev** devp, const mmsdr_open_args* args) {
    if (!devp) return MMSDR_ERR_INVALID;
    *devp = nullptr;
    if (args && !sized(args)) return failNoDev(MMSDR_ERR_INVALID, "open_args.struct_size not set");
    auto str = [&](const char* const* field, size_t end) -> std::string { return args && args->struct_size >= end && *field ? *field : ""; };
    const std::string sel = args ? str(&args->select, offsetof(mmsdr_open_args, select) + sizeof(char*)) : "";
    std::string fw = args ? str(&args->firmware_path, offsetof(mmsdr_open_args, firmware_path) + sizeof(char*)) : "";
    std::string bcf = args ? str(&args->bcf_path, offsetof(mmsdr_open_args, bcf_path) + sizeof(char*)) : "";
    const std::string dir = args ? str(&args->fw_dir, offsetof(mmsdr_open_args, fw_dir) + sizeof(char*)) : "";
    std::string cc = args ? str(&args->country, offsetof(mmsdr_open_args, country) + sizeof(char*)) : "";
    const int32_t ppb = args && HAS(args, freq_correction_ppb) ? args->freq_correction_ppb : 0;
    if (cc.empty()) cc = "US";

    // select: usb_path ("1-4.2") or uid (12 hex digits)
    std::string path;
    if (sel.find('-') != std::string::npos) path = sel;
    else if (!sel.empty()) {
        for (auto& d : MM8108::list(openPaths())) if (d.uid == sel) path = d.path;
        if (path.empty()) return failNoDev(MMSDR_ERR_NO_DEVICE, "no free MM8108 with uid " + sel);
    }

    // firmware files: fw_dir, $MMSDR_FW_DIR, the directory holding mmsdr.dll, then the working directory
    std::vector<std::string> dirs;
    if (!dir.empty()) dirs.push_back(dir);
    if (const char* e = getenv("MMSDR_FW_DIR")) dirs.push_back(e);
    dirs.push_back(dllDir()); dirs.push_back(".");
    auto find = [&](const std::vector<std::string>& names) -> std::string {
        for (auto& d : dirs) for (auto& n : names) if (fileExists(d + "/" + n)) return d + "/" + n;
        return "";
    };
    if (fw.empty()) fw = find({ "mm8108b2-rl.bin" });
    if (fw.empty() || !fileExists(fw)) return failNoDev(MMSDR_ERR_FIRMWARE, "firmware not found (set firmware_path, fw_dir or MMSDR_FW_DIR)");

    std::unique_ptr<mmsdr_dev> d(new mmsdr_dev());
    mmsdr_dev* raw = d.get();
    d->core = std::make_unique<MM8108>([raw](const int16_t* iq, int n, uint64_t pos) { raw->onBlock(iq, n, pos); });
    if (int r = d->core->openDevice(path)) return failNoDev(r, d->core->error());
    if (bcf.empty()) {
        const int board = d->core->info().board;           // 0x0807 = MF15457; unknown when fw was already running
        bcf = find({ "bcf_mf15457.bin", "bcf/bcf_mf15457.bin" });
        if (board >= 0 && board != 0x0807) logf(MMSDR_LOG_WARN, "board type 0x%04x unknown: trying bcf_mf15457.bin", board);
    }
    if (bcf.empty() || !fileExists(bcf)) return failNoDev(MMSDR_ERR_FIRMWARE, "board config not found (set bcf_path)");
    d->fw = fw; d->bcf = bcf;
    d->core->setFiles(fw, bcf, cc); d->core->setPpb(ppb);
    d->ctl = std::thread(&mmsdr_dev::ctlLoop, raw);
    { std::lock_guard<std::mutex> l(regMtx); openDevs[d->core->info().path] = raw; }
    logf(MMSDR_LOG_INFO, "opened %s uid %s", d->core->info().path.c_str(), d->core->info().uid.c_str());
    *devp = d.release();
    return 0;
}

int mmsdr_boot(mmsdr_dev* d) {
    if (!d) return MMSDR_ERR_INVALID;
    if (inCb) return d->fail(MMSDR_ERR_STATE, "mmsdr_boot from the RX callback");
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    if (d->core->streaming()) return d->fail(MMSDR_ERR_STATE, "streaming");
    if (int r = d->core->bootOnly()) return d->fail(r, "boot: " + d->core->error());
    return 0;
}

void mmsdr_close(mmsdr_dev* d) {
    if (!d) return;
    if (inCb) { logf(MMSDR_LOG_ERROR, "mmsdr_close from the RX callback ignored"); return; }
    { std::lock_guard<std::mutex> l(d->qMtx); d->quit = true; }
    d->qCv.notify_all();
    if (d->ctl.joinable()) d->ctl.join();
    { std::lock_guard<std::recursive_mutex> g(d->mtx); d->core->stop(); }
    d->streamOn = false;
    { std::lock_guard<std::mutex> l(regMtx); openDevs.erase(d->core->info().path); }
    {   // wake every blocked rx_read and wait for them to leave before the handle goes away
        std::unique_lock<std::mutex> l(d->syncMtx);
        d->closing = true; d->syncCv.notify_all();
        d->syncCv.wait(l, [&] { return d->readers == 0; });
    }
    delete d;
}

int mmsdr_get_info(mmsdr_dev* d, mmsdr_devinfo* out) {
    if (!d || !out || out->struct_size < sizeof(mmsdr_devinfo)) return MMSDR_ERR_INVALID;
    fillInfo(out, d->core->info(), false);
    return 0;
}

// ---- tuning
int mmsdr_get_freq_range(mmsdr_dev* d, mmsdr_range* o) {
    if (!d || !o) return MMSDR_ERR_INVALID;
    *o = { MM8108::FMIN, MM8108::FMAX, 32e6 / (1 << 24) };
    return 0;
}

int mmsdr_set_freq(mmsdr_dev* d, double hz, mmsdr_apply_result* res) {
    if (!d) return MMSDR_ERR_INVALID;
    if (!d->core->knobs.unclamp && (hz < MM8108::FMIN || hz > MM8108::FMAX)) return d->fail(MMSDR_ERR_RANGE, "frequency outside 750-1100 MHz");
    d->freq = hz;
    if (inCb) { d->post(true, false, false); fillRes(res, MMSDR_APPLY_QUEUED, hz, d->curFs, 0, 0); return 0; }
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    return d->retune(res);
}

double mmsdr_get_freq(mmsdr_dev* d) {
    if (!d) return 0;
    return d->core->streaming() ? (double)d->curFreq : (double)d->freq;
}

int mmsdr_set_hop_enabled(mmsdr_dev* d, int on) {
    if (!d) return MMSDR_ERR_INVALID;
    d->hopOn = on != 0; d->core->setHopEnabled(on != 0);
    return 0;
}

int mmsdr_get_rate_range(mmsdr_dev* d, double f, mmsdr_range* o) {
    if (!d || !o) return MMSDR_ERR_INVALID;
    if (f <= 0) f = d->freq;
    if (f > MM8108::F_RESAMP_MAX) { *o = { MM8108::FS, MM8108::FS, 0 }; return 0; }
    // ratio >= 0.5 caps fs at f_ADC = f/64; USB: 9.7 MS/s at 4 B/sample, packer source sags above ~10.3 MS/s
    const double usb = d->fmt == MMSDR_FMT_CS8 ? 10.3e6 : 9.7e6;
    *o = { MM8108::FS, std::min(f / 64, usb), 0 };
    return 0;
}

int mmsdr_set_sample_rate(mmsdr_dev* d, double fs, mmsdr_apply_result* res) {
    if (!d) return MMSDR_ERR_INVALID;
    if (fs != 0 && fs != MM8108::FS) {
        mmsdr_range r; mmsdr_get_rate_range(d, d->freq, &r);
        if (fs < r.min || fs > r.max + 1) return d->fail(MMSDR_ERR_RANGE, "sample rate outside " + std::to_string(r.min) + "-" + std::to_string(r.max));
    }
    d->rateReq = fs == MM8108::FS ? 0 : fs;
    if (inCb) { d->post(true, false, false); fillRes(res, MMSDR_APPLY_QUEUED, d->curFreq, fs ? fs : MM8108::FS, 0, 0); return 0; }
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    return d->retune(res);
}

double mmsdr_get_sample_rate(mmsdr_dev* d) {
    if (!d) return 0;
    if (d->core->streaming()) return d->curFs;
    return d->rateReq > 0 ? (double)d->rateReq : MM8108::FS;
}

int mmsdr_set_format(mmsdr_dev* d, mmsdr_format f, mmsdr_apply_result* res) {
    if (!d || f < MMSDR_FMT_CS16 || f > MMSDR_FMT_CF32) return MMSDR_ERR_INVALID;
    auto repack = [&] { return (f == MMSDR_FMT_CS8) != (d->fmt == MMSDR_FMT_CS8); };   // CS8 toggles the UPHY packer: reboot
    if (inCb) {                                            // callback mode, streaming by definition: never block here
        const bool rp = repack(); d->fmt = f;
        if (!rp) { d->fmtEpoch++; fillRes(res, MMSDR_APPLY_LIVE, d->curFreq, d->curFs, 0, 0); return 0; }
        d->post(true, false, false); fillRes(res, MMSDR_APPLY_QUEUED, d->curFreq, d->curFs, 0, 0);
        return 0;
    }
    std::lock_guard<std::recursive_mutex> g(d->mtx);       // a sync stream fixes bps at start: decide under the same lock rx_start holds
    const bool streaming = d->streamOn;
    if (streaming && !d->cb) return d->fail(MMSDR_ERR_STATE, "format is fixed while a sync stream runs");
    const bool rp = repack(); d->fmt = f;
    if (!streaming) { fillRes(res, MMSDR_APPLY_NONE, d->freq, mmsdr_get_sample_rate(d), 0, 0); return 0; }
    if (!rp) { d->fmtEpoch++; fillRes(res, MMSDR_APPLY_LIVE, d->curFreq, d->curFs, 0, 0); return 0; }
    return d->retune(res);
}

// ---- gain
int mmsdr_get_gain_range(mmsdr_dev* d, int* lo, int* hi) {
    if (!d || !lo || !hi) return MMSDR_ERR_INVALID;
    *lo = 0; *hi = MM8108::GAIN_MAX;
    return 0;
}

int mmsdr_set_gain(mmsdr_dev* d, int idx) {
    if (!d) return MMSDR_ERR_INVALID;
    if (idx < 0 || idx > MM8108::GAIN_MAX) return d->fail(MMSDR_ERR_RANGE, "gain index outside 0-16");
    d->userGain = idx;
    if (inCb) { d->post(false, true, false); return 0; }
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    try { d->agc = idx; d->quiet = 0; d->core->setGain(idx); d->gainEpoch++; d->agcPeak = 0; }
    catch (const MMError& e) { return d->fail(e.code, std::string("set_gain: ") + e.what()); }
    return 0;
}

int mmsdr_get_gain(mmsdr_dev* d) { return d ? d->core->getGain() : MMSDR_ERR_INVALID; }

int mmsdr_gain_db(mmsdr_dev* d, int idx, double* db, mmsdr_cal_source* src) {
    if (!d || !db || idx < 0 || idx > MM8108::GAIN_MAX) return MMSDR_ERR_INVALID;
    *db = 3.0 * idx;
    if (src) *src = MMSDR_CAL_NOMINAL;
    return 0;
}

int mmsdr_gain_idx_for_db(mmsdr_dev* d, double db) {
    if (!d) return MMSDR_ERR_INVALID;
    return std::clamp((int)lround(db / 3.0), 0, MM8108::GAIN_MAX);
}

int mmsdr_set_agc(mmsdr_dev* d, mmsdr_agc m) {
    if (!d || (m != MMSDR_AGC_OFF && m != MMSDR_AGC_CLIP_GUARD)) return MMSDR_ERR_INVALID;
    d->agcMode = m; d->post(false, true, false);           // restart from the set index
    return 0;
}
mmsdr_agc mmsdr_get_agc(mmsdr_dev* d) { return d ? (mmsdr_agc)(int)d->agcMode : MMSDR_AGC_OFF; }

// ---- DC and IQ
int mmsdr_set_dc_mode(mmsdr_dev* d, mmsdr_dc m) {
    if (!d || m < MMSDR_DC_HOLD || m > MMSDR_DC_OFF) return MMSDR_ERR_INVALID;
    d->dcMode = m;
    if (inCb) { d->post(false, false, true); return 0; }
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    try { d->core->setDcMode((MM8108::DcMode)m); }
    catch (const MMError& e) { return d->fail(e.code, std::string("set_dc_mode: ") + e.what()); }
    return 0;
}
mmsdr_dc mmsdr_get_dc_mode(mmsdr_dev* d) { return d ? (mmsdr_dc)(int)d->dcMode : MMSDR_DC_HOLD; }

int mmsdr_recal_dc(mmsdr_dev* d) {
    if (!d) return MMSDR_ERR_INVALID;
    if (inCb) return d->fail(MMSDR_ERR_STATE, "recal_dc from the RX callback");
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    if (!d->core->streaming()) return d->fail(MMSDR_ERR_STATE, "recal_dc needs a running stream");
    try {
        const std::string e = d->core->recalDc();
        if (!e.empty()) return d->fail(MMSDR_ERR_CMD, e);
    }
    catch (const MMError& e) { return d->fail(e.code, std::string("recal_dc: ") + e.what()); }
    return 0;
}

int mmsdr_set_iq_correction(mmsdr_dev* d, int on) {
    if (!d) return MMSDR_ERR_INVALID;
    d->iqOn = on != 0; d->iqReset = true;
    return 0;
}
int mmsdr_get_iq_correction(mmsdr_dev* d) { return d ? (int)d->iqOn : 0; }

// ---- settings discovery
static const mmsdr_setting_info SETTINGS[] = {
    { sizeof(mmsdr_setting_info), "agc", "Auto gain", "Host clip guard: backs off on int16 clipping, creeps back up, never above the set gain", MMSDR_SETTING_ENUM, "off", "off,clip_guard", MMSDR_APPLY_LIVE },
    { sizeof(mmsdr_setting_info), "iq_correction", "IQ image correction", "Blind IQ imbalance correction on the host", MMSDR_SETTING_BOOL, "false", nullptr, MMSDR_APPLY_LIVE },
    { sizeof(mmsdr_setting_info), "passband_eq", "Flatten analog rolloff", "31-tap FIR inverse of the measured rolloff, boost capped at 10 dB", MMSDR_SETTING_BOOL, "false", nullptr, MMSDR_APPLY_LIVE },
    { sizeof(mmsdr_setting_info), "dc_mode", "DC offset", "hold: fw servo frozen + host residual tracker; servo: fw 80 kHz notch; off: raw", MMSDR_SETTING_ENUM, "hold", "hold,servo,off", MMSDR_APPLY_LIVE },
    { sizeof(mmsdr_setting_info), "hop", "Fast retune", "Register-only PLL hop (~60 ms) instead of a reboot (~0.75 s)", MMSDR_SETTING_BOOL, "true", nullptr, MMSDR_APPLY_NONE },
    { sizeof(mmsdr_setting_info), "format", "Sample format", "cs16 native; cs8 = UPHY 8-bit packer (reboot); cf32 converted on the host", MMSDR_SETTING_ENUM, "cs16", "cs16,cs8,cf32", MMSDR_APPLY_REBOOT },
    { sizeof(mmsdr_setting_info), "freq_correction_ppb", "Frequency correction (ppb)", "XO error; corrects LO and sample rate together", MMSDR_SETTING_INT, "0", nullptr, MMSDR_APPLY_HOP },
};

int mmsdr_list_settings(mmsdr_dev* d, mmsdr_setting_info* out, int max) {
    const int n = (int)(sizeof SETTINGS / sizeof SETTINGS[0]);
    if (!d || (max > 0 && !out)) return MMSDR_ERR_INVALID;
    for (int i = 0; i < n && i < max; i++) {
        if (out[i].struct_size && !sized(&out[i])) return MMSDR_ERR_INVALID;
        const size_t ss = out[i].struct_size ? std::min(out[i].struct_size, sizeof(mmsdr_setting_info)) : sizeof(mmsdr_setting_info);
        memcpy((char*)&out[i] + sizeof(size_t), (const char*)&SETTINGS[i] + sizeof(size_t), ss - sizeof(size_t));
        out[i].struct_size = ss;
    }
    return n;
}

int mmsdr_write_setting(mmsdr_dev* d, const char* key, const char* val, mmsdr_apply_result* res) {
    if (!d || !key || !val) return MMSDR_ERR_INVALID;
    const std::string k = key, v = val;
    const int b = parseBool(val);
    auto live = [&] { fillRes(res, MMSDR_APPLY_LIVE, mmsdr_get_freq(d), mmsdr_get_sample_rate(d), 0, 0); return 0; };
    if (k == "agc") {
        const int m = v == "clip_guard" || b == 1 ? 1 : v == "off" || b == 0 ? 0 : -1;
        if (m < 0) return MMSDR_ERR_INVALID;
        mmsdr_set_agc(d, (mmsdr_agc)m);
        return live();
    }
    if (k == "iq_correction") {
        if (b < 0) return MMSDR_ERR_INVALID;
        mmsdr_set_iq_correction(d, b);
        return live();
    }
    if (k == "passband_eq") {
        if (b < 0) return MMSDR_ERR_INVALID;
        mmsdr_set_passband_eq(d, b);
        return live();
    }
    if (k == "hop") {
        if (b < 0) return MMSDR_ERR_INVALID;
        mmsdr_set_hop_enabled(d, b);
        fillRes(res, MMSDR_APPLY_NONE, mmsdr_get_freq(d), mmsdr_get_sample_rate(d), 0, 0);
        return 0;
    }
    if (k == "dc_mode") {
        const int m = enumIndex(v, { "hold", "servo", "off" });
        if (m < 0) return MMSDR_ERR_INVALID;
        const int r = mmsdr_set_dc_mode(d, (mmsdr_dc)m);
        return r ? r : live();
    }
    if (k == "format") {
        const int f = enumIndex(v, { "cs16", "cs8", "cf32" });
        if (f < 0) return MMSDR_ERR_INVALID;
        return mmsdr_set_format(d, (mmsdr_format)f, res);
    }
    if (k == "freq_correction_ppb") {
        char* e; const long p = strtol(val, &e, 0);
        if (*e) return MMSDR_ERR_INVALID;
        return mmsdr_set_freq_correction_ppb(d, (int32_t)p, res);
    }
    return d->fail(MMSDR_ERR_INVALID, "unknown setting " + k);
}

int mmsdr_read_setting(mmsdr_dev* d, const char* key, char* out, size_t len) {
    if (!d || !key || !out || !len) return MMSDR_ERR_INVALID;
    const std::string k = key;
    std::string v;
    if (k == "agc") v = d->agcMode == MMSDR_AGC_CLIP_GUARD ? "clip_guard" : "off";
    else if (k == "iq_correction") v = d->iqOn ? "true" : "false";
    else if (k == "passband_eq") v = d->eqOn ? "true" : "false";
    else if (k == "hop") v = d->hopOn ? "true" : "false";
    else if (k == "dc_mode") v = DC_NAMES[d->dcMode];
    else if (k == "format") v = FMT_NAMES[d->fmt];
    else if (k == "freq_correction_ppb") v = std::to_string(d->core->getPpb());
    else return d->fail(MMSDR_ERR_INVALID, "unknown setting " + k);
    snprintf(out, len, "%s", v.c_str());
    return 0;
}

// ---- streaming
int mmsdr_rx_start(mmsdr_dev* d, const mmsdr_rx_args* a) {
    if (!d) return MMSDR_ERR_INVALID;
    if (a && !sized(a)) return d->fail(MMSDR_ERR_INVALID, "rx_args.struct_size not set");
    if (inCb) return d->fail(MMSDR_ERR_STATE, "rx_start from the RX callback");
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    if (d->core->streaming()) return d->fail(MMSDR_ERR_STATE, "already streaming");

    // arguments (tail fields optional)
    d->cb = a && HAS(a, callback) ? a->callback : nullptr;
    d->cbUser = a && HAS(a, user) ? a->user : nullptr;
    const uint32_t bs = a && HAS(a, block_samples) ? a->block_samples : 0;
    const uint32_t ms = a && HAS(a, buffer_ms) ? a->buffer_ms : 0;
    const int rt = a && HAS(a, realtime_priority) ? a->realtime_priority : 0;
    d->blockMax = bs ? bs : 16384;
    d->core->knobs.prio = rt < 0 ? 0 : d->prioKnob;        // -1: THREAD_PRIORITY_NORMAL for this run; 0: the usb.prio knob (time-critical)

    // per-run output and AGC state
    d->sampleIndex = 0; d->dcI = d->dcQ = 0; d->dcFast = 0; d->iqB = 0; d->eq.fs = 0;
    d->gainSeen = d->gainEpoch; d->fmtSeen = d->fmtEpoch;
    d->agc = d->userGain; d->quiet = 0; d->sinceBack = 1000; d->clipLast = d->core->stats().clipped;
    {
        std::lock_guard<std::mutex> l(d->syncMtx);
        d->marks.clear(); d->rw = d->rr = 0; d->dropPending = false; d->dropFlags = 0; d->lastMeta = mmsdr_meta{};
        if (!d->cb) {                                      // ring holds at least 4 core blocks so a tiny buffer_ms cannot starve it
            d->bps = bytesPer(d->fmt);
            d->cap = std::max<uint64_t>((uint64_t)((ms ? ms : 250) * 1e-3 * 10e6), 65536);
            try { d->ring.assign(d->cap * d->bps, 0); }
            catch (const std::bad_alloc&) { return d->fail(MMSDR_ERR_NOMEM, "sync ring allocation"); }
        }
        else d->ring.clear();
    }

    // stored settings go to the core; the tune applies them
    d->core->setGain(d->userGain); d->core->setDcMode((MM8108::DcMode)(int)d->dcMode);
    d->core->setWide(d->rateReq); d->core->setPacked(d->fmt == MMSDR_FMT_CS8); d->core->setHopEnabled(d->hopOn);
    const double t0 = now();
    if (int r = d->core->retune(d->freq)) return d->fail(r, "rx_start: " + d->core->error());
    d->curFreq = d->core->synthHz(); d->curFs = d->core->sampleRate(); d->streamOn = true;
    logf(MMSDR_LOG_INFO, "streaming %.6f MHz, %.4f MS/s (%.0f ms)", d->curFreq / 1e6, d->curFs / 1e6, 1e3 * (now() - t0));
    return 0;
}

int mmsdr_rx_stop(mmsdr_dev* d) {
    if (!d) return MMSDR_ERR_INVALID;
    if (inCb) return d->fail(MMSDR_ERR_STATE, "rx_stop from the RX callback");
    { std::lock_guard<std::recursive_mutex> g(d->mtx); d->core->stop(); d->streamOn = false; }
    { std::lock_guard<std::mutex> l(d->syncMtx); d->syncCv.notify_all(); }
    return 0;
}

int mmsdr_rx_is_streaming(mmsdr_dev* d) { return d && d->streamOn; }

int mmsdr_rx_read(mmsdr_dev* d, void* buf, uint32_t max, mmsdr_meta* meta, int timeout_ms) {
    if (!d || !buf || !max) return MMSDR_ERR_INVALID;
    if (d->cb) return d->fail(MMSDR_ERR_STATE, "stream runs in callback mode");
    std::unique_lock<std::mutex> l(d->syncMtx);
    if (d->closing) return d->fail(MMSDR_ERR_STATE, "closing");

    // wait for data; a reboot-retune keeps streamOn, so readers just wait through it
    auto ready = [&] { return d->rw > d->rr || !d->streamOn || d->closing; };
    d->readers++;
    if (timeout_ms < 0) d->syncCv.wait(l, ready);
    else d->syncCv.wait_for(l, std::chrono::milliseconds(timeout_ms), ready);
    d->readers--;
    if (d->closing) { d->syncCv.notify_all(); return d->fail(MMSDR_ERR_STATE, "closing"); }
    if (d->rw == d->rr) return d->streamOn ? 0 : d->fail(MMSDR_ERR_STATE, "not streaming");

    // the read never crosses the next mark, so the front mark's meta describes every sample returned
    while (d->marks.size() > 1 && d->marks[1].at <= d->rr) d->marks.pop_front();
    mmsdr_dev::Mark& m = d->marks.front();
    uint64_t lim = d->rw;
    if (d->marks.size() > 1) lim = std::min(lim, d->marks[1].at);
    const uint64_t n = std::min<uint64_t>(max, lim - d->rr);
    if (meta && sized(meta)) {
        mmsdr_meta v = m.m;
        v.sample_index = m.m.sample_index + (d->rr - m.at);
        v.flags = d->rr == m.at && !m.reported ? m.m.flags : 0;   // flags are reported once, with the mark's first sample
        const size_t ss = std::min(meta->struct_size, sizeof v);
        v.struct_size = ss; memcpy(meta, &v, ss);
    }
    if (d->rr == m.at) m.reported = true;
    for (uint64_t k = 0; k < n;) {
        const uint64_t at = (d->rr + k) % d->cap, run = std::min<uint64_t>(n - k, d->cap - at);
        memcpy((uint8_t*)buf + k * d->bps, &d->ring[at * d->bps], run * d->bps);
        k += run;
    }
    d->rr += n;
    return (int)n;
}

int mmsdr_get_stats(mmsdr_dev* d, mmsdr_stats* o) {
    if (!d || !o || !sized(o)) return MMSDR_ERR_INVALID;
    const MM8108::Stats s = d->core->stats();
    mmsdr_stats v{};
    v.struct_size = std::min(o->struct_size, sizeof v);
    v.samples = s.samples; v.bad_ptr = s.badPtr; v.lapped = s.lapped; v.usb_err = s.usbErr; v.overflow = s.overflow; v.clipped = s.clipped;
    v.hops = s.hops; v.reboots = s.reboots; v.park_retries = s.parkRetries; v.source_gaps = 0;
    v.peak_abs = d->statsPeak.exchange(0); v.dc_epoch = d->core->dcEpoch(); v.boosted_threads = d->core->boostedThreads(); v.iq_irr_db = d->irr;
    memcpy(o, &v, v.struct_size);
    return 0;
}

// ---- sensors and calibration (mostly placeholders)
int mmsdr_get_temperature(mmsdr_dev*, double*) { return MMSDR_ERR_UNSUPPORTED; }
int mmsdr_get_supply_voltage(mmsdr_dev*, double*) { return MMSDR_ERR_UNSUPPORTED; }

int mmsdr_set_freq_correction_ppb(mmsdr_dev* d, int32_t ppb, mmsdr_apply_result* res) {
    if (!d) return MMSDR_ERR_INVALID;
    if (ppb < -500000 || ppb > 500000) return d->fail(MMSDR_ERR_RANGE, "correction outside +-500 ppm");
    d->core->setPpb(ppb);
    if (inCb) { d->post(true, false, false); fillRes(res, MMSDR_APPLY_QUEUED, d->curFreq, d->curFs, 0, 0); return 0; }
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    return d->retune(res);
}
int mmsdr_get_freq_correction_ppb(mmsdr_dev* d, int32_t* ppb) {
    if (!d || !ppb) return MMSDR_ERR_INVALID;
    *ppb = d->core->getPpb();
    return 0;
}
int mmsdr_cal_freq_measure(mmsdr_dev*, double, double, int32_t*) { return MMSDR_ERR_UNSUPPORTED; }

int mmsdr_passband_db(mmsdr_dev* d, double off, double* db) {
    if (!d || !db) return MMSDR_ERR_INVALID;
    *db = rolloffDb(off);
    return std::fabs(off) <= 5e6 ? 0 : MMSDR_ERR_RANGE;    // fit measured over +-4.6 MHz
}
int mmsdr_get_dbfs_to_dbm(mmsdr_dev*, double, int, double*) { return MMSDR_ERR_UNSUPPORTED; }
int mmsdr_set_passband_eq(mmsdr_dev* d, int on) {
    if (!d) return MMSDR_ERR_INVALID;
    d->eqOn = on != 0;
    return 0;
}
int mmsdr_get_passband_eq(mmsdr_dev* d) { return d ? (int)d->eqOn : 0; }

// ---- TX (all placeholders in v0.x)
int mmsdr_tx_get_freq_range(mmsdr_dev*, mmsdr_range*) { return MMSDR_ERR_UNSUPPORTED; }
int mmsdr_tx_get_power_range(mmsdr_dev*, double, mmsdr_range*) { return MMSDR_ERR_UNSUPPORTED; }
int mmsdr_tx_set_freq(mmsdr_dev*, double, mmsdr_apply_result*) { return MMSDR_ERR_UNSUPPORTED; }
int mmsdr_tx_set_power_dbm(mmsdr_dev*, double, double*) { return MMSDR_ERR_UNSUPPORTED; }
int mmsdr_tx_get_state(mmsdr_dev*, mmsdr_txa_state*) { return MMSDR_ERR_UNSUPPORTED; }
int mmsdr_tx_start(mmsdr_dev*, const mmsdr_tx_args*) { return MMSDR_ERR_UNSUPPORTED; }
int mmsdr_tx_write(mmsdr_dev*, const int16_t*, uint32_t, int) { return MMSDR_ERR_UNSUPPORTED; }
int mmsdr_tx_stop(mmsdr_dev*) { return MMSDR_ERR_UNSUPPORTED; }

// ============================================================================ debug interface
static const std::pair<uint32_t, uint32_t> DEFAULT_ALLOW[] = {
    // registers the fw (or our port of it) touches; reads of these classes are capped at 60 B
    { 0x1d00, 0x1d40 }, { 0x2000, 0x2e00 }, { 0x4000, 0x4050 }, { 0x4100, 0x4104 },
    { 0x401300, 0x401400 }, { 0x401930, 0x401938 }, { 0x402000, 0x402080 }, { 0x403000, 0x403010 }, { 0x405000, 0x405060 },
    { 0x406000, 0x406010 }, { 0x406040, 0x406044 }, { 0x406500, 0x406548 }, { 0x409000, 0x409080 }, { 0x409400, 0x409470 },
};

static int policy(mmsdr_dev* d, uint32_t a, uint32_t n, bool rd) {
    const uint64_t e = (uint64_t)a + n;
    const bool reg = a < 0x10000 || (a >= 0x400000 && a < 0x410000);
    if (reg && rd && n > 60) return d->fail(MMSDR_ERR_POLICY, "register reads are capped at 60 B (a 256 B read of 0x409000 wedged the chip)");
    // fw RAM: MAC 0x100000+192K and 0x200000+704K, PHY IMEM/DMEM
    if (!reg && ((a >= 0x100000 && e <= 0x130000) || (a >= 0x200000 && e <= 0x2b0000) || (a >= 0x500000 && e <= 0x560000))) return 0;
    for (auto& r : DEFAULT_ALLOW) if (a >= r.first && e <= r.second) return 0;
    for (auto& r : d->allow) if (a >= r.first && e <= r.second) return 0;
    char b[96]; snprintf(b, sizeof b, "0x%06x+%u is outside the allowlist (mmsdr_debug_allow_range)", a, n);
    return d->fail(MMSDR_ERR_POLICY, b);
}

// common prologue of every blocking debug call: valid handle, not inside the callback, under mtx, exceptions to codes
template <class F> static int guarded(mmsdr_dev* d, const char* what, F fn) {
    if (!d) return MMSDR_ERR_INVALID;
    if (inCb) return d->fail(MMSDR_ERR_STATE, std::string(what) + " from the RX callback");
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    try { return fn(); }
    catch (const MMError& e) { return d->fail(e.code, std::string(what) + ": " + e.what()); }
    catch (const std::exception& e) { return d->fail(MMSDR_ERR_USB, std::string(what) + ": " + e.what()); }
}

int mmsdr_peek32(mmsdr_dev* d, uint32_t a, uint32_t* v) {
    if (!v) return MMSDR_ERR_INVALID;
    return guarded(d, "peek32", [&] {
        if (int r = policy(d, a, 4, true)) return r;
        if (int r = d->core->ensureOpen()) return r;
        *v = d->core->r32(a);
        return 0;
    });
}
int mmsdr_poke32(mmsdr_dev* d, uint32_t a, uint32_t v) {
    return guarded(d, "poke32", [&] {
        if (int r = policy(d, a, 4, false)) return r;
        if (int r = d->core->ensureOpen()) return r;
        d->core->w32(a, v);
        return 0;
    });
}
int mmsdr_peek(mmsdr_dev* d, uint32_t a, void* buf, uint32_t n) {
    if (!buf) return MMSDR_ERR_INVALID;
    return guarded(d, "peek", [&] {
        if (int r = policy(d, a, n, true)) return r;
        if (int r = d->core->ensureOpen()) return r;
        const std::vector<uint8_t> v = d->core->peek(a, n);
        memcpy(buf, v.data(), n);
        return 0;
    });
}
int mmsdr_debug_allow_range(mmsdr_dev* d, uint32_t lo, uint32_t hi) {
    if (!d || hi <= lo) return MMSDR_ERR_INVALID;
    if (inCb) return d->fail(MMSDR_ERR_STATE, "allow_range from the RX callback");
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    d->allow.push_back({ lo, hi });
    return 0;
}

// ---- expert knobs
namespace {
struct Knob { const char* key; const char* def; const char* help; };
const Knob KNOBS[] = {
    { "unclamp", "0", "allow tuning outside 750-1100 MHz (VCO range mapping)" },
    { "nokdco", "0", "skip the host KDCO cal on hops (stock band seeds)" },
    { "nohop", "0", "every retune reboots" },
    { "band54", "", "force 0x402054[15:6] above 1 GHz or below 750 MHz" },
    { "cc", "", "force the signed coarse bank 0x402058[16:10] above 1 GHz or below 750 MHz" },
    { "crashbcast", "0", "keep UPHY's ASSERT doorbell broadcast (halts LPHY)" },
    { "raw", "0", "bypass host DSP (DC tracker, IQ, EQ)" },
    { "usb.ch", "4096", "words per ring read" },
    { "usb.k", "2", "data reads per pointer read" },
    { "usb.depth", "4", "reads in flight" },
    { "usb.prio", "15", "reader thread priority" },
    { "usb.wait_us", "100", "event wait" },
};

std::string knobValue(mmsdr_dev* d, const std::string& k) {
    MM8108::Knobs& kn = d->core->knobs;
    if (k == "unclamp") return std::to_string((int)kn.unclamp);
    if (k == "nokdco") return std::to_string((int)kn.nokdco);
    if (k == "nohop") return std::to_string((int)kn.nohop);
    if (k == "crashbcast") return std::to_string((int)kn.crashbcast);
    if (k == "raw") return std::to_string((int)d->raw);
    if (k == "band54") return kn.band54 < 0 ? "" : std::to_string(kn.band54);
    if (k == "cc") return kn.cc == MM8108::CC_OFF ? "" : std::to_string(kn.cc);
    if (k == "usb.ch") return std::to_string(kn.ch);
    if (k == "usb.k") return std::to_string(kn.k);
    if (k == "usb.depth") return std::to_string(kn.depth);
    if (k == "usb.prio") return std::to_string(kn.prio);
    if (k == "usb.wait_us") return std::to_string(kn.waitUs);
    return "?";
}
}   // namespace

int mmsdr_debug_set(mmsdr_dev* d, const char* key, const char* val) {
    if (!d || !key || !val) return MMSDR_ERR_INVALID;
    const std::string k = key;
    std::string v = val;
    MM8108::Knobs& kn = d->core->knobs;
    const char* def = nullptr;
    for (auto& x : KNOBS) if (k == x.key) def = x.def;
    if (!def) return d->fail(MMSDR_ERR_INVALID, "unknown debug key " + k);
    if (v.empty()) v = def;                                // "" restores the default
    auto num = [&](int dflt) { return v.empty() ? dflt : (int)strtol(v.c_str(), nullptr, 0); };
    const bool b = num(0) != 0;
    if (k == "unclamp") kn.unclamp = b;
    else if (k == "nokdco") kn.nokdco = b;
    else if (k == "nohop") kn.nohop = b;
    else if (k == "crashbcast") kn.crashbcast = b;
    else if (k == "raw") d->raw = b;
    else if (k == "band54") kn.band54 = num(-1);
    else if (k == "cc") kn.cc = v.empty() ? MM8108::CC_OFF : num(0);
    else if (k == "usb.ch") kn.ch = num(4096);
    else if (k == "usb.k") kn.k = num(2);
    else if (k == "usb.depth") kn.depth = num(4);
    else if (k == "usb.prio") kn.prio = d->prioKnob = num(15);
    else if (k == "usb.wait_us") kn.waitUs = num(100);
    return 0;
}

int mmsdr_debug_get(mmsdr_dev* d, const char* key, char* out, size_t len) {
    if (!d || !key || !out || !len) return MMSDR_ERR_INVALID;
    for (auto& x : KNOBS) {
        if (std::string(key) != x.key) continue;
        snprintf(out, len, "%s", knobValue(d, key).c_str());
        return 0;
    }
    return d->fail(MMSDR_ERR_INVALID, std::string("unknown debug key ") + key);
}

int mmsdr_debug_list(mmsdr_dev* d, char* out, size_t len) {
    if (!d || !out || !len) return MMSDR_ERR_INVALID;
    std::string s;
    for (auto& x : KNOBS) s += std::string(x.key) + "=" + knobValue(d, x.key) + "  # " + x.help + "\n";
    snprintf(out, len, "%s", s.c_str());
    return s.size() < len ? 0 : MMSDR_ERR_RANGE;
}

// ---- raw firmware commands and boot hooks
int mmsdr_cmd(mmsdr_dev* d, uint16_t id, const void* payload, uint32_t len, void* resp, uint32_t* resp_len, int timeout_ms) {
    if (len && !payload) return MMSDR_ERR_INVALID;
    return guarded(d, "cmd", [&] {
        std::vector<uint8_t> pl((const uint8_t*)payload, (const uint8_t*)payload + len), r;
        const int st = d->core->cmd(id, pl, (timeout_ms > 0 ? timeout_ms : 1000) / 1e3, &r);
        if (resp && resp_len) {
            const uint32_t k = std::min<uint32_t>(*resp_len, (uint32_t)r.size());
            memcpy(resp, r.data(), k); *resp_len = k;
        }
        else if (resp_len) *resp_len = 0;
        return st;
    });
}

int mmsdr_generic_param(mmsdr_dev* d, uint32_t id, int set, uint32_t* value) {
    if (!value) return MMSDR_ERR_INVALID;
    uint32_t q[4] = { id, set ? 0u : 1u, 0, set ? *value : 0 };
    uint32_t r[2] = { 0, 0 }, rl = sizeof r;
    const int st = mmsdr_cmd(d, 0x003E, q, sizeof q, r, &rl, 1000);
    if (st == 0 && rl >= 8) *value = r[1];
    return st;
}

int mmsdr_lphy_msg(mmsdr_dev*, uint16_t, const uint32_t*, int, int) { return MMSDR_ERR_UNSUPPORTED; }

int mmsdr_debug_set_hook(mmsdr_dev* d, mmsdr_hook_cb cb, void* user) {
    if (!d) return MMSDR_ERR_INVALID;
    if (inCb) return d->fail(MMSDR_ERR_STATE, "set_hook from the RX callback");
    std::lock_guard<std::recursive_mutex> g(d->mtx);
    d->hookCb = cb; d->hookUser = user;
    if (cb) d->core->setHook([d](int p) { d->hookCb(d->hookUser, d, (mmsdr_hook_point)p); });
    else d->core->setHook(nullptr);
    return 0;
}

int mmsdr_debug_pll_state(mmsdr_dev* d, mmsdr_pll_state* o) {
    if (!o || !sized(o)) return MMSDR_ERR_INVALID;
    return guarded(d, "pll_state", [&] {
        if (!d->core->streaming()) return d->fail(MMSDR_ERR_STATE, "pll_state needs a running stream");
        MM8108& c = *d->core;
        mmsdr_pll_state v{};
        v.struct_size = std::min(o->struct_size, sizeof v);
        const uint32_t r50 = c.r32(0x402050);
        c.w32(0x402050, (r50 & 0xcfffffff) | 0x20000000);   // [29:28]=2 selects the tracking word
        v.track_word = c.r32(0x402068) & 0x1ff;
        c.w32(0x402050, r50);
        v.kdco = c.r32(0x40205c);
        v.band = (c.r32(0x402054) >> 6) & 0x1f;
        const uint32_t r58 = c.r32(0x402058), r64 = c.r32(0x402064);
        const int cc = (int)((r58 >> 10) & 0x7f);
        v.coarse = (uint32_t)((r64 >> 4 & 1) ? -cc : cc);
        v.vco_hz = 2 * c.synthHz();
        memcpy(o, &v, v.struct_size);
        return 0;
    });
}
