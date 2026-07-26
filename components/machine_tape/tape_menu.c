// Tape UI — the screen IS the editor: one big waveform (the whole tape) with
// the crop window boxed bright, material outside dimmed, a beat grid anchored
// at the IN point, and a white (red while recording) playhead. Encoder: turn
// moves the selected cursor (grid-snapped when a clock/BPM is known, zero-
// cross otherwise), press cycles IN -> OUT -> WIN (slide both), hold = Setup.
// TR1 = play/stop, TR2 = record punch. Everything else lives in Setup.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "setup_menu.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "sample_browser.h"
#include "sample_ram.h"
#include "tape_priv.h"

static const color_t WF_DIM   = {70, 70, 80};     // outside the crop
static const color_t CROP_COL = {70, 200, 235};   // crop edge ticks (cyan)
static const color_t GRID_COL = {24, 30, 42};     // beat ticks (dim)
static const color_t BAR_COL  = {36, 48, 66};     // every 4th beat
static const color_t PH_COL   = {240, 240, 245};
static const color_t REC_COL  = {230, 60, 50};

// ---- geometry ----------------------------------------------------------------
#define W_X 8
#define W_W 300
// big filename up top (like the other machines), then a tall waveform, then a
// static button row along the bottom.
static int w_y(void) { return 47; }              // wave top (below Tape header + big name, + pad)
static int w_h(void) { return 106; }             // waveform height
static int crop_ry(void) { return 156; }         // crop readout (IN/OUT/beats)
static int strip_y(void) { return 176; }         // edit + FX button row

// on-screen elements the encoder scrolls (turn = move selection, press = act):
// the NAME title (= the Load affordance, upper-left in the wave), the crop points,
// then the edit actions. Pressing a crop point grabs it (turn adjusts, press drops).
// CW scroll order through the loop elements is START > WINDOW > END (Arlo
// 2026-07-25) — the window sits between the two points it slides, so turning
// right walks the loop left-edge, whole-loop, right-edge.
enum { TB_NAME = 0, TB_IN, TB_WIN, TB_OUT, TB_REV, TB_NORM, TB_FADE, TB_CLR,
       TB_FX1, TB_FX2, TB_FX3, TB_ROUTE, TB_N };
static int  s_btn  = 0;               // selected element
static bool s_grab = false;           // a crop point is grabbed -> turn adjusts it
static int  s_cur_slot = 0;           // FX slot the sub-page edits (shared with Setup FX rows)
static int  s_setup_return = -1;      // Setup row to restore on return from a sub-page
static bool tb_is_crop(int b) { return b >= TB_IN && b <= TB_OUT; }

// The Live button strip is a fixed GRID, sized for growth (Arlo 2026-07-25:
// "we'll be making room for more cells tho, prepare for like two rows of six").
// Cell positions are pinned by this table rather than by scroll order, so a new
// button is one entry here plus its TB_* id — nothing re-flows. -1 = reserved
// space. Row 1 = tape edits, row 2 = the FX chain; the spares sit at the end of
// each row so each group grows into its own gap.
#define STRIP_COLS 4          // bump to 6 when the new cells land
#define STRIP_ROWS 2
static const int8_t strip_cell[STRIP_ROWS][STRIP_COLS] = {
    { TB_REV, TB_NORM, TB_FADE, TB_CLR   },
    { TB_FX1, TB_FX2,  TB_FX3,  TB_ROUTE },
};
static bool tb_is_fx(int b)   { return b >= TB_FX1 && b <= TB_FX3; }
static bool tp_ui_stopped(void) { return !tp.playing && !tp.recording; }

// waveform lit-colour reflects transport state (no box border): red recording,
// green playing, blue stopped.
static color_t wave_lit(void)
{
    if (tp.recording) return (color_t){255, 40, 40};    // saturated red
    if (tp.playing)   return (color_t){30, 235, 80};    // saturated green
    return (color_t){35, 120, 255};                     // saturated blue
}

static int s_last_ph = -1;
static int s_last_state = -1;         // last transport state for the border
static unsigned s_sig_head = 0, s_sig_crop = 0, s_sig_name = 0;
static uint32_t s_wave_len = 0;       // len at last wave draw (record growth)

// view spans the RECORDED length (zoom to content), not the full capacity — a
// short take fills the bar instead of being a sliver on a 30 s tape.
static uint32_t tape_view_span(void) { return tp.len ? tp.len : tp.cap; }
static int frame_x(uint32_t fr)
{
    uint32_t span = tape_view_span();
    if (span == 0) return W_X;
    if (fr > span) fr = span;
    return W_X + (int)((uint64_t)fr * W_W / span);
}

static void fmt_secs(uint32_t fr, char *b, size_t n)
{
    snprintf(b, n, "%.2fs", (float)fr / (float)TP_RATE);
}

// ---- drawing -------------------------------------------------------------------
// The top bar is TWO independently-repainted bands (split 2026-07-25). Row A
// (status) ticks with the playhead ~10x/s; row B (the big filename) is static.
// They used to share one draw_header() that cleared the whole bar, so the name
// was erased and redrawn 10 times a second during playback — that was the
// "flashing filename". Each band now clears only its OWN rows.
#define HDR_A_H 13                                 // row A band: y 0..12 (row B starts at 13)

// the big filename in row B — also the Load affordance
static const char *header_name(char *buf, size_t n)
{
    if (tp.rec_dest == TPD_CARD) return "Card Record";
    if (tp.len == 0)             return "Blank Tape";
    if (tp.restore_id[0])        return tp.restore_id;
    snprintf(buf, n, "REC-%03d", tp.take_num);
    return buf;
}

// row A: machine name (left) + state/pos/bpm (right), small. Repainted on the
// fast tick — must NOT touch row B's rows.
static void draw_header_status(void)
{
    _bg = TFT_BLACK; TFT_fillRect(0, 0, _width, HDR_A_H, _bg);
    TFT_setFont(DEF_SMALL_FONT, NULL);
    _fg = (color_t){150, 155, 172}; TFT_print("Tape", 6, 2);
    tape_beat_frames();
    const char *st = tp.recording ? "REC" : tp.playing ? "PLAY" : "STOP";
    char b[48];                                      // compact status (elapsed time); beats live UNDER THE BAR while recording
    if (tp.rec_dest == TPD_CARD) snprintf(b, sizeof(b), "%s  %.1fs   %.0f %s", st,
        (float)tp.pos / TP_RATE, tp.disp_bpm, tp.disp_clk ? "CLK" : "man");
    else snprintf(b, sizeof(b), "%s  %.1f/%.0fs   %.0f %s", st,
        (float)tp.pos / TP_RATE, (float)tp.cap / TP_RATE, tp.disp_bpm, tp.disp_clk ? "CLK" : "man");
    _fg = tp.recording ? REC_COL : tp.playing ? (color_t){40, 200, 90} : (color_t){140, 145, 160};
    TFT_print(b, _width - 6 - TFT_getStringWidth(b), 2);
    TFT_setFont(DEFAULT_FONT, NULL);
}

// row B: the big filename + saved check + selection box. Repainted only when its
// CONTENT changes (see name_sig), never on the playhead tick.
static void draw_header_name(void)
{
    // clear to the waveform top (not w_y()-2): the selection box needs the extra
    // rows, and anything drawn below the cleared band becomes a STALE line that
    // nothing erases — which is exactly how the thick box left an "underline"
    _bg = TFT_BLACK; TFT_fillRect(0, HDR_A_H, _width, w_y() - HDR_A_H, _bg);
    char nb[24];
    const char *nm = header_name(nb, sizeof(nb));
    bool sel = (s_btn == TB_NAME) && tp.rec_dest == TPD_TAPE;
    bool saved = tp.restore_id[0] != 0 && tp.rec_dest == TPD_TAPE;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bh = TFT_getfontheight(), nw = TFT_getStringWidth((char *)nm);
    int nx = 8, ny = 16;                           // +1px pad above the title
    // the name is ALWAYS bright white (Arlo 2026-07-25) — the box carries the
    // selection on its own, so recolouring the text too was just noise
    _fg = (color_t){245, 247, 250};
    TFT_print((char *)nm, nx, ny);
    if (saved) {                                   // green check after the name
        int cx = nx + nw + 8, cy = ny + bh / 2; color_t g = {70, 220, 120};
        TFT_drawLine(cx, cy, cx + 4, cy + 5, g);   TFT_drawLine(cx + 1, cy, cx + 5, cy + 5, g);
        TFT_drawLine(cx + 4, cy + 5, cx + 11, cy - 5, g); TFT_drawLine(cx + 5, cy + 5, cx + 12, cy - 5, g);
    }
    if (sel) {                                     // selection outline (3px, Arlo 2026-07-25)
        int bw = nw + (saved ? 26 : 12);
        color_t gr = {120, 124, 138};
        // nest INWARD from the original 1px bounds — growing outward pushed the
        // bottom edge past the cleared band and left it on screen as an underline
        for (int k = 0; k < 3; k++) TFT_drawRect(3 + k, ny - 3 + k, bw - 2 * k, bh + 6 - 2 * k, gr);
    }
    TFT_setFont(DEFAULT_FONT, NULL);
}

// changes exactly when row B's pixels would change
static unsigned name_sig(void)
{
    char nb[24];
    const char *nm = header_name(nb, sizeof(nb));
    unsigned h = 2166136261u;
    for (const char *p = nm; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return h ^ ((s_btn == TB_NAME) ? 2u : 0u)
             ^ (tp.restore_id[0] ? 4u : 0u)
             ^ ((unsigned)tp.rec_dest << 3);
}

static void draw_header(void) { draw_header_status(); draw_header_name(); }

// one waveform column (also used to erase the playhead)
static void wave_col(int x)
{
    int y0 = w_y(), h = w_h(), cy = y0 + h / 2;
    if (x < W_X || x >= W_X + W_W) return;
    _bg = TFT_BLACK; TFT_fillRect(x, y0, 1, h, _bg);
    uint32_t ein, eout; tape_eff_window(&ein, &eout);
    int xi = frame_x(ein), xo = frame_x(eout);
    // grid tick at this column?
    if (tp.len) {
        uint32_t b = tape_beat_frames();
        if (b > 4410 / 2) {                     // draw grid only when ticks >= ~4px apart
            long fr = (long)((uint64_t)(x - W_X) * tape_view_span() / W_W);
            long rel = fr - (long)tp.in_pt;
            long bi = rel >= 0 ? rel / (long)b : (rel - (long)b + 1) / (long)b;
            long tick = (long)tp.in_pt + bi * (long)b;
            int tx = frame_x((uint32_t)(tick < 0 ? 0 : tick));
            int tx2 = frame_x((uint32_t)(tick + (long)b));
            if (x == tx || x == tx2) {
                long bidx = (x == tx) ? bi : bi + 1;
                TFT_drawLine(x, y0, x, y0 + h, (bidx % 4 == 0) ? BAR_COL : GRID_COL);
            }
        }
    }
    // column -> frame (via the zoomed view span) -> cap-binned peak index, so the
    // waveform lines up with the crop/playhead (peaks are binned over tp.cap).
    long fr = (long)((uint64_t)(x - W_X) * tape_view_span() / W_W);
    int pi = tp.cap ? (int)((uint64_t)fr * TP_PEAKS / tp.cap) : 0;
    if (pi >= 0 && pi < TP_PEAKS && tp.peaks[pi]) {
        int ph = tp.peaks[pi] * (h / 2 - 2) / 255;
        if (ph < 1) ph = 1;
        // lit region = the crop; but a fresh take has no real crop until punch-out
        // (raw out_pt == in_pt), so while recording light the whole recorded extent
        // so the red state colour shows.
        bool crop_set = tp.out_pt > tp.in_pt;
        bool inside = crop_set ? (fr >= (long)ein && fr < (long)eout)
                               : (tp.recording && fr < (long)tp.len);
        TFT_drawLine(x, cy - ph, x, cy + ph, inside ? wave_lit() : WF_DIM);
    }
    // always-on crop edge ticks (1px cyan) — the loop's boundaries stay marked
    // even when nothing is selected; the box and draw_crop_sel paint over them
    if (x == xi || x == xo) TFT_drawLine(x, y0, x, y0 + h - 1, CROP_COL);
    // LOOP BOX (Arlo 2026-07-25): a thick outline around the loop area, drawn
    // ONLY while the Window element is selected — WHITE while merely
    // highlighted, GREEN once grabbed, so green always means "turning the
    // encoder moves this". The rest of the time the lit-vs-dim waveform already
    // shows the loop, so the box stays out of the way. Drawn per-column so the
    // playhead erase (which repaints single columns) can't punch holes in the
    // top/bottom edges; kept inside y0..y0+h-1, the band wave_col clears.
    if (s_btn == TB_WIN && x >= xi && x <= xo) {
        const int BOX_T = 3;                        // outline thickness (px)
        color_t bc = s_grab ? (color_t){30, 215, 90} : (color_t){235, 238, 245};
        int yb = y0 + h - 1;
        if (x < xi + BOX_T || x > xo - BOX_T) TFT_drawLine(x, y0, x, yb, bc);
        else for (int k = 0; k < BOX_T; k++) {
            TFT_drawPixel(x, y0 + k, bc, 1);
            TFT_drawPixel(x, yb - k, bc, 1);
        }
    }
    TFT_drawPixel(x, cy, (color_t){200, 206, 218}, 1);   // white origin line (over the wave centre)
}

// bold the selected crop edge so it's obvious which point you're editing
// (extra-bold + green while grabbed). WIN is deliberately NOT handled here — the
// whole-loop box in wave_col turns green for it, which is the clearer cue and
// would otherwise be painted over by these bars.
static void draw_crop_sel(void)
{
    if (tp.len == 0 || tp.rec_dest != TPD_TAPE) return;
    int y0 = w_y(), h = w_h();
    uint32_t ein, eout; tape_eff_window(&ein, &eout);
    color_t hi = s_grab ? (color_t){25, 175, 70}     // grabbed (clicked in to move) = green
                        : (color_t){150, 235, 255};  // selected = bright cyan
    int wdt = s_grab ? 5 : 4;                        // bold selected crop edge
    if (s_btn == TB_IN) {
        int x = frame_x(ein);
        for (int k = 0; k < wdt && x + k < W_X + W_W; k++) TFT_drawLine(x + k, y0, x + k, y0 + h - 1, hi);
    }
    if (s_btn == TB_OUT) {
        int x = frame_x(eout);
        for (int k = 0; k < wdt && x - k >= W_X; k++) TFT_drawLine(x - k, y0, x - k, y0 + h - 1, hi);
    }
}

static void draw_wave(void)
{
    int y0 = w_y(), h = w_h();
    _bg = TFT_BLACK; TFT_fillRect(W_X - 2, y0, W_W + 4, h, _bg);
    if (tp.len == 0) {
        if (tp.tr2_armed) {                            // long-press erased, waiting for release
            const char *m = "Armed: Release to Record!";
            _fg = (color_t){60, 220, 120};
            TFT_print((char *)m, W_X + (W_W - TFT_getStringWidth((char *)m)) / 2, y0 + h / 2 - TFT_getfontheight() / 2);
        } else if (tp.rec_dest == TPD_CARD) {
            _fg = (color_t){90, 90, 100};
            TFT_print("CARD RECORD - TR2 punches a long take", W_X + 16, y0 + h / 2 - TFT_getfontheight());
            TFT_print("straight to the card (no 30s limit)", W_X + 16, y0 + h / 2 + 2);
        } else {
            _fg = (color_t){90, 90, 100};
            TFT_print("TR2 records line-in  -  or select the", W_X + 16, y0 + h / 2 - TFT_getfontheight());
            TFT_print("name up top to load a sample", W_X + 16, y0 + h / 2 + 2);
        }
        s_last_ph = -1;
        s_wave_len = 0;
        return;
    }
    for (int x = W_X; x < W_X + W_W; x++) wave_col(x);
    draw_crop_sel();
    s_last_ph = -1;
    s_wave_len = tp.len;
}

static void draw_playhead(void)
{
    int y0 = w_y(), h = w_h();
    int ph = -1;
    if ((tp.len || tp.recording) && tp.rec_dest == TPD_TAPE) ph = frame_x((uint32_t)tp.pos);
    if (ph == s_last_ph && !tp.recording) return;
    if (s_last_ph >= 0 && s_last_ph != ph) wave_col(s_last_ph);
    if (ph >= 0) TFT_drawLine(ph, y0, ph, y0 + h, tp.recording ? REC_COL : PH_COL);
    s_last_ph = ph;
}

// crop readout below the wave: IN / OUT / beats, the selected point emphasised
// (crop points are selected "in place" by scrolling, not via buttons).
static void draw_crop_readout(void)
{
    int y = crop_ry();
    _bg = TFT_BLACK; TFT_fillRect(0, y, _width, TFT_getfontheight() + 3, _bg);
    if (tp.rec_dest == TPD_CARD) return;
    uint32_t bt = tape_beat_frames();
    if (tp.recording) {                              // while recording, the crop is meaningless —
        // show live elapsed time + beats, COLUMN-ALIGNED with the IN/OUT/beats
        // readout below (so the numbers don't jump when recording stops).
        int xout   = 6 + TFT_getStringWidth("IN 00.00s") + 16;      // OUT-segment column
        int xbeats = xout + TFT_getStringWidth("OUT 00.00s") + 16;  // beats column
        char t[16]; _fg = REC_COL;
        TFT_print("REC", 6, y);                                     // where "IN" sits
        snprintf(t, sizeof(t), "%.2fs", (float)tp.pos / TP_RATE);
        TFT_print(t, xout + TFT_getStringWidth("OUT "), y);         // under the OUT time
        if (tp.disp_bpm > 0 && bt) {
            snprintf(t, sizeof(t), "%.1f beats", (float)tp.pos / (float)bt);
            TFT_print(t, xbeats, y);                                // under the crop beats
        }
        return;
    }
    if (tp.len == 0) return;
    char a[16], b2[16]; fmt_secs(tp.in_pt, a, sizeof(a)); fmt_secs(tp.out_pt, b2, sizeof(b2));
    float beats = bt ? (float)(tp.out_pt - tp.in_pt) / (float)bt : 0;
    char seg[28]; int x = 6;
    _fg = (s_btn == TB_IN) ? TFT_CYAN : (color_t){150, 150, 162};
    snprintf(seg, sizeof(seg), "IN %s", a);  TFT_print(seg, x, y); x += TFT_getStringWidth(seg) + 16;
    _fg = (s_btn == TB_OUT) ? TFT_CYAN : (color_t){150, 150, 162};
    snprintf(seg, sizeof(seg), "OUT %s", b2); TFT_print(seg, x, y); x += TFT_getStringWidth(seg) + 16;
    _fg = (s_btn == TB_WIN) ? TFT_CYAN : (color_t){110, 112, 124};
    snprintf(seg, sizeof(seg), "%.1f beats", beats); TFT_print(seg, x, y);
    // crop-drop confirmation, right-justified on the same row (the drop itself
    // changes nothing on screen, so this is the whole feedback channel)
    if (tp.drop_ticks > 0 && tp.drop_note[0]) {
        char d[20];
        bool bad = tp.drop_spoiled && !tp.save_busy;    // recorded over mid-write
        snprintf(d, sizeof(d), tp.save_busy ? ">%s..." : bad ? "!%s" : ">%s", tp.drop_note);
        _fg = bad ? (color_t){235, 165, 60}             // amber = the file is a blend
                  : (color_t){120, 225, 150};           // green = it went to the card
        TFT_print(d, _width - 6 - TFT_getStringWidth(d), y);
    }
}

// the button row: edit actions + FX slots. Static (no scroll), selected gets a
// filled highlight (no outline). Crop points are NOT here (selected in place).
static void draw_buttons(void)
{
    int fh = TFT_getfontheight();
    int y = strip_y();
    _bg = TFT_BLACK; TFT_fillRect(0, y - 2, _width, fh * 2 + 8, _bg);
    if (tp.rec_dest == TPD_CARD) return;

    // TB_CLR slot repurposed 2026-07-20 (Arlo): Live gets the crop — Clear was
    // destructive + rarely wanted here; the full wipe stays on Setup > Clear Tape.
    //
    // TWO ROWS OF FOUR (Arlo 2026-07-25: "the rows are getting crowded... we can
    // use two rows for menu options and spread it out"). Same vertical space the
    // old label-over-value strip used, but each cell is ~75px instead of ~37, and
    // the grouping reads: row 1 = tape edits, row 2 = the FX chain.
    //
    // Each cell is ONE line. An FX slot shows its EFFECT NAME, which is self
    // explanatory, and falls back to "FX1/2/3" only when the slot is empty — so
    // the slot number appears exactly when the name would otherwise say "Off".
    static const char *const lab[] = { "Rev", "Norm", "Fade", "Crop", "FX1", "FX2", "FX3", "" };
    _bg = TFT_BLACK;
    bool stopped = tp_ui_stopped();
    const color_t C_SEL  = {255, 255, 255};
    const color_t C_DIM  = {78, 84, 98};
    const color_t C_DEAD = {45, 49, 60};
    int bw = W_W / STRIP_COLS;                // ~50 px per cell
    char tb[12];
    for (int r = 0; r < STRIP_ROWS; r++)
    for (int c = 0; c < STRIP_COLS; c++) {
        int b = strip_cell[r][c];
        if (b < 0) continue;                  // reserved, nothing here yet
        int si = b - TB_REV;
        int bx = W_X + c * bw;
        int by = y + 1 + r * (fh + 2);
        bool sel = (b == s_btn);              // highlight = bright text, no box
        const char *txt;
        if (b == TB_ROUTE) {                  // FX route: value only, no label
            txt = TPFX_NAMES[tp.fx_route];
            _fg = sel  ? C_SEL
                : tp.fx_route == TPFX_OFF  ? C_DEAD
                : tp.fx_route == TPFX_POST ? (color_t){150, 200, 120}   // live, tint it
                                           : C_DIM;
        } else if (b >= TB_FX1) {             // FX slot: the effect name IS the label
            const char *nm = fxrack_slot_name(&tp_rk, b - TB_FX1);
            bool off = (strcmp(nm, "Off") == 0);
            snprintf(tb, sizeof(tb), "%.9s", off ? lab[si] : nm);   // 9 chars fits a 75px cell
            txt = tb;
            _fg = sel ? C_SEL : off ? C_DEAD : (color_t){88, 118, 132};
        } else {                              // tape edit action
            // Rev/Norm/Fade mutate the buffer and can't fire while the transport
            // runs — draw them extra-dim so the row SHOWS what's live. Crop stays
            // available (read-only), which is what makes it findable mid-loop.
            bool dead = !stopped && b <= TB_FADE;
            txt = lab[si];
            _fg = dead ? C_DEAD : sel ? C_SEL : C_DIM;
        }
        TFT_print((char *)txt, bx + 3, by);
    }
}

// one-line hint for the selected element
static void draw_hint(void)
{
    int y = strip_y() + (TFT_getfontheight() * 2 + 6);
    TFT_setFont(DEF_SMALL_FONT, NULL);
    _bg = TFT_BLACK; TFT_fillRect(0, y, _width, TFT_getfontheight() + 2, _bg);
    bool stopped = tp_ui_stopped();
    const char *h = "";
    if (tp.rec_dest == TPD_CARD) h = "";
    else if (s_grab)             h = "turn: move the point    press: drop";
    else switch (s_btn) {
        case TB_NAME: h = "press: load a sample"; break;
        case TB_IN:   h = "press: grab the IN point"; break;
        case TB_OUT:  h = "press: grab the OUT point"; break;
        case TB_WIN:  h = "press: grab & slide the window"; break;
        case TB_REV:  h = stopped ? "press: reverse the crop"   : "stop first"; break;
        case TB_NORM: h = stopped ? "press: normalize the crop" : "stop first"; break;
        case TB_FADE: h = stopped ? "press: fade crop edges"    : "stop first"; break;
        case TB_CLR:  h = tp.recording ? "recording"
                        : tp.save_busy ? "saving..."
                                       : "press: save the loop + crop to it"; break;
        case TB_FX1: case TB_FX2: case TB_FX3: h = "press: cycle FX   long: Setup"; break;
        case TB_ROUTE:
            h = tp.fx_route == TPFX_POST ? "post: FX live, flips to pre on record"
              : tp.fx_route == TPFX_OFF  ? "off: FX chain bypassed"
                                         : "pre: FX printed to tape on record"; break;
    }
    _fg = (color_t){110, 110, 120};
    if (h[0]) TFT_print((char *)h, 6, y);
    // compact transport cue on the right (TR1 play/stop, TR2 record)
    const char *tr = (tp.rec_dest == TPD_CARD) ? "TR2:rec"
                   : tp.recording ? "TR1/2:stop"
                   : tp.playing   ? "TR1:stop TR2:overdub"
                   : tp.len       ? "TR1:play TR2:new"
                                  : "TR1:play TR2:rec";
    _fg = (color_t){90, 95, 110};
    TFT_print((char *)tr, _width - 6 - TFT_getStringWidth((char *)tr), y);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static unsigned head_sig(void)
{
    // position at 0.1 s resolution so the readout visibly counts while rolling
    return (tp.playing ? 1u : 0u) + (tp.recording ? 2u : 0u)
         + (unsigned)tp.disp_bpm * 8u + ((unsigned)((float)tp.pos * 10.0f / TP_RATE) << 8);
}
static unsigned crop_sig(void)
{
    uint32_t ein, eout; tape_eff_window(&ein, &eout);
    return ein * 2654435761u ^ eout * 40503u ^ ((unsigned)s_btn << 3) ^ (s_grab ? 1u : 0u);
}

static void redraw_strip(void) { draw_crop_readout(); draw_buttons(); draw_hint(); }

// Crop button: save the audible loop as a take, then crop the tape to it. The
// note names the file that was written and lives ~6 SLOW ticks (~6 s); the crop
// itself lands a moment later, when tape_drop_adopt_kick() sees the file closed.
// State only: each caller repaints its OWN page (this runs from the Live button
// AND the Setup row, so it must not draw Live geometry).
#define DROP_NOTE_TICKS 6
static bool s_drop_busy = false;        // last save_busy seen while a note is up
static void crop_drop(void)
{
    if (tape_save_crop() == 0) snprintf(tp.drop_note, sizeof(tp.drop_note), "%s", tp.save_id);
    else                       snprintf(tp.drop_note, sizeof(tp.drop_note), "%s",
                                       tp.recording ? "rec" : tp.save_busy ? "busy" : "--");
    tp.drop_ticks = DROP_NOTE_TICKS;
    s_drop_busy   = tp.save_busy;
}

static void main_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    draw_header();  s_sig_head = head_sig(); s_sig_name = name_sig();
    draw_wave();                         // includes the crop highlight
    draw_playhead();
    redraw_strip(); s_sig_crop = crop_sig();
}

// ---- crop-point edit (only while a crop element is grabbed) --------------------
static void nudge(int dir)
{
    if (tp.len == 0) return;
    uint32_t bt = tape_beat_frames();
    long step = (tp.disp_bpm > 0) ? (long)bt : (long)(tp.len / 200 + 1);
    long d = (long)dir * step;
    // switch on the element itself, not on its position in the enum — the scroll
    // order is a UI choice and has been reordered once already
    if (s_btn == TB_IN) {               // IN (grid re-anchors with it)
        long v = (long)tp.in_pt + d;
        v = v < 0 ? 0 : v;
        if (v > (long)tp.out_pt - 64) v = (long)tp.out_pt - 64;
        if (tp.disp_bpm <= 0) v = (long)tape_snap((uint32_t)(v < 0 ? 0 : v));
        tp.in_pt = (uint32_t)(v < 0 ? 0 : v);
    } else if (s_btn == TB_OUT) {       // OUT (snaps to the IN-anchored grid)
        long v = (long)tp.out_pt + d;
        if (v < (long)tp.in_pt + 64) v = (long)tp.in_pt + 64;
        if (v > (long)tp.len) v = (long)tp.len;
        v = (long)tape_snap((uint32_t)v);
        if (v <= (long)tp.in_pt) v = (long)tp.in_pt + 64;
        if (v > (long)tp.len) v = (long)tp.len;
        tp.out_pt = (uint32_t)v;
    } else {                            // WIN: slide both, width fixed
        long w = (long)tp.out_pt - (long)tp.in_pt;
        long i = (long)tp.in_pt + d;
        if (i < 0) i = 0;
        if (i + w > (long)tp.len) i = (long)tp.len - w;
        if (i < 0) i = 0;
        tp.in_pt = (uint32_t)i;
        tp.out_pt = (uint32_t)(i + w);
    }
    tp.cropped = true;                  // user shaped the crop -> auto-save marks it (TCR_)
}

static int tape_main_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU:
            if (tp.restore_pending) {              // reload last take (loop mode only)
                tp.restore_pending = false;
                if (tp.rec_dest == TPD_TAPE) tape_restore_last();
            }
            main_full_redraw();
            break;
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? +1 : -1;
            if (s_grab && tb_is_crop(s_btn)) nudge(dir);              // move the grabbed point
            else {
                // scroll the selection, SKIPPING the DESTRUCTIVE edit actions
                // (Rev/Norm/Fade) while rolling — they can't fire, so don't make
                // the user turn past dead buttons. Crop is NOT skipped: it is a
                // read-only drop and is meant to be pressed mid-loop.
                do { s_btn = (s_btn + dir + TB_N) % TB_N; }
                while (!tp_ui_stopped() && s_btn >= TB_REV && s_btn <= TB_FADE);
            }
            draw_header(); draw_wave(); redraw_strip(); s_sig_crop = crop_sig(); s_sig_name = name_sig();  // header = name-select highlight
            break;
        }
        case EV_SHORT_PRESS:
            if (s_grab) s_grab = false;                              // drop the crop point
            else if (tb_is_crop(s_btn)) s_grab = true;               // grab it
            else if (s_btn == TB_NAME) return M_TAPE_LOAD;           // the name IS the load button
            else if (tb_is_fx(s_btn))                               // cycle this slot's effect IN PLACE
                fxrack_menu_adj(&tp_rk, s_btn - TB_FX1, -1, +1);    //   (params: long-press -> Setup > FX)
            // Crop = drop the audible loop to a new take. Read-only, so it fires
            // WHILE PLAYING too (that's the point); tape_save_crop refuses on its
            // own while recording.
            else if (s_btn == TB_ROUTE) tp.fx_route = (tp.fx_route + 1) % TPFX_N;  // pre > post > auto
            else if (s_btn == TB_CLR) crop_drop();
            else if (tp_ui_stopped()) switch (s_btn) {              // buffer edits: STOPPED only (enforce the "stop first" hint, don't just show it)
                case TB_REV:  tape_reverse(); break;
                case TB_NORM: tape_norm();    break;
                case TB_FADE: tape_fade();    break;
            }
            draw_header(); draw_wave(); redraw_strip(); s_sig_crop = crop_sig(); s_sig_name = name_sig();
            break;
        case EV_LONG_PRESS:
            if (s_grab) { s_grab = false; draw_wave(); redraw_strip(); break; }   // escape grab
            return M_TAPE_SETUP;
        case EV_TIMER_REPEATING_FAST: {
            // cheap + frequent: services (auto-save + card recorder must stay
            // responsive), the header/beat readout, and the playhead. The heavy
            // waveform redraws are gated to the SLOW tick below — rebuild_peaks +
            // a full-strip redraw and its PSRAM shadow-FB writes contend with the
            // audio path (screen is 2nd priority to audio here).
            tape_autosave_kick();
            tape_drop_adopt_kick();          // finish a crop once its file is closed
            tape_card_service();
            unsigned hs = head_sig();
            if (hs != s_sig_head) {
                draw_header_status();                    // STATUS row only — repainting the
                s_sig_head = hs;                         //   name here is what made it flash
                if (tp.recording) draw_crop_readout();   // live elapsed/beats under the bar
            }
            unsigned ns = name_sig();                    // cheap; no TFT writes unless it moved
            if (ns != s_sig_name) { draw_header_name(); s_sig_name = ns; }
            draw_playhead();
            break;
        }
        case EV_TIMER_REPEATING_SLOW: {
            tape_autosave_kick();
            tape_drop_adopt_kick();          // finish a crop once its file is closed
            tape_card_service();
            if (tp.recording && tp.len != s_wave_len) {  // live record growth (throttled to 1 Hz)
                tape_rebuild_peaks(false);
                draw_wave();
            }
            int st = tp.recording ? 2 : tp.playing ? 1 : 0;
            if (st != s_last_state) {                     // state changed -> recolor the waveform
                draw_wave(); redraw_strip(); s_last_state = st;
            }
            unsigned cs = crop_sig();
            if (cs != s_sig_crop) { draw_wave(); redraw_strip(); s_sig_crop = cs; }
            // expire the crop-drop note, repainting when the writer finishes so
            // ">TCR_0007..." loses its ellipsis the moment the file is closed
            if (tp.drop_ticks > 0) {
                bool sb = tp.save_busy;
                if (--tp.drop_ticks == 0 || sb != s_drop_busy) redraw_strip();
                s_drop_busy = sb;
            }
            draw_playhead();
            break;
        }
        default: break;
    }
    return 0;
}

// ---- Setup (shared framework) ------------------------------------------------
static const int BEAT_LADDER[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
#define BEAT_LADDER_N 10
static int s_beats_idx = 3;           // "4 beats"

// Tape/transport params + edit actions live here; the FX rack lives on three
// slot rows (FX1/FX2 generic + FX3 reverb), each opening the shared dynamic
// per-slot sub-page (Effect select + its params). Keeps this list readable.
static const setup_item_t tape_setup_items[] = {
    {"Crop Beats", ST_RANGE},  {"Clock Src",  ST_TOGGLE}, {"Manual BPM", ST_RANGE},
    {"Filter",     ST_TOGGLE}, {"Cutoff",     ST_RANGE},  {"Reso",       ST_RANGE},
    {"Drive",      ST_RANGE},  {"Level",      ST_RANGE},  {"Rec Source", ST_TOGGLE},
    {"Monitor",    ST_TOGGLE},
    {"FX1",        ST_ACTION}, {"FX2",        ST_ACTION}, {"FX3 Reverb", ST_ACTION},
    {"Copy",       ST_ACTION}, {"Cut",        ST_ACTION}, {"Paste",      ST_ACTION},
    {"Normalize",  ST_ACTION}, {"Reverse",    ST_ACTION}, {"Fade Edges", ST_ACTION},
    {"Crop Loop",  ST_ACTION}, {"Load Sample", ST_ACTION}, {"Clear Tape", ST_ACTION},
    {"Tape Len",   ST_TOGGLE}, {"Rec Mode",    ST_TOGGLE}, {"Play Mode",   ST_TOGGLE},
    {"Rec Dest",   ST_TOGGLE}, {"Rec Quant",   ST_TOGGLE},
    {"CV Matrix",  ST_ACTION},   // appended: the index-keyed switches above stay stable
    {"FX Route",   ST_TOGGLE},   // 28: pre (printed to tape) vs post (output only)
};

// (FX rack slot editor state s_cur_slot / s_setup_return declared up top)

static const char *stopped_or(const char *s) { return (!tp.playing && !tp.recording) ? s : "stop first"; }

static void tape_val(int i, char *v, size_t n)
{
    switch (i) {
        case 0: snprintf(v, n, "%d", BEAT_LADDER[s_beats_idx]); break;
        case 1: snprintf(v, n, "%s", clock_source_name(tp.clk_src)); break;
        case 2: snprintf(v, n, "%.0f", tp.manual_bpm); break;
        case 3: snprintf(v, n, "%s", tp.flt_mode == TPF_LP ? "LP" : tp.flt_mode == TPF_BP ? "BP" :
                                     tp.flt_mode == TPF_HP ? "HP" : "off"); break;
        case 4: {
            if (tp.cutoff >= 1000) snprintf(v, n, "%.1fk", tp.cutoff / 1000);
            else                   snprintf(v, n, "%.0f", tp.cutoff);
            break;
        }
        case 5: snprintf(v, n, "%.0f%%", tp.res01 * 100); break;
        case 6: snprintf(v, n, "%.0f%%", tp.drive * 100); break;
        case 7: snprintf(v, n, "%.0f%%", tp.level * 100); break;
        case 8: snprintf(v, n, "%s", tp.rec_src == TPS_TAPE ? "tape (print)" : "input"); break;
        case 9: snprintf(v, n, "%s", tp.monitor ? "ON" : "OFF"); break;
        case 28: snprintf(v, n, "%s", TPFX_NAMES[tp.fx_route]); break;
        case 10: snprintf(v, n, "%s >", fxrack_slot_name(&tp_rk, 0)); break;
        case 11: snprintf(v, n, "%s >", fxrack_slot_name(&tp_rk, 1)); break;
        case 12: snprintf(v, n, "%s >", fxrack_slot_name(&tp_rk, 2)); break;
        case 13: {
            if (tp.clip_len) snprintf(v, n, "%.2fs held", (float)tp.clip_len / TP_RATE);
            else             snprintf(v, n, "%s", stopped_or("copy >"));
            break;
        }
        case 14: snprintf(v, n, "%s", stopped_or("cut >")); break;
        case 15: {
            if (!tp.clip_len) snprintf(v, n, "(empty)");
            else              snprintf(v, n, "%s", stopped_or("at IN >"));
            break;
        }
        case 16: snprintf(v, n, "%s", stopped_or("crop >")); break;
        case 17: snprintf(v, n, "%s", stopped_or("crop >")); break;
        case 18: snprintf(v, n, "%s", stopped_or("crop >")); break;
        case 19: {   // save-loop-then-crop — works while playing, so no stopped_or here
            if (tp.save_busy)       snprintf(v, n, "saving...");
            else if (tp.save_id[0]) snprintf(v, n, "%s", tp.save_id);
            else                    snprintf(v, n, "%s", tp.recording ? "recording" : "crop >");
            break;
        }
        case 20: snprintf(v, n, "%s", stopped_or("browse >")); break;
        case 21: snprintf(v, n, "%s", stopped_or("wipe >")); break;
        case 22: snprintf(v, n, "%us", (unsigned)(tp.cap / TP_RATE)); break;
        case 23: snprintf(v, n, "%s", tp.rec_mode == TPR_MOMENTARY ? "momentary" : "punch"); break;
        case 24: snprintf(v, n, "%s", tp.play_oneshot ? "one-shot" : "loop"); break;
        case 25: snprintf(v, n, "%s", tp.rec_dest == TPD_CARD ? "card (long)" : "tape (loop)"); break;
        case 26: snprintf(v, n, "%s", tp.rec_quant == TPQ_BAR ? "bar" : tp.rec_quant == TPQ_BEAT ? "beat" : "off"); break;
        case 27: {
            int on = 0;
            for (int d = 0; d < tp.mtx.n; d++) if (tp.mtx.src[d] >= 0) on++;
            if (on) snprintf(v, n, "%d on >", on); else snprintf(v, n, "edit >");
            break;
        }
    }
}

static void tape_adj(int i, int dir)
{
    float d = (float)dir;
    switch (i) {
        case 0:
            s_beats_idx = tp_clampi(s_beats_idx + dir, 0, BEAT_LADDER_N - 1);
            tape_crop_beats(BEAT_LADDER[s_beats_idx]);
            break;
        case 1: tp.clk_src = clock_source_cycle_cv_audio(tp.clk_src, dir);
                clockin_reset(&tp.ci, 1.0f); break;
        case 2: tp.manual_bpm = tp_clampf(tp.manual_bpm + d, 40, 240); break;
        case 3: { int m = tp.flt_mode + dir; if (m < 0) m = TPF_N - 1; if (m >= TPF_N) m = 0;
                  tp.flt_mode = m; } break;
        case 4: tp.cutoff = tp_clampf(tp.cutoff * (dir > 0 ? 1.12f : 0.893f), 30, 6000); break;
        case 5: tp.res01 = tp_clampf(tp.res01 + d * 0.05f, 0, 1); break;
        case 6: tp.drive = tp_clampf(tp.drive + d * 0.05f, 0, 1); break;
        case 7: tp.level = tp_clampf(tp.level + d * 0.05f, 0, 1.2f); break;
        case 8: tp.rec_src = tp.rec_src == TPS_INPUT ? TPS_TAPE : TPS_INPUT; break;
        case 9: tp.monitor = !tp.monitor; break;
        case 28: tp.fx_route = (tp.fx_route + (dir > 0 ? 1 : TPFX_N - 1)) % TPFX_N; break;
        case 22: tape_set_len_sel(tp.len_sel + dir < 0 ? TP_LEN_OPTS - 1
                                  : (tp.len_sel + dir) % TP_LEN_OPTS); break;
        case 23: tp.rec_mode = tp.rec_mode == TPR_PUNCH ? TPR_MOMENTARY : TPR_PUNCH; break;
        case 24: tp.play_oneshot = !tp.play_oneshot; break;
        case 25: if (!tp.recording && !tp.playing)   // switch dest only when stopped
                     tp.rec_dest = tp.rec_dest == TPD_TAPE ? TPD_CARD : TPD_TAPE;
                 break;
        case 26: tp.rec_quant = (tp.rec_quant + 1) % 3; break;   // off -> beat -> bar
    }
}

static int tape_action(int i)
{
    switch (i) {
        case 10: s_setup_return = 10; s_cur_slot = 0; return M_TAPE_FX;   // FX1
        case 11: s_setup_return = 11; s_cur_slot = 1; return M_TAPE_FX;   // FX2
        case 12: s_setup_return = 12; s_cur_slot = 2; return M_TAPE_FX;   // FX3 reverb
        case 13: tape_copy(); break;
        case 14: tape_cut(); break;
        case 15: tape_paste(); break;
        case 16: tape_norm(); break;
        case 17: tape_reverse(); break;
        case 18: tape_fade(); break;
        case 19: crop_drop(); break;   // same drop as the Live button (+ its note)
        case 20: return M_TAPE_LOAD;
        case 21: tape_clear(); break;
        case 27: s_setup_return = 27; return M_TAPE_CV;   // CV matrix page
    }
    return 0;
}

static setup_menu_t tape_setup = {
    .items = tape_setup_items,
    .n = 28,
    .title = "Tape Setup",
    .aff_label = "Machine", .aff_target = M_MORE,
    .live_target = M_TAPE_MAIN,
    .render = tape_val, .adjust = tape_adj, .action = tape_action,
};

static int tape_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU && s_setup_return >= 0) {   // return from a slot sub-page
        int p = s_setup_return; s_setup_return = -1;
        setup_menu_enter_at(&tape_setup, p);
        return 0;
    }
    return setup_menu_event(&tape_setup, event);
}

// ---- FX slot editor: one slot at a time, rows driven by the shared fxrack ----
// Reached from the FX1/FX2/FX3 Setup rows; the rate effects (delay/flanger/
// tremolo) carry a Sync toggle + Div, and Tape feeds the grid BPM so they lock.
static setup_menu_t tape_fx;             // defined below
static setup_item_t s_tfx_items[FXRACK_MAXROWS];
static int8_t s_tfx_param[FXRACK_MAXROWS];
static int s_tfx_n;

static void tfx_rebuild(void)
{
    s_tfx_n = fxrack_menu_rows(&tp_rk, s_cur_slot, s_tfx_items, s_tfx_param);
    tape_fx.n = s_tfx_n;
    tape_fx.title = s_cur_slot == 0 ? "Tape FX1" : s_cur_slot == 1 ? "Tape FX2" : "Tape FX3";
    if (tape_fx.sel >= s_tfx_n) tape_fx.sel = s_tfx_n - 1;
    if (tape_fx.sel < 0) tape_fx.sel = 0;
}

static void tape_fx_val(int i, char *v, size_t n)
{
    if (i < 0 || i >= s_tfx_n) { v[0] = 0; return; }
    fxrack_menu_val(&tp_rk, s_cur_slot, s_tfx_param[i], v, n);
}

static void tape_fx_adj(int i, int dir)
{
    if (i < 0 || i >= s_tfx_n) return;
    int p = s_tfx_param[i];
    fxrack_menu_adj(&tp_rk, s_cur_slot, p, dir);
    if (p < 0) tfx_rebuild();            // effect changed -> param rows changed
}

static setup_menu_t tape_fx = {
    .items = s_tfx_items,
    .n = 0,                              // set by tfx_rebuild()
    .title = "Tape FX",                  // overwritten per-slot by tfx_rebuild()
    .aff_label = "Setup", .aff_target = M_TAPE_SETUP,
    .live_target = M_TAPE_MAIN,
    .render = tape_fx_val, .adjust = tape_fx_adj, .action = NULL,
};

static int tape_fx_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) tfx_rebuild();   // dynamic row set for s_cur_slot
    return setup_menu_event(&tape_fx, event);
}

// ---- CV matrix page (the shared cvmtx widget drives everything) -----------------
static int tape_cv_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU)
        tape_mtx_refresh_labels();   // FX rows rename with the loaded effects
    return cvmtx_menu_event(&tp.mtx, event, "Tape CV Matrix",
                            M_TAPE_SETUP, M_TAPE_MAIN);
}

// ---- Load Sample (shared browser) ----------------------------------------------
static int tape_load_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) {
        // open in Tape's home folder (Arlo 2026-07-25), same as Drums/Slicer do.
        // The old "don't force usr/TAPE, it starts empty" reasoning is stale —
        // takes and crops land there now. Every other folder is still one scroll
        // away, and an empty folder just clamps the cursor onto the folder rows.
        sample_browser_enter_dir(true, "Load to Tape", "", SAMPLE_DIR_TAPE);
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1) { tape_load(sample_browser_selected()); return M_TAPE_MAIN; }
    if (r == 2) return M_TAPE_SETUP;
    return 0;
}

// ---- wiring ---------------------------------------------------------------------
static void tape_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_TAPE_MAIN);  menusys_item_set_default_cb(_ms, M_TAPE_MAIN, tape_main_handler);
    menusys_new_item(_ms, M_TAPE_SETUP); menusys_item_set_default_cb(_ms, M_TAPE_SETUP, tape_setup_handler);
    menusys_new_item(_ms, M_TAPE_LOAD);  menusys_item_set_default_cb(_ms, M_TAPE_LOAD, tape_load_handler);
    menusys_new_item(_ms, M_TAPE_FX);    menusys_item_set_default_cb(_ms, M_TAPE_FX, tape_fx_handler);
    menusys_new_item(_ms, M_TAPE_CV);    menusys_item_set_default_cb(_ms, M_TAPE_CV, tape_cv_handler);
}

static int tape_main_event(int event, void *ev_data)
{
    (void)ev_data;
    tape_autosave_kick();   // machine-ambient: spawn a requested auto-save from any Tape page
    tape_card_service();    // machine-ambient: keep the card recorder armed from any Tape page
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        char s[64];
        snprintf(s, sizeof(s), "Tape: %s %.1fs/%us", tp.recording ? "REC" : tp.playing ? "PLAY" : "stop",
                 (float)tp.pos / TP_RATE, (unsigned)(tp.cap / TP_RATE));
        _fg = TFT_LIGHTGREY;
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const tape_main_items[] = { "Tape", "Setup" };
static const int tape_main_targets[] = { M_TAPE_MAIN, M_TAPE_SETUP };

const machine_ui_t tape_menu_ui = {
    .main_items = tape_main_items,
    .main_targets = tape_main_targets,
    .n_main = 2,
    .register_pages = tape_register_pages,
    .main_event = tape_main_event,
    .boot_target = M_TAPE_MAIN,
};
