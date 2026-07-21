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
// house convention; absolute positioning is the host's call.
#define CVMTX_MAX 12

typedef struct {
    const char *const *labels;   // host's destination names (static storage)
    int n;                       // destinations, <= CVMTX_MAX
    int8_t src[CVMTX_MAX];       // -1 off / 0..7 = CV1..8
    float  amt[CVMTX_MAX];       // -1..+1 (bipolar depth)
    int    floor12[2];           // tracked ch1/2 idle floor (audio task)
} cvmtx_t;

void cvmtx_init(cvmtx_t *m, const char *const *labels, int n);

// once per audio block, before any cvmtx_val read: follow the ch1/2 floors
// (drop with the reading, drift back up — the synth tracker)
void cvmtx_track(cvmtx_t *m, const int cvm[8]);

// conditioned source read, 0..1 (0 when the channel hasn't converged)
float cvmtx_cv01(const cvmtx_t *m, const int cvm[8], int src);
// the modulation value for dest d: amt * cv01, -1..+1; 0 when unassigned
float cvmtx_val(const cvmtx_t *m, const int cvm[8], int d);
bool  cvmtx_any(const cvmtx_t *m);   // any dest assigned (for Setup row text)

// the matrix page: feed it every event from the host's page handler.
// Returns 0 normally, ret_page when the user exits via the affordance,
// live_page on a long press.
int cvmtx_menu_event(cvmtx_t *m, int event, const char *title,
                     int ret_page, int live_page);

// persistence: "mxs" (sources) + "mxa" (amounts, int %) parallel arrays
void cvmtx_save(const cvmtx_t *m, cJSON *o);
void cvmtx_load(cvmtx_t *m, const cJSON *node);
