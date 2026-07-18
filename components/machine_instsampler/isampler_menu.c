// Keys UI — a Live dashboard (waveform + loop box + playhead, four macro dials,
// ADSR curve) and a Setup page (shared setup_menu framework). Mirrors the Synth
// dashboard: black canvas, boxed elements, redraw-on-change only. The waveform
// strip shows the sustain-loop window as a box (the box IS the loop) with a
// white playhead — the deck/slicer/sampler3 idiom.
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
#include "instsampler_priv.h"

static const color_t GATE_ON = {40, 200, 90};    // note stays green (gate flips too fast to read)
static const color_t WF_GREY = {125, 125, 135};  // waveform
static const color_t LOOP_COL = {70, 200, 235};  // loop window (cyan)
static const color_t PH_COL  = {240, 240, 245};  // playhead (white)

static const char *const NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void note_name_midi(int midi, char *buf, size_t n)
{
    if (midi < 0) { snprintf(buf, n, "--"); return; }
    if (midi > 127) midi = 127;
    snprintf(buf, n, "%s%d", NOTE_NAMES[midi % 12], midi / 12 - 1);
}

// ---- Live geometry ---------------------------------------------------------
static int L_wy(void) { return TFT_getfontheight() + 16; }
#define L_WX 8
#define L_WW (_width - 16)
#define L_WH 46

static int loop_x(uint32_t frame)
{
    uint32_t fr = inst.zone[0].frames;
    if (fr == 0) return L_WX;
    if (frame > fr) frame = fr;
    return L_WX + (int)((uint64_t)frame * L_WW / fr);
}

static int s_last_note = -9999;
static unsigned s_sig_dials = 0, s_sig_adsr = 0, s_sig_wave = 0;
static int s_last_ph = -1;

// ---- header: title + sample tag + note name (green) ------------------------
static void draw_header(void)
{
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, 0, _width, fh + 12, _bg);
    _fg = TFT_WHITE; TFT_print("Keys", 6, 4);
    _fg = (color_t){130, 130, 140};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    char tag[26]; snprintf(tag, sizeof(tag), "%s", inst.zone[0].sample[0] ? inst.zone[0].sample : "(no sample)");
    TFT_print(tag, 54, 6);
    TFT_setFont(DEFAULT_FONT, NULL);
    char nm[12]; note_name_midi((int)lroundf(inst.note_disp), nm, sizeof(nm));
    Font f = cfont; TFT_setFont(DEJAVU18_FONT, NULL);
    _fg = GATE_ON; TFT_print(nm, _width - 10 - TFT_getStringWidth(nm), 2);
    cfont = f;
}

// one waveform column at pixel x: black bg + grey peak + a loop edge line if it
// falls here (used to erase the playhead cleanly on the incremental path)
static void wave_col(int x)
{
    int wy = L_wy();
    int col = x - L_WX;
    if (col < 0 || col >= L_WW) return;
    _bg = TFT_BLACK; TFT_fillRect(x, wy, 1, L_WH, _bg);
    int pi = clampi(col * IS_PEAKS / L_WW, 0, IS_PEAKS - 1);
    int h = inst.peaks[pi] * (L_WH / 2) / 255;
    if (h < 1 && inst.zone[0].frames) h = 1;
    int cy = wy + L_WH / 2;
    if (h > 0) TFT_drawLine(x, cy - h, x, cy + h, WF_GREY);
    if (inst.zone[0].loop_mode == LOOP_FWD && inst.zone[0].frames) {
        if (x == loop_x(inst.zone[0].loop_start) || x == loop_x(inst.zone[0].loop_end))
            TFT_drawLine(x, wy, x, wy + L_WH, LOOP_COL);
    }
}

// full waveform strip: peaks + loop-window edges + center line
static void draw_wave(void)
{
    int wy = L_wy(), cy = wy + L_WH / 2;
    _bg = TFT_BLACK; TFT_fillRect(L_WX, wy, L_WW, L_WH, _bg);
    if (inst.zone[0].frames == 0) {
        _fg = (color_t){90, 90, 100};
        TFT_print("no sample - Setup > Load Sample", L_WX + 4, cy - TFT_getfontheight() / 2);
        s_last_ph = -1;
        return;
    }
    for (int c = 0; c < L_WW; c++) {
        int pi = c * IS_PEAKS / L_WW;
        int h = inst.peaks[pi] * (L_WH / 2) / 255;
        if (h < 1) h = 1;
        TFT_drawLine(L_WX + c, cy - h, L_WX + c, cy + h, WF_GREY);
    }
    if (inst.zone[0].loop_mode == LOOP_FWD) {
        int lsx = loop_x(inst.zone[0].loop_start), lex = loop_x(inst.zone[0].loop_end);
        TFT_drawLine(lsx, wy, lsx, wy + L_WH, LOOP_COL);
        TFT_drawLine(lex, wy, lex, wy + L_WH, LOOP_COL);
    }
    s_last_ph = -1;
}

static void draw_playhead(void)
{
    int wy = L_wy();
    is_voice_t *v = &inst.voice[0];
    int ph = -1;
    if (v->active && inst.zone[0].frames)
        ph = loop_x((uint32_t)v->pos);
    if (ph == s_last_ph) return;
    if (s_last_ph >= 0) wave_col(s_last_ph);
    if (ph >= 0) TFT_drawLine(ph, wy, ph, wy + L_WH, PH_COL);
    s_last_ph = ph;
}

// ---- a knob dial: ring + pointer, label + value (lifted from Synth) --------
static void dial(int cx, int cy, int r, float v01, const char *lab, const char *val, bool live)
{
    int fh = TFT_getfontheight();
    v01 = clampf(v01, 0.0f, 1.0f);
    _bg = TFT_BLACK;
    TFT_fillRect(cx - r - 5, cy - r - 2, (r + 5) * 2, r * 2 + 2 * fh + 8, _bg);
    color_t ring = live ? (color_t){0, 150, 220} : (color_t){70, 70, 80};
    TFT_drawCircle(cx, cy, r, ring);
    float ang = (-135.0f + v01 * 270.0f) * 0.01745329f;
    int px = cx + (int)(sinf(ang) * (float)(r - 4));
    int py = cy - (int)(cosf(ang) * (float)(r - 4));
    TFT_drawLine(cx, cy, px, py, live ? (color_t){255, 255, 255} : (color_t){150, 150, 160});
    TFT_fillCircle(cx, cy, 3, ring);
    _fg = (color_t){130, 130, 140};
    TFT_print((char *)lab, cx - TFT_getStringWidth((char *)lab) / 2, cy + r + 3);
    _fg = TFT_WHITE;
    TFT_print((char *)val, cx - TFT_getStringWidth((char *)val) / 2, cy + r + 3 + fh + 1);
}

static int dial_cy(void) { return L_wy() + L_WH + 8 + 20; }

static void draw_dials(void)
{
    int cy = dial_cy(), r = 20;
    int cx[4] = { 44, 128, 212, 292 };
    char v[16];
    snprintf(v, sizeof(v), "%.0f%%", inst.start_frac * 100.0f);
    dial(cx[0], cy, r, inst.start_frac, "start", v, inst.knob_live[0]);
    float cv = logf(inst.cutoff_base / 10.0f) / logf(600.0f);
    if (inst.cutoff_base >= 1000.0f) snprintf(v, sizeof(v), "%.1fk", inst.cutoff_base / 1000.0f);
    else                             snprintf(v, sizeof(v), "%.0f", inst.cutoff_base);
    dial(cx[1], cy, r, cv, "cut", v, inst.knob_live[1]);
    snprintf(v, sizeof(v), "%.0f%%", inst.res01 * 100.0f);
    dial(cx[2], cy, r, inst.res01, "res", v, inst.knob_live[2]);
    snprintf(v, sizeof(v), "%.0f%%", inst.env_to_cut * 100.0f);
    dial(cx[3], cy, r, inst.env_to_cut, "env>f", v, inst.knob_live[3]);
}

// ---- ADSR curve (lifted from Synth) ----------------------------------------
static void draw_adsr(void)
{
    int fh = TFT_getfontheight();
    // +21 not +14: at +14 the ENV label/box top overdraws the dials' value row
    // (seen on the first shadow-FB screenshot — "53%" clipped, label cut to "El")
    int x = 8, y = dial_cy() + 20 + 2 * fh + 21, w = _width - 16, h = 38;
    _bg = TFT_BLACK; TFT_fillRect(x, y - fh - 2, w, h + fh + 4, _bg);
    _fg = (color_t){110, 110, 120}; TFT_print("ENV", x, y - fh - 2);
    TFT_drawRect(x, y, w, h, (color_t){40, 60, 90});
    float ta = inst.atk, td = inst.dec, tr = inst.rel, tsum = ta + td + tr;
    if (tsum < 1e-4f) tsum = 1e-4f;
    float body = (float)(w - 4) * 0.72f;
    int aw = (int)(body * ta / tsum), dw = (int)(body * td / tsum), rw = (int)(body * tr / tsum);
    int sw = (w - 4) - aw - dw - rw;
    int xb = x + 2, yb = y + h - 2, yt = y + 2;
    int ys = yb - (int)(inst.sus * (float)(h - 4));
    color_t col = {60, 200, 120};
    int x1 = xb + aw, x2 = x1 + dw, x3 = x2 + sw, x4 = x3 + rw;
    TFT_drawLine(xb, yb, x1, yt, col);
    TFT_drawLine(x1, yt, x2, ys, col);
    TFT_drawLine(x2, ys, x3, ys, col);
    TFT_drawLine(x3, ys, x4, yb, col);
}

static unsigned dials_sig(void)
{
    return (unsigned)(inst.cutoff_base) * 131u
         + (unsigned)(inst.start_frac * 1000.0f) * 17u
         + (unsigned)(inst.res01 * 1000.0f) * 29u
         + (unsigned)(inst.env_to_cut * 1000.0f) * 41u
         + (inst.knob_live[0]?1u:0u) + (inst.knob_live[1]?2u:0u)
         + (inst.knob_live[2]?4u:0u) + (inst.knob_live[3]?8u:0u);
}
static unsigned adsr_sig(void)
{
    return (unsigned)(inst.atk*1000)*7u + (unsigned)(inst.dec*1000)*13u
         + (unsigned)(inst.sus*1000)*17u + (unsigned)(inst.rel*1000)*19u;
}
static unsigned wave_sig(void)
{
    unsigned h = inst.zone[0].frames * 2654435761u;
    h ^= inst.zone[0].loop_start * 40503u;
    h ^= inst.zone[0].loop_end * 2246822519u;
    h ^= (unsigned)inst.zone[0].loop_mode * 7u;
    for (const char *p = inst.zone[0].sample; *p; p++) h = h * 31u + (unsigned)*p;
    return h;
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    draw_header();  s_last_note = (int)lroundf(inst.note_disp);
    draw_wave();    s_sig_wave = wave_sig();
    draw_playhead();
    draw_dials();   s_sig_dials = dials_sig();
    draw_adsr();    s_sig_adsr = adsr_sig();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("CV1:pitch  TR1:gate   K5:start K6:cut K7:res K8:env>f", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int keys_live_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST: {
            int midi = (int)lroundf(inst.note_disp);
            if (midi != s_last_note) { draw_header(); s_last_note = midi; }
            unsigned ws = wave_sig();
            if (ws != s_sig_wave) { draw_wave(); s_sig_wave = ws; }
            draw_playhead();
            unsigned ds = dials_sig();
            if (ds != s_sig_dials) { draw_dials(); s_sig_dials = ds; }
            unsigned as = adsr_sig();
            if (as != s_sig_adsr) { draw_adsr(); s_sig_adsr = as; }
            break;
        }
        case EV_LONG_PRESS: return M_ISMP_SETUP;
        default: break;
    }
    return 0;
}

// ---- Setup (shared framework) ----------------------------------------------
// Instrument params live here; all five effects moved to a dedicated FX
// sub-page (the "FX" action row) so this list stays readable.
static const setup_item_t ks_setup_items[] = {
    {"Load Sample", ST_ACTION}, {"Root Note", ST_RANGE},  {"Base Note", ST_RANGE},
    {"Quantize",    ST_TOGGLE}, {"Loop Mode", ST_TOGGLE}, {"Loop Start", ST_RANGE},
    {"Loop End",    ST_RANGE},  {"Loop Xfade", ST_RANGE},
    {"Attack",      ST_RANGE},  {"Decay",     ST_RANGE},  {"Sustain",   ST_RANGE},
    {"Release",     ST_RANGE},  {"Env>Cut",   ST_RANGE},  {"Glide",     ST_RANGE},
    {"Level",       ST_RANGE},
    {"CV Matrix",   ST_ACTION},
    {"FX1",         ST_ACTION}, {"FX2",       ST_ACTION}, {"FX3 Reverb", ST_ACTION},
    {"Save Patch",  ST_ACTION}, {"Load Patch", ST_ACTION},
};

// last saved patch id, shown inline on the Save Patch row as confirmation
static char s_last_saved[12];

// effect name for a slot row on the Setup page (gen_desc is defined lower down)
static const char *fxk_name(int k)
{
    switch (k) {
        case FXK_OD:   return "Overdrive";
        case FXK_FLG:  return "Flanger";
        case FXK_TREM: return "Tremolo";
        case FXK_DLY:  return "Delay";
        case FXK_FILT: return "Filter";
        default:       return "Off";
    }
}
static int s_cur_slot = 0;      // which FX slot the shared FX sub-page is editing
static int s_setup_return = -1; // Setup row to restore on return from a sub-page

static void ks_val(int i, char *v, size_t n)
{
    is_zone_t *z = &inst.zone[0];
    char nm[12];
    switch (i) {
        case 0: snprintf(v, n, "%s", z->sample[0] ? z->sample : "(none)"); break;
        case 1: note_name_midi(z->root, nm, sizeof(nm)); snprintf(v, n, "%s (%d)", nm, z->root); break;
        case 2: note_name_midi(inst.base_note, nm, sizeof(nm)); snprintf(v, n, "%s (%d)", nm, inst.base_note); break;
        case 3: snprintf(v, n, "%s", inst.quantize ? "ON" : "OFF"); break;
        case 4: snprintf(v, n, "%s", z->loop_mode == LOOP_FWD ? "Fwd" : "Off"); break;
        case 5: snprintf(v, n, "%u ms", (unsigned)((uint64_t)z->loop_start * 1000 / IS_RATE)); break;
        case 6: snprintf(v, n, "%u ms", (unsigned)((uint64_t)z->loop_end * 1000 / IS_RATE)); break;
        case 7: snprintf(v, n, "%u ms", (unsigned)((uint64_t)z->loop_xfade * 1000 / IS_RATE)); break;
        case 8: snprintf(v, n, "%d ms", (int)(inst.atk * 1000.0f)); break;
        case 9: snprintf(v, n, "%d ms", (int)(inst.dec * 1000.0f)); break;
        case 10: snprintf(v, n, "%.0f%%", inst.sus * 100.0f); break;
        case 11: snprintf(v, n, "%d ms", (int)(inst.rel * 1000.0f)); break;
        case 12: snprintf(v, n, "%.0f%%", inst.env_to_cut * 100.0f); break;
        case 13: snprintf(v, n, "%d ms", (int)(inst.glide * 1000.0f)); break;
        case 14: snprintf(v, n, "%.0f%%", inst.level * 100.0f); break;
        case 15: { int on = 0; for (int d = 0; d < ISM_N; d++) if (inst.mtx_src[d] >= 0) on++;
                   if (on) snprintf(v, n, "%d on >", on); else snprintf(v, n, "edit >"); break; }
        case 16: snprintf(v, n, "%s >", fxk_name(inst.fx_slot[0])); break;
        case 17: snprintf(v, n, "%s >", fxk_name(inst.fx_slot[1])); break;
        case 18: snprintf(v, n, "%s >", inst.rv.mode != RV_OFF ? reverb_mode_name(inst.rv.mode) : "Off"); break;
        case 19: snprintf(v, n, "%s", s_last_saved[0] ? s_last_saved : "save >"); break;
        case 20: snprintf(v, n, "load >"); break;
    }
}

static void ks_adj(int i, int dir)
{
    is_zone_t *z = &inst.zone[0];
    float d = (float)dir;
    long step = IS_RATE / 100;                        // ~10 ms for loop points
    switch (i) {
        case 1: z->root = (uint8_t)clampi((int)z->root + dir, 12, 108); break;
        case 2: inst.base_note = clampi(inst.base_note + dir, 12, 108); break;
        case 3: inst.quantize = !inst.quantize; break;
        case 4: z->loop_mode = (z->loop_mode == LOOP_FWD) ? LOOP_OFF : LOOP_FWD; break;
        case 5: if (z->frames) {                       // Loop Start (zero-cross snapped)
                    long ls = clampi((int)((long)z->loop_start + dir * step), 0, (int)z->loop_end - 64);
                    if (ls < 0) ls = 0;
                    ls = (long)keys_snap_zero((uint32_t)ls);
                    if (ls >= (long)z->loop_end) ls = (long)z->loop_end - 64;
                    if (ls < 0) ls = 0;
                    z->loop_start = (uint32_t)ls;
                } break;
        case 6: if (z->frames) {                       // Loop End (zero-cross snapped)
                    long le = clampi((int)((long)z->loop_end + dir * step), (int)z->loop_start + 64, (int)z->frames);
                    le = (long)keys_snap_zero((uint32_t)le);
                    if (le > (long)z->frames) le = (long)z->frames;
                    if (le <= (long)z->loop_start) le = (long)z->loop_start + 64;
                    z->loop_end = (uint32_t)le;
                } break;
        case 7: z->loop_xfade = (uint32_t)clampi((int)z->loop_xfade + dir * (IS_RATE / 1000), 0, 4096); break;
        case 8: inst.atk = clampf(inst.atk + d * 0.005f, 0.0005f, 2.0f); break;
        case 9: inst.dec = clampf(inst.dec + d * 0.01f, 0.001f, 2.0f); break;
        case 10: inst.sus = clampf(inst.sus + d * 0.05f, 0.0f, 1.0f); break;
        case 11: inst.rel = clampf(inst.rel + d * 0.02f, 0.001f, 3.0f); break;
        case 12: inst.env_to_cut = clampf(inst.env_to_cut + d * 0.05f, 0.0f, 1.0f); break;
        case 13: inst.glide = clampf(inst.glide + d * 0.02f, 0.0f, 2.0f); break;
        case 14: inst.level = clampf(inst.level + d * 0.05f, 0.0f, 1.0f); break;
    }
}

static int ks_action(int i)
{
    if (i == 0)  return M_ISMP_LOAD;     // Load Sample -> browser
    if (i == 15) { s_setup_return = 15; return M_ISMP_MATRIX; }   // CV Matrix
    if (i == 16) { s_setup_return = 16; s_cur_slot = 0; return M_ISMP_FX; }   // FX1
    if (i == 17) { s_setup_return = 17; s_cur_slot = 1; return M_ISMP_FX; }   // FX2
    if (i == 18) { s_setup_return = 18; s_cur_slot = 2; return M_ISMP_FX; }   // FX3 reverb
    if (i == 19) {                       // Save Patch: mint + write, stay on Setup
        if (keys_patch_save(s_last_saved, sizeof(s_last_saved)) != 0)
            snprintf(s_last_saved, sizeof(s_last_saved), "err");
        return 0;                        // framework redraws -> row shows the id
    }
    if (i == 20) return M_ISMP_PATCH;    // Load Patch -> patch browser
    return 0;
}

static setup_menu_t ks_setup = {
    .items = ks_setup_items,
    .n = 21,
    .title = "Keys Setup",
    .aff_label = "Machine", .aff_target = M_MORE,
    .live_target = M_ISMP_LIVE,
    .render = ks_val, .adjust = ks_adj, .action = ks_action,
};

// ---- FX sub-page: all five effects, reached from the Setup "FX" row --------
// ===== FX rack: curated slots (FX1/FX2 = a generic effect each, FX3 = reverb).
// The FX page is DYNAMIC: each slot shows a select row, and below it only the
// selected effect's params. fmt/adj operate on the global `inst`. =============

typedef struct { const char *label; setup_kind_t kind;
                 void (*fmt)(char *, size_t); void (*adj)(int); } kfx_p_t;
typedef struct { const char *name; const kfx_p_t *p; int np; } kfx_desc_t;

// --- overdrive ---
static void od_fd(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.od.drive*100);}
static void od_ad(int d){inst.od.drive=clampf(inst.od.drive+d*0.05f,0,1);}
static void od_ft(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.od.tone*100);}
static void od_at(int d){inst.od.tone=clampf(inst.od.tone+d*0.05f,0,1);}
static void od_fb(char*v,size_t n){snprintf(v,n,"%+.0f%%",inst.od.bias*100);}
static void od_ab(int d){inst.od.bias=clampf(inst.od.bias+d*0.05f,-1,1);}
static void od_fl(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.od.level*100);}
static void od_al(int d){inst.od.level=clampf(inst.od.level+d*0.05f,0,1);}
static const kfx_p_t od_params[] = {
    {"Drive",ST_RANGE,od_fd,od_ad},{"Tone",ST_RANGE,od_ft,od_at},
    {"Bias",ST_RANGE,od_fb,od_ab},{"Level",ST_RANGE,od_fl,od_al}};

// --- flanger ---
static void fl_fr(char*v,size_t n){snprintf(v,n,"%.2f Hz",inst.flg.rate);}
static void fl_ar(int d){inst.flg.rate=clampf(inst.flg.rate+d*0.05f,0.01f,10);}
static void fl_fd(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.flg.depth*100);}
static void fl_ad(int d){inst.flg.depth=clampf(inst.flg.depth+d*0.05f,0,1);}
static void fl_ff(char*v,size_t n){snprintf(v,n,"%+.0f%%",inst.flg.fb*100);}
static void fl_af(int d){inst.flg.fb=clampf(inst.flg.fb+d*0.05f,-0.95f,0.95f);}
static void fl_fm(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.flg.wet*100);}
static void fl_am(int d){inst.flg.wet=clampf(inst.flg.wet+d*0.05f,0,1);}
static const kfx_p_t flg_params[] = {
    {"Rate",ST_RANGE,fl_fr,fl_ar},{"Depth",ST_RANGE,fl_fd,fl_ad},
    {"Fdbk",ST_RANGE,fl_ff,fl_af},{"Mix",ST_RANGE,fl_fm,fl_am}};

// --- tremolo ---
static void tr_fr(char*v,size_t n){snprintf(v,n,"%.2f Hz",inst.trem.rate);}
static void tr_ar(int d){inst.trem.rate=clampf(inst.trem.rate+d*0.25f,0.05f,20);}
static void tr_fd(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.trem.depth*100);}
static void tr_ad(int d){inst.trem.depth=clampf(inst.trem.depth+d*0.05f,0,1);}
static void tr_fs(char*v,size_t n){snprintf(v,n,"%s",inst.trem.shape==TREM_TRI?"Tri":inst.trem.shape==TREM_SQR?"Sqr":"Sine");}
static void tr_as(int d){(void)d;inst.trem.shape=(inst.trem.shape+1)%3;}
static void tr_fst(char*v,size_t n){snprintf(v,n,"%s",inst.trem.stereo?"ON":"OFF");}
static void tr_ast(int d){(void)d;inst.trem.stereo=!inst.trem.stereo;}
static const kfx_p_t trem_params[] = {
    {"Rate",ST_RANGE,tr_fr,tr_ar},{"Depth",ST_RANGE,tr_fd,tr_ad},
    {"Shape",ST_TOGGLE,tr_fs,tr_as},{"Stereo",ST_TOGGLE,tr_fst,tr_ast}};

// --- delay ---
static void dl_ft(char*v,size_t n){snprintf(v,n,"%d ms",(int)(fxdelay_time_ms(&inst.dly)+0.5f));}
static void dl_at(int d){if(inst.dly.bufL)fxdelay_set_time_ms(&inst.dly,fxdelay_time_ms(&inst.dly)+d*10.0f);}
static void dl_ff(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.dly.fb*100);}
static void dl_af(int d){if(inst.dly.bufL)fxdelay_set_feedback(&inst.dly,inst.dly.fb+d*0.05f);}
static void dl_fm(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.dly.wet*100);}
static void dl_am(int d){if(inst.dly.bufL)fxdelay_set_mix(&inst.dly,inst.dly.wet+d*0.05f);}
static void dl_fto(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.dly.damp*100);}
static void dl_ato(int d){if(inst.dly.bufL)fxdelay_set_damp(&inst.dly,inst.dly.damp+d*0.05f);}
static void dl_fp(char*v,size_t n){snprintf(v,n,"%s",inst.dly.pingpong?"Ping-Pong":"Stereo");}
static void dl_ap(int d){(void)d;if(inst.dly.bufL)fxdelay_set_pingpong(&inst.dly,!inst.dly.pingpong);}
static const kfx_p_t dly_params[] = {
    {"Time",ST_RANGE,dl_ft,dl_at},{"Fdbk",ST_RANGE,dl_ff,dl_af},
    {"Mix",ST_RANGE,dl_fm,dl_am},{"Tone",ST_RANGE,dl_fto,dl_ato},
    {"Ping",ST_TOGGLE,dl_fp,dl_ap}};

// --- filter (insert brick) ---
static void ft_fm(char*v,size_t n){snprintf(v,n,"%s",inst.filt.mode==FILT_LP?"LP":inst.filt.mode==FILT_HP?"HP":"BP");}
static void ft_am(int d){(void)d;inst.filt.mode=(inst.filt.mode+1)%FILT_NMODE;}
static void ft_fc(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.filt.cutoff*100);}
static void ft_ac(int d){inst.filt.cutoff=clampf(inst.filt.cutoff+d*0.05f,0,1);}
static void ft_fq(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.filt.reso*100);}
static void ft_aq(int d){inst.filt.reso=clampf(inst.filt.reso+d*0.05f,0,1);}
static const kfx_p_t filt_params[] = {
    {"Mode",ST_TOGGLE,ft_fm,ft_am},{"Cutoff",ST_RANGE,ft_fc,ft_ac},
    {"Reso",ST_RANGE,ft_fq,ft_aq}};

// --- reverb (FX3, fixed slot) ---
static void rv_fx(char*v,size_t n){snprintf(v,n,"%.0f%%",inst.rv.wet*100);}
static void rv_ax(int d){reverb_set_mix(&inst.rv,clampf(inst.rv.wet+d*0.05f,0,1));}
static const kfx_p_t rev_params[] = {{"Rev Mix",ST_RANGE,rv_fx,rv_ax}};
static const kfx_desc_t rev_desc = {"Reverb", rev_params, 1};

// generic-effect table indexed by FXK_* (FXK_OFF has no params)
static const kfx_desc_t gen_desc[FXK_NGEN] = {
    [FXK_OFF]  = {"OFF", NULL, 0},
    [FXK_OD]   = {"Overdrive", od_params,   4},
    [FXK_FLG]  = {"Flanger",   flg_params,  4},
    [FXK_TREM] = {"Tremolo",   trem_params, 4},
    [FXK_DLY]  = {"Delay",     dly_params,  5},
    [FXK_FILT] = {"Filter",    filt_params, 3},
};

// dynamic row list built from the slots (select row + selected effect's params)
#define KFX_MAXROWS 24
static setup_item_t s_kfx_items[KFX_MAXROWS];
static struct { int8_t slot; int8_t param; } s_kfx_row[KFX_MAXROWS];  // param<0 = select row
static int s_kfx_n;
static setup_menu_t ks_fx;   // defined below; rebuilt each entry/selection

// keep the *_on enable cache in sync with the slot assignment
static void ks_fx_sync(void){
    inst.od_on=inst.flg_on=inst.trem_on=inst.dly_on=inst.filt_on=false;
    for(int s=0;s<FX_NSLOT_GEN;s++) switch(inst.fx_slot[s]){
        case FXK_OD:inst.od_on=true;break; case FXK_FLG:inst.flg_on=true;break;
        case FXK_TREM:inst.trem_on=true;break; case FXK_DLY:inst.dly_on=true;break;
        case FXK_FILT:inst.filt_on=true;break; default:break; }
}

// assign effect `kind` to `slot`: lazy-init buffers + audible defaults (like the
// old toggles), enforce one-effect-per-slot, then sync enables.
static void kfx_slot_set(int slot, int kind){
    if(kind==FXK_DLY){ if(!inst.dly.bufL) fxdelay_init(&inst.dly);
        if(!inst.dly.bufL) kind=FXK_OFF; else if(inst.dly.wet<0.01f) fxdelay_set_mix(&inst.dly,0.30f); }
    else if(kind==FXK_FLG){ if(!inst.flg.bufL) flanger_init(&inst.flg);
        if(!inst.flg.bufL) kind=FXK_OFF; else if(inst.flg.wet<0.01f) inst.flg.wet=0.5f; }
    else if(kind==FXK_OD){ if(inst.od.level<0.01f){inst.od.drive=0.4f;inst.od.tone=0.5f;inst.od.level=0.8f;} overdrive_reset(&inst.od); }
    else if(kind==FXK_TREM){ if(inst.trem.depth<0.01f){inst.trem.depth=0.5f;inst.trem.rate=5.0f;} }
    for(int s=0;s<FX_NSLOT_GEN;s++) if(s!=slot && inst.fx_slot[s]==kind && kind!=FXK_OFF) inst.fx_slot[s]=FXK_OFF;
    inst.fx_slot[slot]=kind;
    ks_fx_sync();
}

// build the rows for the ONE slot being edited (s_cur_slot): an effect-select
// row, then the selected effect's params. FX3 (slot 2) is the reverb slot.
static void kfx_rebuild(void){
    int r=0;
    s_kfx_items[r].label="Effect"; s_kfx_items[r].kind=ST_TOGGLE;
    s_kfx_row[r].param=-1; r++;
    if(s_cur_slot==2){
        if(inst.rv.mode!=RV_OFF) for(int p=0;p<rev_desc.np && r<KFX_MAXROWS;p++){
            s_kfx_items[r].label=rev_desc.p[p].label; s_kfx_items[r].kind=rev_desc.p[p].kind;
            s_kfx_row[r].param=p; r++; }
    } else {
        int k=inst.fx_slot[s_cur_slot];
        if(k!=FXK_OFF) for(int p=0;p<gen_desc[k].np && r<KFX_MAXROWS;p++){
            s_kfx_items[r].label=gen_desc[k].p[p].label; s_kfx_items[r].kind=gen_desc[k].p[p].kind;
            s_kfx_row[r].param=p; r++; }
    }
    s_kfx_n=r; ks_fx.n=r;
    ks_fx.title = s_cur_slot==0?"Keys FX1":s_cur_slot==1?"Keys FX2":"Keys FX3";
    if(ks_fx.sel>=r) ks_fx.sel=r-1;
    if(ks_fx.sel<0)  ks_fx.sel=0;
}

static void ks_fx_val(int i, char *v, size_t n)
{
    v[0] = 0;
    if (i < 0 || i >= s_kfx_n) return;
    int p = s_kfx_row[i].param;
    if (p < 0) {                                   // effect-select row
        if (s_cur_slot == 2) snprintf(v, n, "%s", reverb_mode_name(inst.rv.mode));
        else snprintf(v, n, "%s", gen_desc[inst.fx_slot[s_cur_slot]].name);
        return;
    }
    if (s_cur_slot == 2) rev_desc.p[p].fmt(v, n);
    else gen_desc[inst.fx_slot[s_cur_slot]].p[p].fmt(v, n);
}

static void ks_fx_adj(int i, int dir)
{
    if (i < 0 || i >= s_kfx_n) return;
    int p = s_kfx_row[i].param;
    if (p < 0) {                                   // effect-select row: change effect
        if (s_cur_slot == 2) {                     // FX3: cycle reverb mode
            int m = inst.rv.mode + dir;
            if (m < 0) m = RV_N_MODES - 1;
            if (m >= RV_N_MODES) m = RV_OFF;
            if (m != RV_OFF && !inst.rv.slab && reverb_init(&inst.rv) != ESP_OK) m = RV_OFF;
            reverb_set_mode(&inst.rv, m);
        } else {                                   // FX1/FX2: cycle generic effect
            int k = inst.fx_slot[s_cur_slot] + dir;
            if (k < 0) k = FXK_NGEN - 1;
            if (k >= FXK_NGEN) k = FXK_OFF;
            kfx_slot_set(s_cur_slot, k);
        }
        kfx_rebuild();                             // param rows changed
        return;
    }
    if (s_cur_slot == 2) rev_desc.p[p].adj(dir);
    else gen_desc[inst.fx_slot[s_cur_slot]].p[p].adj(dir);
}

static setup_menu_t ks_fx = {
    .items = s_kfx_items,
    .n = 0,                       // set by kfx_rebuild()
    .title = "Keys FX",           // overwritten per-slot by kfx_rebuild()
    .aff_label = "Setup", .aff_target = M_ISMP_SETUP,
    .live_target = M_ISMP_SETUP,  // long-press = up one level to Setup
    .render = ks_fx_val, .adjust = ks_fx_adj, .action = NULL,
};

static int keys_fx_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) kfx_rebuild();   // dynamic row set
    return setup_menu_event(&ks_fx, event);
}

static int keys_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU && s_setup_return >= 0) {   // came back from a sub-page
        int p = s_setup_return; s_setup_return = -1;
        setup_menu_enter_at(&ks_setup, p);                   // land on the line we left
        return 0;
    }
    return setup_menu_event(&ks_setup, event);
}

// ---- Load Sample: the shared sample browser -> zone[0] ---------------------
static int keys_load_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) {
        // plain browse (opens in POOL, remembers position, all folders reachable).
        // NOT forced into usr/KEYS: that folder starts empty, and landing there
        // leaves nothing to load. KEYS is still a folder row + web destination.
        sample_browser_enter(true, "Load Sample", inst.zone[0].sample);
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1) {
        keys_load_zone(sample_browser_selected());
        return M_ISMP_LIVE;             // loaded: jump straight to Live to play it
    }
    if (r == 2) return M_ISMP_SETUP;    // cancelled: back to Setup where we came from
    return 0;
}

// ---- CV Matrix page (Synth's, relabelled for the sampler) ------------------
static const char *const mtx_labels[ISM_N] = {
    "Cutoff", "Reso", "Env>Cut", "Level", "Pitch", "Start", "LoopMov", "LoopLen"
};

static void mtx_redraw(int pos, int field)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print("Keys CV Matrix", 6, 4);
    menuTFTPrintAffordance("Setup", pos == -1);
    int row_h = fh + 5, y0 = fh + 14;
    int vis = (_height - fh - 6 - y0) / row_h;
    if (vis < 1) vis = 1;
    int top = 0;
    if (pos >= vis) top = pos - vis + 1;
    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= ISM_N) break;
        int y = y0 + r * row_h;
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        TFT_fillRect(0, y - 2, _width, fh + 4, _bg);
        _fg = TFT_WHITE;
        TFT_print((char *)mtx_labels[i], 8, y);
        char src[8], amt[10];
        if (inst.mtx_src[i] < 0) snprintf(src, sizeof(src), "off");
        else                     snprintf(src, sizeof(src), "CV%d", inst.mtx_src[i] + 1);
        snprintf(amt, sizeof(amt), "%+d%%", (int)(inst.mtx_amt[i] * 100.0f));
        int cw = TFT_getStringWidth("]");
        int src_x = _width - 150, src_w = TFT_getStringWidth(src);
        _fg = (i == pos && field == 1) ? TFT_CYAN : (color_t){170, 170, 180};
        TFT_print(src, src_x, y);
        if (i == pos && field == 1) { TFT_print("[", src_x - cw, y); TFT_print("]", src_x + src_w, y); }
        int amt_w = TFT_getStringWidth(amt), amt_x = _width - 10 - cw - amt_w;
        _fg = (inst.mtx_src[i] < 0) ? (color_t){80, 80, 90}
            : (i == pos && field == 2) ? TFT_CYAN : TFT_WHITE;
        TFT_print(amt, amt_x, y);
        if (i == pos && field == 2) { TFT_print("[", amt_x - cw, y); TFT_print("]", amt_x + amt_w, y); }
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("press: src > amt > done   turn: change", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int keys_matrix_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    static int pos = 0, field = 0;
    switch (event) {
        case EV_ENTERED_MENU: pos = 0; field = 0; mtx_redraw(pos, field); break;
        case EV_FWD:
            if (field == 1)      { int s = inst.mtx_src[pos] + 1; if (s > 7)  s = -1; inst.mtx_src[pos] = (int8_t)s; }
            else if (field == 2) { float a = inst.mtx_amt[pos] + 0.05f; if (a > 1.0f) a = 1.0f; inst.mtx_amt[pos] = a; }
            else { pos++; if (pos >= ISM_N) pos = -1; }
            mtx_redraw(pos, field);
            break;
        case EV_BWD:
            if (field == 1)      { int s = inst.mtx_src[pos] - 1; if (s < -1) s = 7; inst.mtx_src[pos] = (int8_t)s; }
            else if (field == 2) { float a = inst.mtx_amt[pos] - 0.05f; if (a < -1.0f) a = -1.0f; inst.mtx_amt[pos] = a; }
            else { pos--; if (pos < -1) pos = ISM_N - 1; }
            mtx_redraw(pos, field);
            break;
        case EV_SHORT_PRESS:
            if (pos == -1) return M_ISMP_SETUP;
            field = (field + 1) % 3;
            mtx_redraw(pos, field);
            break;
        case EV_LONG_PRESS: return M_ISMP_LIVE;
        default: break;
    }
    return 0;
}

// ---- Load Patch: a scrollable list of usr/keys/PAT_NNN.jsn (newest first) ---
// The Synth #23 browser page verbatim (over the shared preset_store).
#define KS_PATCH_MAX 64
static char s_patches[KS_PATCH_MAX][12];
static int  s_npatch = 0, s_patch_sel = 0;

static void patch_redraw(void)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print("Load Patch", 6, 4);
    menuTFTPrintAffordance("Setup", s_patch_sel == -1);
    if (s_npatch == 0) {
        _fg = (color_t){150, 150, 160};
        TFT_print("(no saved patches)", 8, fh + 20);
        return;
    }
    int row_h = fh + 5, y0 = fh + 14;
    int vis = (_height - fh - 6 - y0) / row_h;
    if (vis < 1) vis = 1;
    int top = 0;
    if (s_patch_sel >= vis) top = s_patch_sel - vis + 1;
    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= s_npatch) break;
        int y = y0 + r * row_h;
        _bg = (i == s_patch_sel) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == s_patch_sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 4, _bg);
        TFT_print(s_patches[i], 8, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn: pick   press: load   hold: back", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int keys_patch_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU:
            s_npatch = keys_patch_list(s_patches, KS_PATCH_MAX);
            s_patch_sel = s_npatch ? 0 : -1;
            patch_redraw();
            break;
        case EV_FWD:
            if (s_npatch) { s_patch_sel++; if (s_patch_sel >= s_npatch) s_patch_sel = -1; patch_redraw(); }
            break;
        case EV_BWD:
            if (s_npatch) { s_patch_sel--; if (s_patch_sel < -1) s_patch_sel = s_npatch - 1; patch_redraw(); }
            break;
        case EV_SHORT_PRESS:
            if (s_patch_sel < 0) return M_ISMP_SETUP;           // affordance / empty
            keys_patch_load(s_patches[s_patch_sel]);
            return M_ISMP_SETUP;
        case EV_LONG_PRESS: return M_ISMP_SETUP;
        default: break;
    }
    return 0;
}

static void keys_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_ISMP_LIVE);   menusys_item_set_default_cb(_ms, M_ISMP_LIVE, keys_live_handler);
    menusys_new_item(_ms, M_ISMP_SETUP);  menusys_item_set_default_cb(_ms, M_ISMP_SETUP, keys_setup_handler);
    menusys_new_item(_ms, M_ISMP_LOAD);   menusys_item_set_default_cb(_ms, M_ISMP_LOAD, keys_load_handler);
    menusys_new_item(_ms, M_ISMP_MATRIX); menusys_item_set_default_cb(_ms, M_ISMP_MATRIX, keys_matrix_handler);
    menusys_new_item(_ms, M_ISMP_PATCH);  menusys_item_set_default_cb(_ms, M_ISMP_PATCH, keys_patch_handler);
    menusys_new_item(_ms, M_ISMP_FX);     menusys_item_set_default_cb(_ms, M_ISMP_FX, keys_fx_handler);
}

static int keys_main_event(int event, void *ev_data)
{
    (void)ev_data;
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        char nm[12]; note_name_midi((int)lroundf(inst.note_disp), nm, sizeof(nm));
        _fg = TFT_LIGHTGREY;
        char s[64]; snprintf(s, sizeof(s), "Keys: %s  %s", nm, inst.zone[0].sample[0] ? inst.zone[0].sample : "(no sample)");
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const keys_main_items[] = { "Live", "Setup", "Matrix" };
static const int keys_main_targets[] = { M_ISMP_LIVE, M_ISMP_SETUP, M_ISMP_MATRIX };

const machine_ui_t keys_menu_ui = {
    .main_items = keys_main_items,
    .main_targets = keys_main_targets,
    .n_main = 3,
    .register_pages = keys_register_pages,
    .main_event = keys_main_event,
    .boot_target = M_ISMP_LIVE,
};
