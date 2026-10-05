#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "cvmtx.h"
#include "setup_menu.h"

// ptab — the shared PARAMETER TABLE (Looper is the first host, 2026-10-04).
//
// A machine used to describe each setting in five places that had to be kept
// in step by hand: the Setup row label, the value string, the adjust switch,
// the preset save and the preset load. Its knobs were four more: the matrix
// labels, the default sources, the hold calls and the lines that turn a knob
// position into a value. Here each is ONE ROW, and the Setup page, the web
// mirror, the preset and the knob block are all read off the same table — so
// a row count can no longer disagree with the rows (the setup_menu_t.n trap).
//
// Nothing is replaced: rows are drawn by setup_menu, knobs are taken over by
// cvmtx (GRAB/CATCH, hold, override). This is the declaration on top of them.
//
// STATE. A machine may want its controls to mean different things depending on
// what it is doing — filter engaged, a loop running, recording. The widget does
// not own the idea of a mode (machines differ far too much, see cvmtx.h); the
// machine reports its state as a bitmask (state(), any task) and every row and
// destination names the bits it needs: live while ALL of `when` are set and
// NONE of `unless` are (0 = no condition).
//   a ROW that is not live is drawn dim and ignores press/turn, panel and web
//   a DESTINATION that is not live is HELD (cvmtx_hold): its capture follows
//     the knob, so the moment it comes live it is armed where the knob sits —
//     dead until moved (GRAB) or until it crosses the value (CATCH)
// ONE KNOB, TWO JOBS is therefore two destinations with the same default source
// and opposite conditions — { "Cutoff", 5, .unless = REC } beside { "Rec level",
// 5, .when = REC }. Both are ordinary matrix rows: the page shows both, either
// can be re-aimed, and nothing about the preset changes with state. Parameters
// never change with state, only what is pointing at them.

// Row types. BOOL and INT are backed by the machine's own variable (p): the
// table renders, adjusts, clamps, saves and loads them. TOGGLE and RANGE are
// drawn by the machine (render + adjust) for values that are not its to store
// (the core clock) or do not fit an int. ACTION fires action().
typedef enum {
    PT_BOOL = 0,   // p = volatile bool *   "ON"/"OFF", press flips     -> ST_TOGGLE
    PT_INT,        // p = volatile int *    lo..hi, turn steps by one   -> ST_RANGE
    PT_TOGGLE,     // machine-drawn, press cycles                       -> ST_TOGGLE
    PT_RANGE,      // machine-drawn, [ ] edit + turn                    -> ST_RANGE
    PT_ACTION,     // press fires action() (returns a menu id, 0 = stay)-> ST_ACTION
} pt_type_t;

typedef struct {
    const char *label;     // Setup row label
    const char *key;       // preset key, NULL = not saved (BOOL/INT only)
    uint8_t  type;         // pt_type_t
    void    *p;            // BOOL/INT storage
    int      lo, hi;       // INT: range, applied on edit AND on preset load
    uint16_t when, unless; // live while all of `when` and none of `unless` are set
    // optional for BOOL/INT (replace the default), required for TOGGLE/RANGE:
    void (*render)(char *buf, size_t n);
    void (*adjust)(int dir);
    int  (*action)(void);  // ACTION
} pt_row_t;

// A knob destination = one CV Matrix row. apply() runs in the AUDIO task with
// the taken-over position 0..1; the mapping (range, curve) is the machine's.
typedef struct {
    const char *label;     // CV Matrix row label
    int8_t   def_src;      // default source (ABS knob), -1 = none
    uint8_t  tk;           // CVM_TK_GRAB / CVM_TK_CATCH
    uint16_t when, unless; // live while all of `when` and none of `unless` are set
    void (*apply)(float k01);
} pt_dest_t;

// Counts come from PT_N() at the definition, never a literal.
#define PT_N(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef struct ptab_s {
    const pt_row_t   *rows;   int n_rows;
    const pt_dest_t  *dests;  int n_dests;    // <= CVMTX_MAX
    uint16_t (*state)(void);                  // NULL = no states
} ptab_t;

// what cvmtx needs handed to it as arrays, built from the table. Lives in the
// machine's state next to its cvmtx_t.
typedef struct {
    const char *labels[CVMTX_MAX];
    int8_t      defs[CVMTX_MAX];
} ptab_rt_t;

uint16_t ptab_state(const ptab_t *t);

// ---- rows (UI task) — setup_menu calls these when setup_menu_t.tab is set
bool ptab_row_live(const ptab_t *t, int i);
setup_kind_t ptab_row_kind(const ptab_t *t, int i);
void ptab_row_render(const ptab_t *t, int i, char *buf, size_t n);
void ptab_row_adjust(const ptab_t *t, int i, int dir);   // no-op on a row that is not live
int  ptab_row_action(const ptab_t *t, int i);            // 0 on a row that is not live

// ---- preset: every row with a key. Load leaves a missing key alone and
// clamps an INT to lo..hi.
void ptab_save(const ptab_t *t, cJSON *o);
void ptab_load(const ptab_t *t, const cJSON *node);

// ---- knobs. ptab_mtx_init replaces cvmtx_init. ptab_knobs runs once per audio
// block in place of the machine's hold calls + cvmtx_track + cvmtx_abs lines:
// holds from the state, then the takeover, then apply() for every destination
// that has taken over. skip_src and re-arming on the machine's own events
// (track select, preset load) stay with the machine.
void ptab_mtx_init(const ptab_t *t, cvmtx_t *m, ptab_rt_t *rt);
void ptab_knobs(const ptab_t *t, cvmtx_t *m, const int cvm[8]);
