#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"

// cvmtx — the shared assignable CV matrix (Tape is the first host; the
// synth/sampler3 matrices predate it and can migrate later). N host-named
// destinations, each: source (-1 off / 0..7 = CV1..8) + bipolar amount
// (-100..+100%). The widget owns:
//   - source conditioning: ch1/2 are 1V/oct jacks idling ~21% up the scale,
//     so they rescale from a tracked idle floor and a patched source spans
//     the full 0..1 (the synth matrix's sy_mtx_cv01, lifted verbatim)
//   - the matrix PAGE (label | [src] | [amt] rows, press cycles
//     nav > src > amt > nav — the synth page generalized)
//   - (de)serialization ("mxs"/"mxa" parallel arrays)
// The HOST applies the values: cvmtx_val() is bipolar (amt * conditioned
// CV, -1..+1), 0 when off — fold it into whatever the destination means.
// OFFSET semantics (live, non-destructive, like Tape's win_move) is the
// house convention.
//
// ABSOLUTE mode (2026-09-08, "stage 3" of the web remote rework): an entry may
// instead be a KNOB — takeover semantics lifted verbatim from the hosts' old
// hard-wired K5-K8 code: the entry's source is captured, does nothing until it
// moves past 0.03 of its range, then REPLACES the destination's base value for
// as long as the machine runs (cvmtx_abs() hands the host the 0..1 position;
// the host keeps its own mapping — cutoff log curve, etc.). The host passes a
// DEFAULTS table at init (one source per destination, -1 = none): those
// entries are created as ABS at init/reset and re-created on loading a preset
// that predates the mode array ("mxm") wherever the destination is still
// unassigned — so every existing patch keeps its knobs, while a patch that had
// already routed an OFFSET onto that destination is left single-driven (the
// 2026-07-28 "Cutoff <- CV6 at full amount while K6 IS cutoff" trap can no
// longer arise by default). The clock's CV channel must never take over a
// destination (skip_src, see clock_src_is_cv()); nodirty masks destinations
// whose knob moves are performance-only and must not churn the autosave.
#define CVMTX_MAX 12
#define CVM_OFFSET 0
#define CVM_ABS    1

typedef struct {
    const char *const *labels;   // host's destination names (static storage)
    int n;                       // destinations, <= CVMTX_MAX
    int8_t src[CVMTX_MAX];       // -1 off / 0..7 = CV1..8
    float  amt[CVMTX_MAX];       // -1..+1 (bipolar depth; OFFSET entries)
    uint8_t mode[CVMTX_MAX];     // CVM_OFFSET / CVM_ABS
    const int8_t *def_src;       // host defaults per dest (NULL = none)
    // ABS takeover state (audio task)
    float  capt[CVMTX_MAX];      // captured position at the last (re)arm
    bool   live[CVMTX_MAX];      // moved past threshold -> drives the dest
    float  last01[CVMTX_MAX];    // last committed position (dirty hysteresis)
    float  pos01[CVMTX_MAX];     // this block's conditioned position (cvmtx_abs)
    bool   rearm;                // next track(): recapture, everything un-live
    int8_t skip_src;             // -1, or the CV channel that must never take over (clock)
    uint16_t nodirty;            // bit per dest: ABS moves there don't flag autosave
    int    floor12[2];           // tracked ch1/2 idle floor (audio task)
} cvmtx_t;

// def_src: n entries, -1 = no default; applied at init (cvmtx_reset_defaults)
void cvmtx_init(cvmtx_t *m, const char *const *labels, int n, const int8_t *def_src);
void cvmtx_reset_defaults(cvmtx_t *m);   // everything off, then the defaults as ABS
void cvmtx_rearm(cvmtx_t *m);            // preset/patch load, engine change: recapture
bool cvmtx_is_default(const cvmtx_t *m, int d);   // entry d == its default (page marker)

// once per audio block, before any cvmtx_val/cvmtx_abs read: follow the ch1/2
// floors (drop with the reading, drift back up — the synth tracker) and run
// the ABS takeover detection
void cvmtx_track(cvmtx_t *m, const int cvm[8]);
// ABS entry d is sourced, not the clock channel, and has taken over: *k01 =
// its conditioned position 0..1. False = leave the base value alone.
bool cvmtx_abs(const cvmtx_t *m, int d, float *k01);

// conditioned source read, 0..1 (0 when the channel hasn't converged)
float cvmtx_cv01(const cvmtx_t *m, const int cvm[8], int src);
// the modulation value for dest d: amt * cv01, -1..+1; 0 when unassigned or ABS
float cvmtx_val(const cvmtx_t *m, const int cvm[8], int d);
bool  cvmtx_any(const cvmtx_t *m);   // any dest assigned (for Setup row text)

// the matrix page: feed it every event from the host's page handler.
// Returns 0 normally, ret_page when the user exits via the affordance,
// live_page on a long press.
int cvmtx_menu_event(cvmtx_t *m, int event, const char *title,
                     int ret_page, int live_page);

// persistence: "mxs" (sources) + "mxa" (amounts, int %) + "mxm" (modes)
// parallel arrays, plus "mxl" (labels) and "mxd" (defaults) for the web page.
// Load without "mxm" = a pre-stage-3 preset: defaults migrate onto still-
// unassigned destinations (see above). Both re-arm the takeover.
void cvmtx_save(const cvmtx_t *m, cJSON *o);
void cvmtx_load(cvmtx_t *m, const cJSON *node);
