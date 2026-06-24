#pragma once

// Callback for a value change: (sid, vid, delta +1/-1, opaque ctx)
typedef void (*menu_nav_change_fn)(int sid, int vid, int delta, void *ctx);

// Descriptor for a scrollable parameter menu.
// Handles FWD/BWD cursor movement and value editing, SHORT_PRESS toggle, LONG_PRESS exit.
typedef struct {
    const char         **labels;
    const int           *sids;
    int                  n;
    int                  parent;    // menu_ids_t to return on LONG_PRESS
    menu_nav_change_fn   change;
    void                *ctx;
} menu_nav_t;

// Call from a def_handler for every event. Returns 0 to stay, parent id to exit.
// EV_ENTERED_MENU is NOT handled here — caller draws the initial screen.
int menu_nav_step(menu_nav_t *nav, int *pos, int *sel, int vid, int event);
