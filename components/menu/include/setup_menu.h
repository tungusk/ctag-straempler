#pragma once
#include <stddef.h>

// Shared Setup-menu framework — the ONE place the house convention lives:
//   ST_TOGGLE  small option set  -> PRESS CYCLES to the next option (no edit mode)
//   ST_RANGE   wide value/range  -> PRESS enters a [ ] edit, TURN adjusts, press exits
//   ST_ACTION  list / sub-page   -> PRESS fires action() (returns a menu id to open)
// A machine declares a `setup_item_t[]` (label + kind) plus three index-based
// callbacks (render/adjust/action, mirroring the old setup_val/xxx_adj switches),
// wires a `setup_menu_t`, and its Setup handler is one line: `return
// setup_menu_event(&m, event);`. Rendering + scrolling + the grammar are shared.
typedef enum { ST_TOGGLE = 0, ST_RANGE, ST_ACTION } setup_kind_t;

typedef struct {
    const char  *label;
    setup_kind_t kind;
} setup_item_t;

typedef struct {
    const setup_item_t *items;
    int          n;
    const char  *title;        // page title, top-left
    const char  *aff_label;    // top affordance label (e.g. "Machine"); pos -1
    int          aff_target;   // menu id when the affordance is pressed
    int          live_target;  // menu id on long-press (back to Live)
    // machine callbacks, index-based:
    void (*render)(int i, char *buf, size_t n);  // value string for item i
    void (*adjust)(int i, int dir);              // TOGGLE: cycle; RANGE: step +/-
    int  (*action)(int i);                       // ACTION: press -> menu id (0 = stay)
    // runtime (owned by the framework):
    int pos, sel;
} setup_menu_t;

// Feed a menusys event. Returns a target menu id to navigate to, or 0 to stay.
int setup_menu_event(setup_menu_t *m, int event);
