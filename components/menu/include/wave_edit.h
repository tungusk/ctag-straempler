#pragma once
#include <stdint.h>
#include <stdbool.h>

// wave_edit — the shared WAVEFORM + CROP WINDOW strip.
//
// Lifted (2026-09-12) from Tape's live page, which had the only implementation:
// the column-peak waveform, the lit crop region, the crop edge ticks, the loop
// box, the playhead, and the grab modal where a turn moves an edit point
// instead of the selection. Tape earned that feel by being played; the widget
// exists so the Editor can have it too without a second copy diverging from it.
//
// The host owns the MATERIAL (the peak columns and the frame counts); the
// widget owns the VIEW, the edit points, the cursor, and every pixel of the
// strip. That split is what lets Tape drive it from a PSRAM bank and the Editor
// drive it from a file on the card.
//
// One addition Tape did not have: a zoom, because the Editor's files are
// minutes long and a whole-file x-axis makes a 200 ms edit unclickable. With
// view0 = 0 and view_len = frames the widget behaves exactly as Tape's did.

enum { WE_IN = 0, WE_OUT, WE_WIN, WE_ZOOM, WE_CURSOR_N };

#define WE_NO_PLAY 0xFFFFFFFFu

// wave_edit_event() return codes
enum {
    WE_NONE = 0,
    WE_MOVED,      // in_pt/out_pt/view changed — host should redraw its readout
    WE_CURSOR,     // selection or grab changed — strip already redrawn
    WE_EXIT,       // long press with nothing grabbed: host leaves the page
};

typedef struct wave_edit_s {
    // ---- material (host-owned, may change between draws) ----
    const uint8_t *peaks;        // n_peaks columns, 0..255, binned over `frames`
    int            n_peaks;
    uint32_t       frames;       // total length of the material

    // ---- view (owned by the widget, seeded by the host) ----
    uint32_t view0, view_len;    // visible span; view_len 0 = whole thing

    // ---- edit state ----
    uint32_t in_pt, out_pt;
    uint32_t play;               // playhead frame, WE_NO_PLAY for none
    int      cursor;             // WE_*
    bool     grabbed;            // a turn moves the point, not the selection
    bool     rec;                // playhead drawn in the record colour

    // ---- optional beat grid (0 = none) ----
    uint32_t grid_frames, grid_anchor;

    // ---- optional host snap (grid / zero-cross). NULL = raw frames ----
    uint32_t (*snap)(uint32_t frame, int dir);

    // ---- geometry ----
    int x, y, w, h;

    // ---- private ----
    int _last_ph;
} wave_edit_t;

// Full strip repaint. Uses the banded DMA blit when it can get the buffers and
// falls back to per-column drawing when it cannot.
void wave_edit_draw(wave_edit_t *e);

// Cheap playhead move: repaints only the column being left and the new one.
void wave_edit_playhead(wave_edit_t *e);

// Feed a menusys event. Grammar, unchanged from Tape's:
//   turn (loose)    cycle IN > OUT > WIN > ZOOM
//   press (loose)   grab the selected element (green = a turn moves it)
//   turn (grabbed)  move it, one detent per pixel, through snap()
//   press (grabbed) drop it
//   long            escape the grab if grabbed, else WE_EXIT
int wave_edit_event(wave_edit_t *e, int event);

// Frame -> screen x, for a host drawing its own marks over the strip.
int wave_edit_x(const wave_edit_t *e, uint32_t frame);

// Fit the view to the whole file (the host's "zoom out" action).
void wave_edit_zoom_all(wave_edit_t *e);
