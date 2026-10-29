// SDR++ source module for the Morse Micro MM8108 (Etna MF15457), on libmmsdr.
#include <utils/flog.h>
#include <module.h>
#include <core.h>
#include <config.h>
#include <gui/smgui.h>
#include <signal_path/source.h>
#include "mmsdr.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

// declared here instead of via signal_path.h, which drags in the whole DSP chain (and real volk headers)
namespace sigpath { SDRPP_EXPORT SourceManager sourceManager; }

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "mm8108_source",
    /* Description:     */ "Morse Micro MM8108 HaLow chip as an 8-9.7 MS/s receiver (libmmsdr)",
    /* Author:          */ "UnstableRobotGF",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1
};

ConfigManager config;
static const char* DC_MODES = "Hold (no notch)\0Servo (fw 80 kHz notch)\0Off (raw offset)\0";
static const char* BW_MODES = "8.0 MS/s (stock)\0" "9.0 MS/s wide\0" "9.5 MS/s wide\0" "9.7 MS/s wide (USB limit)\0";
static const double BW_RATES[] = { 0, 9.0e6, 9.5e6, 9.7e6 };   // wide = resampler ratio < 1, f <= 1024 MHz only
static constexpr double FMIN = 750e6, FMAX = 1100e6;
static constexpr int GAIN_MAX = 16;

class MM8108SourceModule : public ModuleManager::Instance {
public:
    MM8108SourceModule(std::string name) : name(name) {
        config.acquire();
        fwPath = config.conf["firmware"]; bcfPath = config.conf["bcf"];
        freq = config.conf["frequency"]; gain = config.conf["gain"]; dcMode = config.conf["dcMode"];
        autoGain = config.conf.contains("autoGain") ? (bool)config.conf["autoGain"] : true;
        hop = config.conf.contains("hop") ? (bool)config.conf["hop"] : true;
        iqCorr = config.conf.contains("iqCorr") ? (bool)config.conf["iqCorr"] : true;
        bwMode = config.conf.contains("bwMode") ? std::clamp((int)config.conf["bwMode"], 0, 3) : 0;
        eqOn = config.conf.contains("rolloffEq") ? (bool)config.conf["rolloffEq"] : true;
        config.release();

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        worker = std::thread(&MM8108SourceModule::workerLoop, this);
        sigpath::sourceManager.registerSource("MM8108", &handler);
    }

    ~MM8108SourceModule() {
        stop(this);
        { std::lock_guard<std::mutex> l(reqMtx); quit = true; }
        reqCv.notify_all();
        if (worker.joinable()) worker.join();
        sigpath::sourceManager.unregisterSource("MM8108");
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    // device ops run on one worker so the UI never blocks (first tune boots, ~0.75 s; then ~60 ms hops); requests coalesce
    enum Req { R_NONE = 0, R_START = 1, R_TUNE = 2, R_GAIN = 4, R_DC = 8, R_RATE = 16 };
    void post(int r) { { std::lock_guard<std::mutex> l(reqMtx); pending |= r; } reqCv.notify_all(); }

    void err(const std::string& what, int e) {
        std::string m = what + ": " + mmsdr_strerror(e) + " (" + mmsdr_last_error_detail(dev) + ")";
        setStatus("Error: " + m); flog::error("MM8108: {0}", m);
    }

    void report(const mmsdr_apply_result& r, double ms) {
        if (r.action != MMSDR_APPLY_HOP && r.action != MMSDR_APPLY_REBOOT) return;
        char b[128];
        snprintf(b, sizeof b, "%s %.4f MHz (%.0f ms)", r.action == MMSDR_APPLY_HOP ? "Hopped to" : "Booted at", r.freq_hz / 1e6, ms);
        setStatus(b);
        if (r.sample_rate != curRate) { curRate = r.sample_rate; core::setInputSampleRate(curRate); }
    }

    void openAndStart() {
        mmsdr_open_args oa{}; oa.struct_size = sizeof oa; oa.firmware_path = fwPath.c_str(); oa.bcf_path = bcfPath.c_str();
        mmsdr_dev* d = nullptr;                                   // published to the UI only once streaming
        if (int e = mmsdr_open(&d, &oa)) {
            setStatus(std::string("Error: ") + mmsdr_strerror(e) + " (" + mmsdr_last_error_detail(nullptr) + ")");
            return;
        }
        // stored settings; the tune in rx_start applies them
        mmsdr_set_format(d, MMSDR_FMT_CF32, nullptr);
        mmsdr_set_gain(d, gain);
        mmsdr_set_agc(d, autoGain ? MMSDR_AGC_CLIP_GUARD : MMSDR_AGC_OFF);
        mmsdr_set_dc_mode(d, (mmsdr_dc)dcMode);
        mmsdr_set_hop_enabled(d, hop);
        mmsdr_set_iq_correction(d, iqCorr);
        mmsdr_set_passband_eq(d, eqOn);
        mmsdr_set_freq(d, std::clamp(freq, FMIN, FMAX), nullptr);
        mmsdr_set_sample_rate(d, BW_RATES[bwMode], nullptr);

        mmsdr_rx_args ra{}; ra.struct_size = sizeof ra; ra.callback = onBlock; ra.user = this;
        setStatus("Booting...");
        auto t0 = std::chrono::steady_clock::now();
        if (int e = mmsdr_rx_start(d, &ra)) {
            std::string m = std::string(mmsdr_strerror(e)) + " (" + mmsdr_last_error_detail(d) + ")";
            setStatus("Error: start: " + m); flog::error("MM8108: start: {0}", m);
            mmsdr_close(d);
            return;
        }
        double ms = 1e3 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        mmsdr_apply_result r{}; r.struct_size = sizeof r; r.action = MMSDR_APPLY_REBOOT;
        r.freq_hz = mmsdr_get_freq(d); r.sample_rate = mmsdr_get_sample_rate(d);
        dev = d;
        report(r, ms);
    }

    void workerLoop() {
        for (;;) {
            int r;
            {
                std::unique_lock<std::mutex> l(reqMtx);
                reqCv.wait(l, [&] { return quit || pending; });
                if (quit) return;
                r = pending; pending = 0;
            }
            std::lock_guard<std::mutex> l(devMtx);
            if (!running) continue;
            if (r & R_START) { openAndStart(); continue; }   // applies everything else too
            if (!dev) continue;
            if (r & (R_TUNE | R_RATE)) {
                setStatus("Tuning...");
                auto t0 = std::chrono::steady_clock::now();
                mmsdr_apply_result res{}; res.struct_size = sizeof res;
                int e = (r & R_RATE) ? mmsdr_set_sample_rate(dev, BW_RATES[bwMode], &res) : 0;
                if (!e && (r & R_TUNE)) e = mmsdr_set_freq(dev, freq, &res);
                if (e) err("tune", e);
                else report(res, 1e3 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
            if (r & R_DC) if (int e = mmsdr_set_dc_mode(dev, (mmsdr_dc)dcMode)) err("DC mode", e);
            if (r & R_GAIN) if (int e = mmsdr_set_gain(dev, gain)) err("gain", e);
        }
    }

    // library output thread: CF32 with DC/IQ/EQ already applied
    static void onBlock(void* user, const mmsdr_rx_block* b) {
        MM8108SourceModule* _this = (MM8108SourceModule*)user;
        memcpy(_this->stream.writeBuf, b->samples, sizeof(dsp::complex_t) * b->count);
        _this->stream.swap((int)b->count);
    }

    static void menuSelected(void* ctx) {
        MM8108SourceModule* _this = (MM8108SourceModule*)ctx;
        core::setInputSampleRate(_this->curRate);
    }
    static void menuDeselected(void* ctx) {}

    static void start(void* ctx) {
        MM8108SourceModule* _this = (MM8108SourceModule*)ctx;
        if (_this->running) return;
        _this->running = true;
        _this->post(R_START);
        flog::info("MM8108SourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        MM8108SourceModule* _this = (MM8108SourceModule*)ctx;
        if (!_this->running) return;
        _this->running = false;
        _this->stream.stopWriter();                               // unblocks the output thread if it waits in swap()
        {
            std::lock_guard<std::mutex> l(_this->devMtx);
            if (_this->dev) { mmsdr_rx_stop(_this->dev); mmsdr_close(_this->dev); _this->dev = nullptr; }
        }
        _this->stream.clearWriteStop();
        _this->setStatus("Stopped");
        flog::info("MM8108SourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double f, void* ctx) {
        MM8108SourceModule* _this = (MM8108SourceModule*)ctx;
        _this->freq = std::clamp(f, FMIN, FMAX);
        config.acquire(); config.conf["frequency"] = _this->freq; config.release(true);
        if (_this->running) _this->post(R_TUNE);
    }

    static void menuHandler(void* ctx) {
        MM8108SourceModule* _this = (MM8108SourceModule*)ctx;
        mmsdr_dev* d = _this->dev;                                // UI thread: only host-side setters and lock-free getters
        SmGui::LeftLabel("Gain");
        SmGui::FillWidth();
        if (SmGui::SliderInt(CONCAT("##_mm8108_gain_", _this->name), &_this->gain, 0, GAIN_MAX)) {
            config.acquire(); config.conf["gain"] = _this->gain; config.release(true);
            if (_this->running) _this->post(R_GAIN);
        }
        if (SmGui::Checkbox(CONCAT("Auto gain (slider = max)##_mm8108_agc_", _this->name), &_this->autoGain)) {
            config.acquire(); config.conf["autoGain"] = _this->autoGain; config.release(true);
            if (d) mmsdr_set_agc(d, _this->autoGain ? MMSDR_AGC_CLIP_GUARD : MMSDR_AGC_OFF);
        }
        if (SmGui::Checkbox(CONCAT("Fast retune (PLL hop)##_mm8108_hop_", _this->name), &_this->hop)) {
            config.acquire(); config.conf["hop"] = _this->hop; config.release(true);
            if (d) mmsdr_set_hop_enabled(d, _this->hop);
        }
        if (SmGui::Checkbox(CONCAT("IQ image correction##_mm8108_iq_", _this->name), &_this->iqCorr)) {
            config.acquire(); config.conf["iqCorr"] = _this->iqCorr; config.release(true);
            if (d) mmsdr_set_iq_correction(d, _this->iqCorr);
        }
        SmGui::LeftLabel("Rate");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_mm8108_bw_", _this->name), &_this->bwMode, BW_MODES)) {
            config.acquire(); config.conf["bwMode"] = _this->bwMode; config.release(true);
            if (_this->running) _this->post(R_RATE);
        }
        if (SmGui::Checkbox(CONCAT("Flatten analog rolloff (EQ)##_mm8108_eq_", _this->name), &_this->eqOn)) {
            config.acquire(); config.conf["rolloffEq"] = _this->eqOn; config.release(true);
            if (d) mmsdr_set_passband_eq(d, _this->eqOn);
        }
        SmGui::LeftLabel("DC");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_mm8108_dc_", _this->name), &_this->dcMode, DC_MODES)) {
            config.acquire(); config.conf["dcMode"] = _this->dcMode; config.release(true);
            if (_this->running) _this->post(R_DC);
        }
        { std::string st = _this->getStatus(); SmGui::Text(st.c_str()); }
        if (_this->running && d) {
            _this->updateRateText(d);
            SmGui::Text(_this->rateTxt.c_str());
        }
        if (FMIN > _this->freq || _this->freq > FMAX) SmGui::Text("Range 750-1100 MHz");
    }

    // per-second deltas of the reader counters, for the menu
    void updateRateText(mmsdr_dev* d) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - tLast).count();
        if (dt < 1.0) return;
        mmsdr_stats c{}; c.struct_size = sizeof c; mmsdr_get_stats(d, &c);
        const mmsdr_stats& l = last;
        char b[200];
        snprintf(b, sizeof b, "%.3f MS/s  gain %d  raw IRR %.0f dB  clips %llu  laps %llu  bad ptr %llu  usb err %llu  overflow %llu",
                 (c.samples - l.samples) / dt / 1e6, mmsdr_get_gain(d), c.iq_irr_db > 0 ? c.iq_irr_db : 99.0f,
                 c.clipped - l.clipped, c.lapped - l.lapped, c.bad_ptr - l.bad_ptr, c.usb_err - l.usb_err, c.overflow - l.overflow);
        rateTxt = b; last = c; tLast = now;
    }

    void setStatus(const std::string& v) { std::lock_guard<std::mutex> l(statusMtx); status = v; }
    std::string getStatus() { std::lock_guard<std::mutex> l(statusMtx); return status; }

    // ---- state
    std::string name, fwPath, bcfPath;
    bool enabled = true;
    std::atomic<bool> running{ false };
    std::atomic<mmsdr_dev*> dev{ nullptr };                       // set by the worker once streaming; UI reads it
    // settings (config.json)
    double freq = 851.975e6;
    int gain = GAIN_MAX, dcMode = 0, bwMode = 0;
    bool autoGain = true, hop = true, iqCorr = true, eqOn = true;
    double curRate = 8e6;
    // status line and rate text: written by the worker / menu, read by the UI
    std::mutex statusMtx;
    std::string status = "Idle", rateTxt;
    mmsdr_stats last{};
    std::chrono::steady_clock::time_point tLast{};
    // SDR++ plumbing and the worker's request queue
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    std::mutex devMtx, reqMtx;
    std::condition_variable reqCv;
    int pending = 0;
    bool quit = false;
    std::thread worker;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["firmware"] = "";                                  // empty: libmmsdr searches MMSDR_FW_DIR, then next to mmsdr.dll (sdrpp.exe's dir)
    def["bcf"] = "";
    def["frequency"] = 851.975e6;
    def["gain"] = GAIN_MAX;
    def["dcMode"] = 0;
    def["autoGain"] = true;
    def["hop"] = true;
    def["iqCorr"] = true;
    def["bwMode"] = 0;
    def["rolloffEq"] = true;
    config.setPath(core::args["root"].s() + "/mm8108_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) { return new MM8108SourceModule(name); }
MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) { delete (MM8108SourceModule*)instance; }
MOD_EXPORT void _END_() { config.disableAutoSave(); config.save(); }
