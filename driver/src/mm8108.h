// MM8108 (Etna MF15457) as an 8 MS/s receiver: USB memory bridge, userspace boot, YAPS, ring streaming.
// Native port of tools/{mmusb,boot,yaps,tune,owner,radio,ringstream}.py. Internal to libmmsdr (mmsdr.cpp wraps it).
#pragma once
#include "mmsdr.h"
#include <stdint.h>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

// every internal failure carries an mmsdr_err code
struct MMError : std::runtime_error {
    int code;
    MMError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};

class MM8108 {
public:
    // ---------------------------------------------------------------- constants and types
    static constexpr double FS = 8e6;                        // exact everywhere: resampler ratio 0x409404 is programmed per tune
    static constexpr double FMIN = 750e6, FMAX = 1100e6;
    static constexpr double F_RESAMP_MAX = 1024e6;           // above: ADC = VCO/256 so the resampler ratio stays in [0.5, 1)
    static constexpr int GAIN_MAX = 16;                      // reduction = GAIN_MAX - gain (~3 dB/step)
    static constexpr int CC_OFF = INT_MIN;
    enum DcMode { DC_HOLD, DC_SERVO, DC_OFF };
    enum Hook { HOOK_FW_LOADED, HOOK_PRE_MEDIUM_EVAL, HOOK_TUNED, HOOK_RING_TAKEN };   // == mmsdr_hook_point

    struct Stats { uint64_t samples, badPtr, lapped, usbErr, overflow, clipped, hops, reboots, parkRetries; };
    struct DevInfo { std::string path, serial, uid; uint32_t chipId = 0; int board = -1; bool busy = false; };
    // expert knobs (mmsdr_debug_set); these were the MM_* env vars
    struct Knobs {
        std::atomic<bool> unclamp{ false }, nokdco{ false }, nohop{ false }, crashbcast{ false };
        std::atomic<int> band54{ -1 }, cc{ CC_OFF };
        std::atomic<int> ch{ 4096 }, k{ 2 }, depth{ 4 }, prio{ 15 }, waitUs{ 100 };   // prio 15 = THREAD_PRIORITY_TIME_CRITICAL
    };
    // de-inverted interleaved int16 I/Q, complex count, index of sample 0 since this run's ring take (output thread)
    using IQFn = std::function<void(const int16_t*, int, uint64_t)>;

    // ---------------------------------------------------------------- lifecycle and device
    explicit MM8108(IQFn onIQ);
    ~MM8108();
    // VID/PID matches; uid only where claimable; skipped paths are reported busy without touching them
    static std::vector<DevInfo> list(const std::set<std::string>& skipPaths = {});
    int openDevice(const std::string& usbPath);              // "" = first claimable; reads uid (fw host table, else OTP)
    int ensureOpen() { return running ? 0 : openUsb(); }     // after a failed retune closed the handle
    const DevInfo& info() const { return dev; }
    const std::string& error() const { return err; }
    void setFiles(std::string fw, std::string bcf, std::string country);
    void setHook(std::function<void(int)> fn) { hook = std::move(fn); }
    Knobs knobs;

    // ---------------------------------------------------------------- tuning (0 or mmsdr_err; detail in error())
    // streaming: register-only PLL hop; otherwise reboot + tune + freeze UPHY + take ring
    int retune(double freqHz);
    int bootOnly();                                          // fw up, stock state, no ring take
    bool lastTuneWasHop() const { return lastHop; }
    int lastRetries() const { return lastPark; }
    void setHopEnabled(bool on) { hopOn = on; }
    double synthHz() const { return synth; }
    double sampleRate() const { return fsNow; }
    // wide: output fs = f_ADC / (2 ratio), ratio in [0.5, 1); f_ADC = VCO/128 (f <= 1024 MHz). Analog rolloff -6 dB at +-4 MHz. 0 = off
    void setWide(double targetFs) { wideFs = targetFs; }
    double wide() const { return wideFs; }
    // packed: UPHY core packs the ring to 8-bit I/Q (2 B/sample); ~48 dB range. Takes a reboot
    void setPacked(bool on) { packOn = on; }
    bool packed() const { return packedNow; }
    bool packRequested() const { return packOn; }
    // XO error in ppb: LO word and fs scale with the one 32 MHz crystal, so correcting the synth word fixes both
    void setPpb(int32_t p) { ppb = p; }
    int32_t getPpb() const { return ppb; }

    // ---------------------------------------------------------------- gain and DC
    void setGain(int gain);
    int getGain() const { return gain; }
    void setDcMode(DcMode m);
    uint32_t dcEpoch() const { return dcEp; }                // bumps on every DC servo re-hold (hop, gain change, mode)
    std::string recalDc();                                   // streaming only: LPHY 0x3017 analog DC cal (21 gain idx); "" or error

    // ---------------------------------------------------------------- stream state
    void stop();
    bool streaming() const { return running; }
    bool booted() const { return ysl != 0; }
    uint64_t writtenSamples() const { return wpos.load() * (packedNow ? 2 : 1); }
    uint64_t hopSample() const { return hopAt; }             // writtenSamples() right after the last hop's LO write
    // SPSC drops: words lost right before ring word position wordPos (consumed), and the next drop position (UINT64_MAX: none)
    uint64_t takeGap(uint64_t wordPos);
    uint64_t nextGap();
    Stats stats() const;
    int boostedThreads() const { return boosted; }

    // ---------------------------------------------------------------- register access and raw commands
    // while streaming these go through the reader's ordered pipeline; otherwise the sync bridge
    uint32_t r32(uint32_t addr);
    void w32(uint32_t addr, uint32_t v);
    std::vector<uint8_t> peek(uint32_t addr, uint32_t n);    // sync bulk read when idle, word reads while streaming
    // raw YAPS command; fw booted and not streaming (boot hooks). Returns fw status
    int cmd(uint16_t id, const std::vector<uint8_t>& payload, double timeout, std::vector<uint8_t>* resp);

private:
    using CtlP = std::shared_ptr<std::promise<uint32_t>>;    // shared: the caller may time out and leave before the reader completes it
    struct Slot;                                             // one in-flight IN transfer (mm8108.cpp)
    struct OutX;                                             // OUT transfer user_data (mm8108.cpp)
    struct RingRun;                                          // per-run reader schedule state (mm8108.cpp)
    struct Ctl { bool write; uint32_t addr, val; CtlP p; };  // live register request queued for the reader

    // ---- sync bridge (only while the reader is stopped)
    int openUsb();
    void close();
    std::vector<uint8_t> read(uint32_t addr, uint32_t n);
    void write(uint32_t addr, const void* data, uint32_t n);
    uint32_t sr32(uint32_t addr);
    void sw32(uint32_t addr, uint32_t v);
    void readIdentity();
    void readOtpIdentity(uint8_t mac[6]);

    // ---- boot, YAPS and firmware patches
    void aonLatch();
    void digitalReset();
    void boot();
    void bringUp();                                          // boot + YAPS + fw patches
    void yapsInit();
    int yapsSend(uint16_t id, const std::vector<uint8_t>& payload, double timeout, std::vector<uint8_t>* resp = nullptr);
    void patchFirmware();

    // ---- reboot-retune steps (radio.retune)
    bool hop(double f);
    void tuneAndTakeRing(double f);
    int setChannel(double fc);
    void installPacker();
    void applyRingPatch();
    void takeRing();
    void waitParked();
    int startStream();
    void applyRate();
    void applyDc();

    // ---- PLL (host replica of LPHY set_lo)
    void setLo(double f);
    uint32_t kdcoCal(double f);
    static uint32_t kdcoModel(double f);
    double xo() const { return 1.0 + ppb * 1e-9; }
    void fire(int point) { if (hook) hook(point); }

    // ---- reader thread: paced ring reads plus live register traffic, all through one ordered pipeline
    void readerLoop();
    void scheduleReads(RingRun& run);
    void drainCtlQueue();
    void completeSlot(RingRun& run, Slot* s);
    void onPointer(RingRun& run, uint32_t v);
    void pushWords(const uint8_t* buf, uint32_t n);
    void finishReader();
    static void cbIn(libusb_transfer* t);
    static void cbOut(libusb_transfer* t);
    void submitRead(uint32_t addr, uint32_t n, int kind, int64_t tag, CtlP ctl);
    void submitOut(const void* data, int n, CtlP done = nullptr);

    // ---- output thread: SPSC words -> de-inverted int16 I/Q blocks
    void outputLoop();
    uint64_t unpackPacked(uint64_t r, int nw, int16_t* iq);
    uint64_t unpackNative(uint64_t r, int n, int16_t* iq);

    // ---- configuration and device
    std::string fwPath, bcfPath, regdom = "US", err;
    IQFn onIQ;
    std::function<void(int)> hook;
    DevInfo dev;
    libusb_context* ctx = nullptr;
    libusb_device_handle* h = nullptr;
    int boosted = 0;                                         // libusb threads raised to time-critical
    std::map<uint32_t, uint32_t> stock;                      // stock fw words at every patch site, read from the ELF at boot

    // ---- tune state
    double synth = 0;
    std::atomic<double> fsNow{ FS }, wideFs{ 0 };
    std::atomic<bool> packOn{ false }, hopOn{ true };
    bool packedNow = false, lastHop = false;
    std::atomic<int32_t> ppb{ 0 };
    std::atomic<int> gain{ GAIN_MAX };
    DcMode dcMode = DC_HOLD;
    std::atomic<uint32_t> dcEp{ 0 };
    std::atomic<uint64_t> hopAt{ 0 };
    int parkRetries = 0, lastPark = 0;                       // reboot attempts when UPHY fails to park

    // ---- YAPS host interface (from the fw host table)
    uint32_t ysl = 0, yds = 0, sts = 0;
    int cmdQSize = 0, reserved = 0;
    uint16_t seq = 0;

    // ---- reader thread state
    std::thread reader, output;
    std::atomic<bool> running{ false }, stopReq{ false };
    std::deque<Slot*> fifo;                                  // in-flight reads, consumed strictly in submit order
    std::vector<Slot*> freeSlots, zombies;                   // zombies: cancelled but not yet reaped, never reused until done
    std::vector<libusb_transfer*> freeOut;
    int outInFlight = 0;                                     // OUT cmds must be reaped before close
    std::mutex ctlMtx;
    std::deque<Ctl> ctlQ;                                    // live register requests from other threads
    std::mutex gapMtx;
    std::deque<std::pair<uint64_t, uint64_t>> gaps;          // (ring word pos, words dropped) per SPSC overflow

    // ---- reader -> output SPSC ring of raw words
    std::vector<uint32_t> spsc;
    std::atomic<uint64_t> wpos{ 0 }, rpos{ 0 };
    std::mutex outMtx;
    std::condition_variable outCv;

    // ---- counters (stats())
    std::atomic<uint64_t> nSamples{ 0 }, nBadPtr{ 0 }, nLapped{ 0 }, nUsbErr{ 0 }, nOverflow{ 0 }, nClipped{ 0 };
    std::atomic<uint64_t> nHops{ 0 }, nReboots{ 0 }, nPark{ 0 };
};
