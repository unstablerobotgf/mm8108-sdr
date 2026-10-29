/* libmmsdr: Morse Micro MM8108 HaLow module as an SDR (USB, userspace boot). See docs/libmmsdr.md.
 * v0.1: RX implemented; calls marked PLACEHOLDER return MMSDR_ERR_UNSUPPORTED. */
#ifndef MMSDR_H
#define MMSDR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(MMSDR_BUILD)
#define MMSDR_API __declspec(dllexport)
#elif defined(_WIN32)
#define MMSDR_API __declspec(dllimport)
#else
#define MMSDR_API __attribute__((visibility("default")))
#endif

/* ABI: major bumps break; minor adds functions or struct tail fields (structs carry struct_size) */
#define MMSDR_API_MAJOR 0
#define MMSDR_API_MINOR 1
MMSDR_API uint32_t mmsdr_api_version(void);            /* (major << 16) | minor of the loaded library */
MMSDR_API const char* mmsdr_version_string(void);     /* "0.1.0+<git>" */

/* ------------------------------------------------------------------ errors */

typedef enum {
    MMSDR_OK = 0,
    MMSDR_ERR_INVALID = -1,        /* bad argument or struct_size */
    MMSDR_ERR_NO_DEVICE = -2,
    MMSDR_ERR_ACCESS = -3,         /* claimed by another process or a kernel driver we could not detach */
    MMSDR_ERR_USB = -4,            /* transfer failed; device may recover */
    MMSDR_ERR_NEEDS_REPLUG = -5,   /* chip wedged (no reads complete); only a physical replug recovers */
    MMSDR_ERR_FIRMWARE = -6,       /* fw/BCF file missing, not an ELF, or encoding check failed */
    MMSDR_ERR_BOOT = -7,           /* host table never set or fw not running */
    MMSDR_ERR_CMD = -8,            /* YAPS command rejected, CRC latch, or no response */
    MMSDR_ERR_PARK = -9,           /* UPHY did not park after ring take, internal retries exhausted */
    MMSDR_ERR_TIMEOUT = -10,
    MMSDR_ERR_RANGE = -11,         /* frequency/rate/gain outside what the hardware does */
    MMSDR_ERR_STATE = -12,         /* wrong state, e.g. blocking call from inside the RX callback */
    MMSDR_ERR_UNSUPPORTED = -13,   /* placeholder API, not implemented in this build */
    MMSDR_ERR_POLICY = -14,        /* debug MMIO access outside the 60 B / allowlist rule */
    MMSDR_ERR_NOMEM = -15,
    MMSDR_ERR_OVERFLOW = -16,      /* reserved; overflow is reported in mmsdr_meta flags */
} mmsdr_err;

MMSDR_API const char* mmsdr_strerror(int err);
typedef struct mmsdr_dev mmsdr_dev;
/* human detail for the last error on this device and thread, e.g. "LPHY DC cal timeout: lock 1 door 0 ..." */
MMSDR_API const char* mmsdr_last_error_detail(mmsdr_dev* dev);

typedef enum { MMSDR_LOG_ERROR, MMSDR_LOG_WARN, MMSDR_LOG_INFO, MMSDR_LOG_DEBUG } mmsdr_log_level;
typedef void (*mmsdr_log_cb)(void* user, mmsdr_log_level level, const char* msg);
MMSDR_API void mmsdr_set_log(mmsdr_log_cb cb, void* user, mmsdr_log_level max_level);   /* NULL: stderr */

/* ------------------------------------------------------------------ discovery and open */

typedef enum { MMSDR_CHIP_UNKNOWN = 0, MMSDR_CHIP_MM8108B2 = 0x809 } mmsdr_chip;

typedef struct {
    size_t struct_size;
    char serial[64];               /* USB iSerial: "MM0000000123" is a ROM constant (0x310c74), NOT unique */
    char uid[32];                  /* factory MAC from OTP banks 26/27, "0cbf7400329f"; "" if busy elsewhere */
    char usb_path[32];             /* "bus-port.port", unique while plugged */
    uint16_t vid, pid;             /* 0x325b:0x8100 */
    mmsdr_chip chip;               /* UNKNOWN until booted once */
    int busy;                      /* nonzero: claimed elsewhere */
} mmsdr_devinfo;

MMSDR_API int mmsdr_list(mmsdr_devinfo* out, int max);   /* count found (may exceed max) or error */

typedef struct {
    size_t struct_size;
    const char* select;            /* uid or usb_path; NULL = first free device */
    const char* firmware_path;     /* mm8108b2-rl.bin; NULL = search fw_dir, then $MMSDR_FW_DIR */
    const char* bcf_path;          /* board config; NULL = search, matched to the OTP board type */
    const char* fw_dir;
    const char* country;           /* regdom TLV to load, "US" default */
    int32_t freq_correction_ppb;   /* XO error, applied from the first tune (0 for a TCXO-clocked board) */
} mmsdr_open_args;

/* claims USB only; the ~0.35 s boot happens on first tune or mmsdr_boot() */
MMSDR_API int mmsdr_open(mmsdr_dev** dev, const mmsdr_open_args* args);
MMSDR_API int mmsdr_boot(mmsdr_dev* dev);
/* stops streams, wakes blocked mmsdr_rx_read callers (they return MMSDR_ERR_STATE), releases USB.
 * No other call on this handle may still be running, apart from rx_read and the RX callback. */
MMSDR_API void mmsdr_close(mmsdr_dev* dev);
MMSDR_API int mmsdr_get_info(mmsdr_dev* dev, mmsdr_devinfo* out);

/* ------------------------------------------------------------------ RX configuration */

/* What a setter had to do. Streams see DISCONTINUITY for HOP and REBOOT. */
typedef enum {
    MMSDR_APPLY_NONE = 0,          /* stored; applies at next start */
    MMSDR_APPLY_QUEUED,            /* called from the RX callback: handed to the control thread, newest wins;
                                      meta RETUNED marks the first sample at the new setting */
    MMSDR_APPLY_LIVE,              /* register write, no gap */
    MMSDR_APPLY_HOP,               /* register-only PLL hop, ~60 ms gap */
    MMSDR_APPLY_REBOOT,            /* reboot + tune + ring take, ~0.75 s gap */
} mmsdr_apply;

typedef struct {
    size_t struct_size;
    mmsdr_apply action;
    double freq_hz;                /* LO actually locked, from the synth readback */
    double sample_rate;            /* delivered rate, from the resampler readback */
    uint32_t retries;              /* internal park retries used */
    double elapsed_s;
} mmsdr_apply_result;              /* every result pointer below may be NULL */

typedef struct { double min, max, step; } mmsdr_range;

MMSDR_API int mmsdr_get_freq_range(mmsdr_dev* dev, mmsdr_range* out);   /* 750-1100 MHz, 1.9 Hz step */
MMSDR_API int mmsdr_set_freq(mmsdr_dev* dev, double hz, mmsdr_apply_result* res);
MMSDR_API double mmsdr_get_freq(mmsdr_dev* dev);
MMSDR_API int mmsdr_set_hop_enabled(mmsdr_dev* dev, int on);  /* 0: every retune reboots */

/* 0 = default 8 MS/s. Otherwise wide mode: fs = f_ADC / (2 ratio), ratio in [0.5, 1).
 * Analog rolloff is fixed (-3 dB at +-3 MHz, -6 dB at +-4 MHz); USB caps the rate per format. */
MMSDR_API int mmsdr_get_rate_range(mmsdr_dev* dev, double freq_hz, mmsdr_range* out);
MMSDR_API int mmsdr_set_sample_rate(mmsdr_dev* dev, double fs, mmsdr_apply_result* res);
MMSDR_API double mmsdr_get_sample_rate(mmsdr_dev* dev);

typedef enum {
    MMSDR_FMT_CS16 = 0,            /* native, interleaved int16 I/Q, spectral inversion already undone */
    MMSDR_FMT_CS8,                 /* UPHY packer, 8-bit I/Q, ~48 dB range, lets raw ADC rate fit USB; reboot */
    MMSDR_FMT_CF32,                /* library converts from CS16; amplitude cal applied if enabled */
} mmsdr_format;
MMSDR_API int mmsdr_set_format(mmsdr_dev* dev, mmsdr_format fmt, mmsdr_apply_result* res);

/* Gain index 0..16, ~3 dB/step; drives 0x403004[13:8] (AGC fixed-gain mode). */
MMSDR_API int mmsdr_get_gain_range(mmsdr_dev* dev, int* min_idx, int* max_idx);
MMSDR_API int mmsdr_set_gain(mmsdr_dev* dev, int idx);
MMSDR_API int mmsdr_get_gain(mmsdr_dev* dev);       /* current index, including AGC changes */
/* Relative gain in dB: nominal 3 dB/step (0..48) until a measured table is loaded. */
typedef enum { MMSDR_CAL_NOMINAL = 0, MMSDR_CAL_MEASURED } mmsdr_cal_source;
MMSDR_API int mmsdr_gain_db(mmsdr_dev* dev, int idx, double* db, mmsdr_cal_source* src);
MMSDR_API int mmsdr_gain_idx_for_db(mmsdr_dev* dev, double db);   /* nearest index, for dB-based clients */

/* Host AGC: backs off on int16 clipping, creeps up when quiet, never above the set index. Default OFF. */
typedef enum { MMSDR_AGC_OFF = 0, MMSDR_AGC_CLIP_GUARD } mmsdr_agc;
MMSDR_API int mmsdr_set_agc(mmsdr_dev* dev, mmsdr_agc mode);
MMSDR_API mmsdr_agc mmsdr_get_agc(mmsdr_dev* dev);

/* HOLD: fw servo converges 20 ms then freezes (no notch), host tracker removes the residual. SERVO: fw 80 kHz HPF. */
typedef enum { MMSDR_DC_HOLD = 0, MMSDR_DC_SERVO, MMSDR_DC_OFF } mmsdr_dc;
MMSDR_API int mmsdr_set_dc_mode(mmsdr_dev* dev, mmsdr_dc mode);
MMSDR_API mmsdr_dc mmsdr_get_dc_mode(mmsdr_dev* dev);
MMSDR_API int mmsdr_recal_dc(mmsdr_dev* dev);       /* LPHY 0x3017 analog DC cal over 21 gain indices; streaming only */
/* host-side blind IQ imbalance fix. Default OFF. */
MMSDR_API int mmsdr_set_iq_correction(mmsdr_dev* dev, int on);
MMSDR_API int mmsdr_get_iq_correction(mmsdr_dev* dev);

/* ------------------------------------------------------------------ settings discovery */

/* Every public toggle is also a named setting, so generic hosts (Soapy writeSetting, GUIs) can list
 * and flip them without knowing this header. Keys: "agc", "iq_correction", "passband_eq",
 * "dc_mode", "hop", "format". Typed setters above and these keys share one state. */
typedef enum { MMSDR_SETTING_BOOL, MMSDR_SETTING_INT, MMSDR_SETTING_FLOAT, MMSDR_SETTING_ENUM } mmsdr_setting_type;
typedef struct {
    size_t struct_size;
    const char* key;
    const char* name;              /* UI label */
    const char* description;
    mmsdr_setting_type type;
    const char* default_value;
    const char* options;           /* ENUM only: "hold,servo,off" */
    mmsdr_apply cost;              /* LIVE, HOP or REBOOT when changed while streaming */
} mmsdr_setting_info;
MMSDR_API int mmsdr_list_settings(mmsdr_dev* dev, mmsdr_setting_info* out, int max);   /* count or error */
MMSDR_API int mmsdr_write_setting(mmsdr_dev* dev, const char* key, const char* val, mmsdr_apply_result* res);
MMSDR_API int mmsdr_read_setting(mmsdr_dev* dev, const char* key, char* out, size_t out_len);

/* ------------------------------------------------------------------ RX streaming */

enum {
    MMSDR_META_DISCONTINUITY = 1 << 0,   /* samples before this block are not contiguous with it */
    MMSDR_META_RETUNED = 1 << 1,         /* freq/rate/format changed at sample 0 of this block */
    MMSDR_META_GAIN_CHANGED = 1 << 2,    /* gain index changed (user or AGC); DC re-held */
    MMSDR_META_OVERFLOW = 1 << 3,        /* host fell behind, samples dropped right before sample 0 of this block */
    MMSDR_META_CLIPPED = 1 << 4,         /* at least one sample hit the int16 rails */
    MMSDR_META_TIME_VALID = 1 << 5,      /* PLACEHOLDER: hw_time_ns is meaningful */
    MMSDR_META_SOURCE_GAP = 1 << 6,      /* PLACEHOLDER: fw stalled the ring DMA (the 8 MHz ~30 ms episodes); never set yet */
};

typedef struct {
    size_t struct_size;
    uint64_t sample_index;         /* index of sample 0 since stream start, counts dropped samples too */
    uint64_t hw_time_ns;           /* PLACEHOLDER: from the 1 MHz hwtimer 0x406040, needs ring-to-timer alignment */
    uint32_t flags;                /* MMSDR_META_* */
    double freq_hz, sample_rate;
    int gain_idx;
    float gain_db;                 /* relative, from mmsdr_gain_db (nominal 3 dB/step) */
    float dbfs_to_dbm;             /* absolute: NaN until a measured table exists */
} mmsdr_meta;

typedef struct {
    const void* samples;           /* format as configured; valid only during the callback */
    uint32_t count;                /* complex samples */
    mmsdr_format format;
    const mmsdr_meta* meta;
} mmsdr_rx_block;

/* Called on the library's output thread. Any setter may be called inside: host-side ones (agc, iq, eq, hop)
 * apply at once; anything touching the chip returns MMSDR_APPLY_QUEUED and runs on the control thread.
 * Blocking calls (rx_stop, close, debug access) return MMSDR_ERR_STATE here. */
typedef void (*mmsdr_rx_cb)(void* user, const mmsdr_rx_block* blk);

typedef struct {
    size_t struct_size;
    mmsdr_rx_cb callback;          /* NULL: sync mode, pull with mmsdr_rx_read */
    void* user;
    uint32_t block_samples;        /* max samples per callback; 0 = 16384 */
    uint32_t buffer_ms;            /* host ring depth for sync mode; 0 = 250 ms */
    int realtime_priority;         /* 0 = default (USB reader time-critical: needed for gapless 8 MS/s), -1 = normal */
} mmsdr_rx_args;

MMSDR_API int mmsdr_rx_start(mmsdr_dev* dev, const mmsdr_rx_args* args);  /* boots and tunes if needed */
MMSDR_API int mmsdr_rx_stop(mmsdr_dev* dev);
MMSDR_API int mmsdr_rx_is_streaming(mmsdr_dev* dev);
/* sync mode: returns samples copied (0 on timeout) or error. A read never spans a meta change, so meta
 * describes sample 0; dropped samples show as OVERFLOW. timeout_ms < 0 waits forever. Format is fixed while streaming. */
MMSDR_API int mmsdr_rx_read(mmsdr_dev* dev, void* buf, uint32_t max_samples, mmsdr_meta* meta, int timeout_ms);

typedef struct {
    size_t struct_size;
    uint64_t samples, bad_ptr, lapped, usb_err, overflow, clipped;
    uint64_t hops, reboots, park_retries, source_gaps;   /* source_gaps: PLACEHOLDER, always 0 */
    int peak_abs;                  /* max |I|,|Q| since last mmsdr_get_stats */
    uint32_t dc_epoch;             /* bumps on every DC re-hold */
    int boosted_threads;
    float iq_irr_db;               /* raw image rejection the IQ corrector measures; 0 when it is off */
} mmsdr_stats;
MMSDR_API int mmsdr_get_stats(mmsdr_dev* dev, mmsdr_stats* out);

/* ------------------------------------------------------------------ sensors (placeholders) */

/* LPHY 0x3013: aux ADC ch 8 (temperature) and ch 9 (supply); scaling unverified */
/* PLACEHOLDER */
MMSDR_API int mmsdr_get_temperature(mmsdr_dev* dev, double* celsius);
MMSDR_API int mmsdr_get_supply_voltage(mmsdr_dev* dev, double* volts);

/* ------------------------------------------------------------------ calibration (placeholders) */

/* Frequency: one XO feeds the ADPLL, so an XO error scales both LO and fs. The correction is
 * applied to the synth word and the resampler ratio, so reported freq/rate stay true. */
MMSDR_API int mmsdr_set_freq_correction_ppb(mmsdr_dev* dev, int32_t ppb, mmsdr_apply_result* res);
MMSDR_API int mmsdr_get_freq_correction_ppb(mmsdr_dev* dev, int32_t* ppb);
/* Estimate ppb from a carrier at a known frequency in the current capture window (P25 CC, GPSDO tone). */
/* PLACEHOLDER */
MMSDR_API int mmsdr_cal_freq_measure(mmsdr_dev* dev, double known_hz, double seconds, int32_t* ppb_out);

/* Amplitude (rough, built in): relative gain via mmsdr_gain_db, plus the measured analog
 * rolloff (-1 dB +-1.5, -3 dB +-3, -6 dB +-4, -10 dB +-5 MHz). Absolute dBm needs a reference (later). */
MMSDR_API int mmsdr_passband_db(mmsdr_dev* dev, double offset_hz, double* db);
MMSDR_API int mmsdr_get_dbfs_to_dbm(mmsdr_dev* dev, double freq_hz, int gain_idx, double* offset_db);   /* UNSUPPORTED for now */
/* Wide mode: flatten the rolloff in software with the curve above. Default OFF. */
MMSDR_API int mmsdr_set_passband_eq(mmsdr_dev* dev, int on);
MMSDR_API int mmsdr_get_passband_eq(mmsdr_dev* dev);

/* ------------------------------------------------------------------ TX (placeholders, all UNSUPPORTED in v0.x) */

/* An instrument, like a signal generator: no gating beyond what the hardware can do. */
typedef enum { MMSDR_TXA_OFF = 0, MMSDR_TXA_WARMING, MMSDR_TXA_READY } mmsdr_txa_state;   /* 0x402070 FSM, ~2300 us warm-up */

MMSDR_API int mmsdr_tx_get_freq_range(mmsdr_dev* dev, mmsdr_range* out);
MMSDR_API int mmsdr_tx_get_power_range(mmsdr_dev* dev, double freq_hz, mmsdr_range* out_dbm);
MMSDR_API int mmsdr_tx_set_freq(mmsdr_dev* dev, double hz, mmsdr_apply_result* res);
MMSDR_API int mmsdr_tx_set_power_dbm(mmsdr_dev* dev, double dbm, double* actual_dbm);   /* BCF power tables */
MMSDR_API int mmsdr_tx_get_state(mmsdr_dev* dev, mmsdr_txa_state* out);

/* fills `count` CS16 samples for the TX FIFO (32-bit I/Q words via DMA ch3) */
typedef int (*mmsdr_tx_cb)(void* user, int16_t* iq, uint32_t count, mmsdr_meta* meta);
typedef struct {
    size_t struct_size;
    mmsdr_tx_cb callback;          /* NULL: sync mode, push with mmsdr_tx_write */
    void* user;
    uint32_t block_samples;
    int underrun_zero;             /* nonzero: send zeros on underrun instead of ending the burst */
} mmsdr_tx_args;
MMSDR_API int mmsdr_tx_start(mmsdr_dev* dev, const mmsdr_tx_args* args);
MMSDR_API int mmsdr_tx_write(mmsdr_dev* dev, const int16_t* iq, uint32_t count, int timeout_ms);
MMSDR_API int mmsdr_tx_stop(mmsdr_dev* dev);       /* also called by close and by RX restarts */

#ifdef __cplusplus
}
#endif
#endif /* MMSDR_H */
