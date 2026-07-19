#pragma once
#include <stdint.h>
#include <stddef.h>
#include "overdrive.h"
#include "flanger.h"
#include "tremolo.h"
#include "fxdelay.h"
#include "fxfilter.h"
#include "reverb.h"
#include "setup_menu.h"
#include "cJSON.h"

// fxrack — the shared FX rack: curated slots over a machine's existing effect
// instances. FX1/FX2 each hold ONE generic effect (or Off) and run in slot
// order; FX3 is the fixed reverb slot. One descriptor table + one dynamic menu +
// one process path + one (de)serializer, reused by Keys / Synth / Tape instead
// of each carrying its own copy. See plans/fx-rack-20260717.md.
//
// An fxrack_t is a POINTER VIEW: the machine owns the effect structs (od/flg/…)
// plus an int8_t slot[FX_NSLOT_GEN], and hands the rack pointers to them. No
// struct re-layout needed in the machines.

enum { FXK_OFF = 0, FXK_OD, FXK_FLG, FXK_TREM, FXK_DLY, FXK_FILT, FXK_BAND, FXK_NGEN };
#define FX_NSLOT_GEN 2          // FX1, FX2 (generic); FX3 (reverb) is separate

// Shared musical divisions for the clock-syncable rate effects (delay time /
// flanger + tremolo LFO). One table so display order and the beats the kernel
// uses can never disagree; machines that know a tempo set fxrack_t.bpm and flip
// an effect's `sync` on to lock it to the grid.
#define FXRACK_NDIV 7
extern const float       fxrack_div_beats[FXRACK_NDIV];
extern const char *const fxrack_div_names[FXRACK_NDIV];

typedef struct {
    overdrive_t *od;
    flanger_t   *flg;
    tremolo_t   *trem;
    fxdelay_t   *dly;
    fxfilter_t  *filt;          // LP/HP/BP filter brick
    fxfilter_t  *band;          // base/width band filter brick (own instance)
    reverb_t    *rv;
    int8_t      *slot;          // [FX_NSLOT_GEN] = FXK_* per generic slot
    float        bpm;           // grid tempo for synced effects (0 = free-running)
} fxrack_t;

// process the machine buffer in place: unpack -> generic slots in order ->
// reverb -> soft-clip pack (fxchain.h). No inter-stage clamp.
void fxrack_process_i32(const fxrack_t *rk, int32_t *out, int frames);

// display name of a generic slot's effect, for the host's Setup line
const char *fxrack_slot_name(const fxrack_t *rk, int slot);   // slot 2 -> reverb mode

// ---- dynamic per-slot menu (one slot at a time) -------------------------------
// Build the rows for `slot` (0,1 generic; 2 = reverb): an Effect-select row then
// the selected effect's params. Fills items[] + param_of_row[] (param<0 = the
// select row), returns the row count. items/param_of_row must hold >= FXRACK_MAXROWS.
#define FXRACK_MAXROWS 12
int  fxrack_menu_rows(const fxrack_t *rk, int slot, setup_item_t *items, int8_t *param_of_row);
// value string for row `param` (param<0 = select row: effect name + live CPU%)
void fxrack_menu_val(const fxrack_t *rk, int slot, int param, char *v, size_t n);
// adjust row `param` by dir (param<0 = change the effect; lazy-inits buffers)
void fxrack_menu_adj(const fxrack_t *rk, int slot, int param, int dir);

// ---- autosave: owns the whole FX serialization (slots + every effect param) ---
void fxrack_save(const fxrack_t *rk, cJSON *o);
// loads slots + params; if the "fxsl" key is absent, migrates from the legacy
// per-effect on/off bools ("od"/"flg"/"trem"/"dly") in the node.
void fxrack_load(const fxrack_t *rk, const cJSON *node);
