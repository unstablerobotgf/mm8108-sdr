/* libmmsdr expert/RE interface. Not covered by the ABI promise: may change in any minor release. */
#ifndef MMSDR_DEBUG_H
#define MMSDR_DEBUG_H

#include "mmsdr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Live register access through the reader's ordered pipeline (never a side channel).
 * Policy: MMIO 0x400000-0x40ffff reads are at most 60 B and only within the allowlist of
 * registers the firmware itself touches; anything else returns MMSDR_ERR_POLICY. */
MMSDR_API int mmsdr_peek32(mmsdr_dev* dev, uint32_t addr, uint32_t* val);
MMSDR_API int mmsdr_poke32(mmsdr_dev* dev, uint32_t addr, uint32_t val);
MMSDR_API int mmsdr_peek(mmsdr_dev* dev, uint32_t addr, void* buf, uint32_t len);
MMSDR_API int mmsdr_debug_allow_range(mmsdr_dev* dev, uint32_t lo, uint32_t hi);   /* widen the allowlist */

/* Replaces the MM_* getenv hooks. Keys: "unclamp", "nokdco", "nohop", "band54", "cc", "crashbcast", "raw"
 * (bypass all host DSP: DC tracker, IQ, EQ), "usb.ch", "usb.k", "usb.depth", "usb.prio", "usb.wait_us".
 * Values are strings, "" restores the default. */
MMSDR_API int mmsdr_debug_set(mmsdr_dev* dev, const char* key, const char* val);
MMSDR_API int mmsdr_debug_get(mmsdr_dev* dev, const char* key, char* out, size_t out_len);
/* fills `out` with "key=value\n" lines describing every knob */
MMSDR_API int mmsdr_debug_list(mmsdr_dev* dev, char* out, size_t out_len);

/* Raw host command (YAPS); fw booted, not streaming (mmsdr_boot or a boot hook). Returns fw status or error. */
MMSDR_API int mmsdr_cmd(mmsdr_dev* dev, uint16_t id, const void* payload, uint32_t len,
                        void* resp, uint32_t* resp_len, int timeout_ms);
/* GET_SET_GENERIC_PARAM 0x003E; only valid before the medium-eval step of boot (use the boot hook) */
MMSDR_API int mmsdr_generic_param(mmsdr_dev* dev, uint32_t id, int set, uint32_t* value);

/* PLACEHOLDER: post a message straight to the LPHY inbox (e.g. 0x3013 sensors). */
MMSDR_API int mmsdr_lphy_msg(mmsdr_dev* dev, uint16_t id, const uint32_t* params, int n, int timeout_ms);

/* Runs at fixed points in a reboot-retune, on the thread doing it; peek/poke/cmd work inside.
 * FW_LOADED: fw up + patched. PRE_MEDIUM_EVAL: tuned, generic params still accepted. TUNED: after
 * medium eval, UPHY alive (host cal cmds). RING_TAKEN: UPHY parked, reader not started yet. */
typedef enum { MMSDR_HOOK_FW_LOADED, MMSDR_HOOK_PRE_MEDIUM_EVAL, MMSDR_HOOK_TUNED, MMSDR_HOOK_RING_TAKEN } mmsdr_hook_point;
typedef void (*mmsdr_hook_cb)(void* user, mmsdr_dev* dev, mmsdr_hook_point point);
MMSDR_API int mmsdr_debug_set_hook(mmsdr_dev* dev, mmsdr_hook_cb cb, void* user);

/* Raw ADPLL state for the VCO/KDCO work: tracking word, KDCO, band, coarse bank, VCO estimate. */
typedef struct {
    size_t struct_size;
    uint32_t track_word, kdco, band, coarse;
    double vco_hz;                 /* 2 x synth readback: programmed, not measured */
} mmsdr_pll_state;
MMSDR_API int mmsdr_debug_pll_state(mmsdr_dev* dev, mmsdr_pll_state* out);

#ifdef __cplusplus
}
#endif
#endif /* MMSDR_DEBUG_H */
