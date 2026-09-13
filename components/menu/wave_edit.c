// Shared waveform + crop strip — see wave_edit.h. Column descriptors, priority
// order and the banded DMA blit are Tape's (tape_menu.c); the view/zoom maths
// and the four-way cursor are the generalisation.
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "tft.h"
#include "tftspi.h"
#include "ui_events.h"
#include "wave_edit.h"

static const color_t WF_LIT   = {120, 200, 255};
static const color_t WF_DIM   = {40, 60, 80};
static const color_t CROP_COL = {0, 190, 220};
static const color_t GRID_COL = {35, 40, 52};
static const color_t BAR_COL  = {70, 80, 100};
static const color_t PH_COL   = {245, 245, 250};
static const color_t REC_COL  = {230, 60, 50};
static const color_t ORG_COL  = {200, 206, 218};
static const color_t GRAB_COL = {30, 215, 90};
static const color_t SEL_COL  = {235, 238, 245};

typedef struct { int16_t ph; uint8_t lit, crop, grid, box; } wcol_t;

// ---- view helpers -----------------------------------------------------------
static uint32_t vlen(const wave_edit_t *e)
{
    uint32_t v = e->view_len ? e->view_len : e->frames;
    if (v == 0) v = 1;
    return v;
}

int wave_edit_x(const wave_edit_t *e, uint32_t frame)
{
    uint32_t v = vlen(e);
    if (frame <= e->view0) return e->x;
    uint32_t rel = frame - e->view0;
    if (rel >= v) return e->x + e->w - 1;
    return e->x + (int)((uint64_t)rel * e->w / v);
}

static uint32_t x_to_frame(const wave_edit_t *e, int px)
{
    if (px <= e->x) return e->view0;
    uint64_t rel = (uint64_t)(px - e->x) * vlen(e) / (e->w ? e->w : 1);
    return e->view0 + (uint32_t)rel;
}

void wave_edit_zoom_all(wave_edit_t *e)
{
    e->view0 = 0;
    e->view_len = e->frames;
}

// keep the view inside the file, and the selected point inside the view
static void view_clamp(wave_edit_t *e)
{
    if (e->frames == 0) { e->view0 = 0; e->view_len = 0; return; }
    if (e->view_len == 0 || e->view_len > e->frames) e->view_len = e->frames;
    if (e->view_len < 64) e->view_len = 64;
    if (e->view0 + e->view_len > e->frames)
        e->view0 = (e->frames > e->view_len) ? e->frames - e->view_len : 0;
}

// scroll the view so `f` stays visible with a margin — an edit point dragged
// off the edge should pull the window along, not vanish
static void view_follow(wave_edit_t *e, uint32_t f)
{
    uint32_t v = vlen(e);
    uint32_t margin = v / 8;
    if (f < e->view0 + margin)
        e->view0 = (f > margin) ? f - margin : 0;
    else if (f > e->view0 + v - margin)
        e->view0 = f + margin > v ? f + margin - v : 0;
    view_clamp(e);
}

// ---- column description ------------------------------------------------------
static bool col_desc(const wave_edit_t *e, int x, wcol_t *d)
{
    memset(d, 0, sizeof(*d));
    if (x < e->x || x >= e->x + e->w) return false;
    int xi = wave_edit_x(e, e->in_pt), xo = wave_edit_x(e, e->out_pt);
    uint32_t fr = x_to_frame(e, x);

    // beat grid, only when ticks are far enough apart to read
    if (e->grid_frames) {
        uint32_t v = vlen(e);
        if ((uint64_t)e->grid_frames * e->w / v >= 4) {
            long b = (long)e->grid_frames;
            long rel = (long)fr - (long)e->grid_anchor;
            long bi = rel >= 0 ? rel / b : (rel - b + 1) / b;
            long tick = (long)e->grid_anchor + bi * b;
            int tx  = wave_edit_x(e, (uint32_t)(tick < 0 ? 0 : tick));
            int tx2 = wave_edit_x(e, (uint32_t)(tick + b));
            if (x == tx || x == tx2) {
                long bidx = (x == tx) ? bi : bi + 1;
                d->grid = (bidx % 4 == 0) ? 2 : 1;
            }
        }
    }

    // peaks are binned over the WHOLE file; the view is a window into it
    if (e->peaks && e->n_peaks > 0 && e->frames) {
        int pi = (int)((uint64_t)fr * e->n_peaks / e->frames);
        if (pi >= 0 && pi < e->n_peaks && e->peaks[pi]) {
            int ph = e->peaks[pi] * (e->h / 2 - 2) / 255;
            if (ph < 1) ph = 1;
            d->ph  = (int16_t)ph;
            d->lit = (fr >= e->in_pt && fr < e->out_pt) ? 1 : 0;
        }
    }

    if (x == xi || x == xo) d->crop = 1;

    // the loop box only while WIN is selected — the lit/dim split already shows
    // the window the rest of the time, so the box stays out of the way
    if (e->cursor == WE_WIN && x >= xi && x <= xo)
        d->box = (x < xi + 3 || x > xo - 3) ? 1 : 2;
    return true;
}

static color_t box_col(const wave_edit_t *e) { return e->grabbed ? GRAB_COL : SEL_COL; }

static void draw_col(wave_edit_t *e, int x)
{
    int y0 = e->y, h = e->h, cy = y0 + h / 2, yb = y0 + h - 1;
    wcol_t d;
    if (!col_desc(e, x, &d)) return;
    _bg = TFT_BLACK;
    TFT_fillRect(x, y0, 1, h, _bg);
    if (d.grid) TFT_drawLine(x, y0, x, y0 + h, d.grid == 2 ? BAR_COL : GRID_COL);
    if (d.ph)   TFT_drawLine(x, cy - d.ph, x, cy + d.ph, d.lit ? WF_LIT : WF_DIM);
    if (d.crop) TFT_drawLine(x, y0, x, yb, CROP_COL);
    if (d.box == 1) TFT_drawLine(x, y0, x, yb, box_col(e));
    else if (d.box == 2) {
        color_t bc = box_col(e);
        for (int k = 0; k < 3; k++) { TFT_drawPixel(x, y0 + k, bc, 1); TFT_drawPixel(x, yb - k, bc, 1); }
    }
    TFT_drawPixel(x, cy, ORG_COL, 1);
}

// the whole strip in ~18 transfers instead of w * 3-4 transactions
static bool draw_blit(wave_edit_t *e)
{
    const int x0 = e->x, wcols = e->w, y0 = e->y, h = e->h, cyr = h / 2;
    const int BAND = 6;
    color_t *buf  = heap_caps_malloc((size_t)wcols * BAND * sizeof(color_t), MALLOC_CAP_DMA);
    wcol_t  *desc = heap_caps_malloc((size_t)wcols * sizeof(wcol_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !desc) { free(buf); free(desc); return false; }
    for (int c = 0; c < wcols; c++) col_desc(e, x0 + c, &desc[c]);
    const color_t black = {0, 0, 0}, bc = box_col(e);
    for (int r0 = 0; r0 < h; r0 += BAND) {
        int rows = (h - r0 < BAND) ? (h - r0) : BAND;
        for (int r = 0; r < rows; r++) {
            int rr = r0 + r;
            color_t *row = buf + r * wcols;
            for (int c = 0; c < wcols; c++) {
                const wcol_t *d = &desc[c];
                color_t px = black;
                if (d->grid) px = (d->grid == 2) ? BAR_COL : GRID_COL;
                if (d->ph && rr >= cyr - d->ph && rr <= cyr + d->ph) px = d->lit ? WF_LIT : WF_DIM;
                if (d->crop) px = CROP_COL;
                if (d->box == 1 || (d->box == 2 && (rr < 3 || rr > h - 4))) px = bc;
                if (rr == cyr) px = ORG_COL;
                row[c] = px;
            }
        }
        if (disp_select() != ESP_OK) break;
        send_data(x0, y0 + r0, x0 + wcols - 1, y0 + r0 + rows - 1, (uint32_t)(wcols * rows), buf);
        disp_deselect();
    }
    free(buf);
    free(desc);
    return true;
}

// bold the selected crop edge (extra bold + green once grabbed). WIN is not
// handled here: its whole-loop box is the clearer cue and these bars would
// paint over it.
static void draw_sel(wave_edit_t *e)
{
    color_t hi = e->grabbed ? GRAB_COL : (color_t){150, 235, 255};
    int wdt = e->grabbed ? 5 : 4;
    if (e->cursor == WE_IN) {
        int x = wave_edit_x(e, e->in_pt);
        for (int k = 0; k < wdt && x + k < e->x + e->w; k++)
            TFT_drawLine(x + k, e->y, x + k, e->y + e->h - 1, hi);
    } else if (e->cursor == WE_OUT) {
        int x = wave_edit_x(e, e->out_pt);
        for (int k = 0; k < wdt && x - k >= e->x; k++)
            TFT_drawLine(x - k, e->y, x - k, e->y + e->h - 1, hi);
    } else if (e->cursor == WE_ZOOM) {
        // zoom has no point of its own — mark the whole view instead
        for (int k = 0; k < 2; k++) {
            TFT_drawLine(e->x, e->y + k, e->x + e->w - 1, e->y + k, hi);
            TFT_drawLine(e->x, e->y + e->h - 1 - k, e->x + e->w - 1, e->y + e->h - 1 - k, hi);
        }
    }
}

void wave_edit_draw(wave_edit_t *e)
{
    if (!e || e->w <= 0 || e->h <= 0) return;
    view_clamp(e);
    if (e->frames == 0) {
        _bg = TFT_BLACK;
        TFT_fillRect(e->x, e->y, e->w, e->h, TFT_BLACK);
        _fg = (color_t){90, 90, 100};
        const char *m = "no sample loaded";
        TFT_print((char *)m, e->x + (e->w - TFT_getStringWidth((char *)m)) / 2,
                  e->y + e->h / 2 - TFT_getfontheight() / 2);
        e->_last_ph = -1;
        return;
    }
    if (!draw_blit(e)) {
        TFT_fillRect(e->x, e->y, e->w, e->h, TFT_BLACK);
        for (int x = e->x; x < e->x + e->w; x++) draw_col(e, x);
    }
    draw_sel(e);
    e->_last_ph = -1;
}

void wave_edit_playhead(wave_edit_t *e)
{
    if (!e || e->frames == 0) return;
    int ph = (e->play == WE_NO_PLAY) ? -1 : wave_edit_x(e, e->play);
    if (ph == e->_last_ph) return;
    if (e->_last_ph >= 0 && e->_last_ph != ph) {
        draw_col(e, e->_last_ph);
        draw_sel(e);
    }
    if (ph >= 0) TFT_drawLine(ph, e->y, ph, e->y + e->h, e->rec ? REC_COL : PH_COL);
    e->_last_ph = ph;
}

// ---- input ------------------------------------------------------------------
// one detent = one pixel, so the feel does not change with the zoom
static uint32_t step_frames(const wave_edit_t *e)
{
    uint32_t s = vlen(e) / (uint32_t)(e->w > 0 ? e->w : 1);
    return s ? s : 1;
}

static uint32_t move_pt(wave_edit_t *e, uint32_t pt, int dir)
{
    uint32_t st = step_frames(e);
    long v = (long)pt + (long)dir * (long)st;
    if (v < 0) v = 0;
    if (v > (long)e->frames) v = (long)e->frames;
    uint32_t f = (uint32_t)v;
    if (e->snap) f = e->snap(f, dir);
    if (f > e->frames) f = e->frames;
    return f;
}

static int grabbed_turn(wave_edit_t *e, int dir)
{
    switch (e->cursor) {
        case WE_IN: {
            uint32_t f = move_pt(e, e->in_pt, dir);
            if (f >= e->out_pt) f = e->out_pt > 1 ? e->out_pt - 1 : 0;
            e->in_pt = f;
            view_follow(e, f);
            break;
        }
        case WE_OUT: {
            uint32_t f = move_pt(e, e->out_pt, dir);
            if (f <= e->in_pt) f = e->in_pt + 1;
            if (f > e->frames) f = e->frames;
            e->out_pt = f;
            view_follow(e, f);
            break;
        }
        case WE_WIN: {
            // slide the whole window, keeping its length
            uint32_t len = e->out_pt - e->in_pt;
            uint32_t st = step_frames(e);
            long v = (long)e->in_pt + (long)dir * (long)st;
            if (v < 0) v = 0;
            if (v + (long)len > (long)e->frames) v = (long)e->frames - (long)len;
            if (v < 0) v = 0;
            e->in_pt = (uint32_t)v;
            e->out_pt = e->in_pt + len;
            view_follow(e, e->in_pt);
            break;
        }
        case WE_ZOOM: {
            // geometric, anchored on the window so the selection stays put
            uint32_t v = vlen(e);
            uint32_t nv = (dir > 0) ? (v * 4) / 5 : (v * 5) / 4;
            if (nv < 64) nv = 64;
            if (nv > e->frames) nv = e->frames;
            uint32_t centre = e->in_pt + (e->out_pt - e->in_pt) / 2;
            e->view_len = nv;
            e->view0 = (centre > nv / 2) ? centre - nv / 2 : 0;
            view_clamp(e);
            break;
        }
        default: return WE_NONE;
    }
    return WE_MOVED;
}

int wave_edit_event(wave_edit_t *e, int event)
{
    if (!e) return WE_NONE;
    switch (event) {
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? 1 : -1;
            if (e->grabbed) {
                int r = grabbed_turn(e, dir);
                wave_edit_draw(e);
                return r;
            }
            e->cursor = (e->cursor + dir + WE_CURSOR_N) % WE_CURSOR_N;
            wave_edit_draw(e);
            return WE_CURSOR;
        }
        case EV_SHORT_PRESS:
            e->grabbed = !e->grabbed;
            wave_edit_draw(e);
            return WE_CURSOR;
        case EV_LONG_PRESS:
            // a hold escapes the grab rather than leaving the page — Tape's rule,
            // so a long press never both drops a point AND navigates away
            if (e->grabbed) { e->grabbed = false; wave_edit_draw(e); return WE_CURSOR; }
            return WE_EXIT;
        default:
            return WE_NONE;
    }
}
