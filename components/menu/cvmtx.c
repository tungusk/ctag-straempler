// cvmtx — shared assignable CV matrix: state, conditioning, page, persistence.
// See cvmtx.h. The page is the synth matrix page generalized; nav state is a
// singleton (one menu page is ever active), the sample_browser idiom.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tft.h"
#include "tftspi.h"
#include "menutft.h"
#include "ui_events.h"
#include "cvmtx.h"
#include "machine.h"    // machine_state_dirty() for ABS knob moves

#define CVM_TAKEOVER 0.03f   // the hosts' knob threshold, lifted verbatim

void cvmtx_reset_defaults(cvmtx_t *m)
{
    for (int d = 0; d < CVMTX_MAX; d++) {
        m->src[d] = (m->def_src && d < m->n) ? m->def_src[d] : -1;
        m->mode[d] = (m->src[d] >= 0) ? CVM_ABS : CVM_OFFSET;
        m->amt[d] = 0.0f;
    }
    m->rearm = true;
}

void cvmtx_init(cvmtx_t *m, const char *const *labels, int n, const int8_t *def_src)
{
    memset(m, 0, sizeof(*m));
    m->labels = labels;
    m->n = (n > CVMTX_MAX) ? CVMTX_MAX : n;
    m->def_src = def_src;
    m->skip_src = -1;
    m->floor12[0] = m->floor12[1] = 4095;   // trackers converge down on first reads
    cvmtx_reset_defaults(m);
}

void cvmtx_rearm(cvmtx_t *m) { m->rearm = true; }

bool cvmtx_is_default(const cvmtx_t *m, int d)
{
    if (!m->def_src || d < 0 || d >= m->n || m->def_src[d] < 0) return false;
    return m->src[d] == m->def_src[d] && m->mode[d] == CVM_ABS;
}

void cvmtx_track(cvmtx_t *m, const int cvm[8])
{
    for (int c = 0; c < 2; c++) {
        if (cvm[c] < m->floor12[c]) m->floor12[c] = cvm[c];
        else if (m->floor12[c] < 4095) m->floor12[c]++;   // slow drift back up
    }
    // ABS takeover: capture on (re)arm, go live past the threshold, hold on
    // the clock channel, flag committed moves for the autosave (hysteresis —
    // a live knob tracks every block)
    bool rearm = m->rearm;
    m->rearm = false;
    for (int d = 0; d < m->n; d++) {
        if (m->mode[d] != CVM_ABS || m->src[d] < 0) { m->live[d] = false; continue; }
        float k = cvmtx_cv01(m, cvm, m->src[d]);
        m->pos01[d] = k;
        if (rearm || m->src[d] == m->skip_src) {
            m->capt[d] = k; m->live[d] = false; m->last01[d] = -1.0f;
            continue;
        }
        if (!m->live[d] && fabsf(k - m->capt[d]) > CVM_TAKEOVER) m->live[d] = true;
        if (m->live[d] && !(m->nodirty & (1u << d)) &&
            (m->last01[d] < 0 || fabsf(k - m->last01[d]) > CVM_TAKEOVER)) {
            m->last01[d] = k;
            machine_state_dirty();
        }
    }
}

bool cvmtx_abs(const cvmtx_t *m, int d, float *k01)
{
    if (d < 0 || d >= m->n || m->mode[d] != CVM_ABS || m->src[d] < 0 || !m->live[d]) return false;
    *k01 = m->pos01[d];   // this block's conditioned read (cvmtx_track ran first)
    return true;
}

float cvmtx_cv01(const cvmtx_t *m, const int cvm[8], int src)
{
    int c = cvm[src & 7];
    if ((src & 7) < 2) {
        int fl = m->floor12[src & 7];
        if (fl > 3800) return 0.0f;                     // not converged / dead jack
        c = (int)((int32_t)(c - fl) * 4095 / (4095 - fl));
        if (c < 0) c = 0;
        if (c > 4095) c = 4095;
    }
    return (float)c / 4095.0f;
}

float cvmtx_val(const cvmtx_t *m, const int cvm[8], int d)
{
    if (d < 0 || d >= m->n || m->src[d] < 0 || m->mode[d] == CVM_ABS) return 0.0f;
    return m->amt[d] * cvmtx_cv01(m, cvm, m->src[d]);
}

bool cvmtx_any(const cvmtx_t *m)
{
    for (int d = 0; d < m->n; d++) if (m->src[d] >= 0) return true;
    return false;
}

// ---- the page ----------------------------------------------------------------
static int s_pos = 0, s_field = 0;   // field: 0 nav / 1 edit src / 2 edit amt

static void mtx_redraw(const cvmtx_t *m, const char *title)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print((char *)title, 6, 4);
    menuTFTPrintAffordance("Setup", s_pos == -1);
    int row_h = fh + 5, y0 = fh + 14;
    int vis = (_height - fh - 6 - y0) / row_h;
    if (vis < 1) vis = 1;
    int top = 0;
    if (s_pos >= vis) top = s_pos - vis + 1;
    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= m->n) break;
        int y = y0 + r * row_h;
        _bg = (i == s_pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        TFT_fillRect(0, y - 2, _width, fh + 4, _bg);
        _fg = TFT_WHITE;
        char lab[20];   // a default (knob) entry that still matches its default wears a dot
        snprintf(lab, sizeof(lab), "%s%s", cvmtx_is_default(m, i) ? "\xb7" : "", m->labels[i]);
        TFT_print(lab, 8, y);
        char src[8], amt[10];
        if (m->src[i] < 0) snprintf(src, sizeof(src), "off");
        else               snprintf(src, sizeof(src), "CV%d", m->src[i] + 1);
        if (m->mode[i] == CVM_ABS) snprintf(amt, sizeof(amt), "knob");   // ABSOLUTE: takeover
        else snprintf(amt, sizeof(amt), "%+d%%", (int)(m->amt[i] * 100.0f));
        // fixed positions + brackets that hug the value -> nothing shifts on select
        int cw = TFT_getStringWidth("]");
        int src_x = _width - 150, src_w = TFT_getStringWidth(src);
        _fg = (i == s_pos && s_field == 1) ? TFT_CYAN : (color_t){170, 170, 180};
        TFT_print(src, src_x, y);
        if (i == s_pos && s_field == 1) { TFT_print("[", src_x - cw, y); TFT_print("]", src_x + src_w, y); }
        int amt_w = TFT_getStringWidth(amt), amt_x = _width - 10 - cw - amt_w;
        _fg = (m->src[i] < 0) ? (color_t){80, 80, 90}
            : (i == s_pos && s_field == 2) ? TFT_CYAN : TFT_WHITE;
        TFT_print(amt, amt_x, y);
        if (i == s_pos && s_field == 2) { TFT_print("[", amt_x - cw, y); TFT_print("]", amt_x + amt_w, y); }
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("press: src > amt > done   amt past +100 = knob", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
    _bg = TFT_BLACK;
}

int cvmtx_menu_event(cvmtx_t *m, int event, const char *title,
                     int ret_page, int live_page)
{
    switch (event) {
        case EV_ENTERED_MENU: s_pos = 0; s_field = 0; mtx_redraw(m, title); break;
        case EV_FWD:
            if (s_field == 1)      { int s = m->src[s_pos] + 1; if (s > 7)  s = -1; m->src[s_pos] = (int8_t)s; m->rearm = true; }
            else if (s_field == 2) {   // amount ... +100% > knob (ABSOLUTE)
                if (m->mode[s_pos] != CVM_ABS) {
                    float a = m->amt[s_pos] + 0.05f;
                    if (a > 1.001f) { m->mode[s_pos] = CVM_ABS; m->amt[s_pos] = 1.0f; m->rearm = true; }
                    else m->amt[s_pos] = a > 1.0f ? 1.0f : a;
                }
            }
            else { s_pos++; if (s_pos >= m->n) s_pos = -1; }
            mtx_redraw(m, title);
            break;
        case EV_BWD:
            if (s_field == 1)      { int s = m->src[s_pos] - 1; if (s < -1) s = 7; m->src[s_pos] = (int8_t)s; m->rearm = true; }
            else if (s_field == 2) {   // knob > +100% > ... -100%
                if (m->mode[s_pos] == CVM_ABS) { m->mode[s_pos] = CVM_OFFSET; m->amt[s_pos] = 1.0f; }
                else { float a = m->amt[s_pos] - 0.05f; if (a < -1.0f) a = -1.0f; m->amt[s_pos] = a; }
            }
            else { s_pos--; if (s_pos < -1) s_pos = m->n - 1; }
            mtx_redraw(m, title);
            break;
        case EV_SHORT_PRESS:
            if (s_pos == -1) return ret_page;
            s_field = (s_field + 1) % 3;                 // nav -> src -> amt -> nav
            mtx_redraw(m, title);
            break;
        case EV_LONG_PRESS: return live_page;
        default: break;
    }
    return 0;
}

// ---- persistence ---------------------------------------------------------------
void cvmtx_save(const cvmtx_t *m, cJSON *o)
{
    cJSON *ms = cJSON_AddArrayToObject(o, "mxs");
    cJSON *ma = cJSON_AddArrayToObject(o, "mxa");
    cJSON *mm = cJSON_AddArrayToObject(o, "mxm");   // modes (0 offset / 1 knob)
    cJSON *ml = cJSON_AddArrayToObject(o, "mxl");   // destination names, so the web
    cJSON *md = cJSON_AddArrayToObject(o, "mxd");   //   matrix editor can label its rows
    for (int d = 0; d < m->n; d++) {                //   and mark/revert the defaults
        cJSON_AddItemToArray(ms, cJSON_CreateNumber(m->src[d]));
        cJSON_AddItemToArray(mm, cJSON_CreateNumber(m->mode[d]));
        cJSON_AddItemToArray(md, cJSON_CreateNumber(m->def_src ? m->def_src[d] : -1));
        cJSON_AddItemToArray(ma, cJSON_CreateNumber(
            (int)(m->amt[d] * 100.0f + (m->amt[d] < 0 ? -0.5f : 0.5f))));
        cJSON_AddItemToArray(ml, cJSON_CreateString(m->labels && m->labels[d] ? m->labels[d] : ""));
    }
}

void cvmtx_load(cvmtx_t *m, const cJSON *node)
{
    if (!node) return;
    cJSON *ms = cJSON_GetObjectItemCaseSensitive(node, "mxs");
    cJSON *ma = cJSON_GetObjectItemCaseSensitive(node, "mxa");
    bool legacy = false;
    if (!cJSON_IsArray(ms)) {
        // pre-cvmtx synth/keys matrices: "msrc"/"mamt", amounts as raw floats
        ms = cJSON_GetObjectItemCaseSensitive(node, "msrc");
        ma = cJSON_GetObjectItemCaseSensitive(node, "mamt");
        if (!cJSON_IsArray(ms)) return;                 // pre-matrix preset: leave defaults
        legacy = true;
    }
    cJSON *mm = cJSON_GetObjectItemCaseSensitive(node, "mxm");
    bool have_modes = cJSON_IsArray(mm);
    for (int d = 0; d < m->n; d++) {
        cJSON *si = cJSON_GetArrayItem(ms, d);
        cJSON *ai = cJSON_IsArray(ma) ? cJSON_GetArrayItem(ma, d) : NULL;
        cJSON *mi = have_modes ? cJSON_GetArrayItem(mm, d) : NULL;
        if (cJSON_IsNumber(si)) {
            int v = si->valueint;
            m->src[d] = (v < -1 || v > 7) ? -1 : (int8_t)v;
        }
        if (cJSON_IsNumber(ai)) {
            float a = legacy ? (float)ai->valuedouble
                             : (float)ai->valueint / 100.0f;
            m->amt[d] = a < -1.0f ? -1.0f : a > 1.0f ? 1.0f : a;
        }
        if (have_modes) m->mode[d] = (cJSON_IsNumber(mi) && mi->valueint == CVM_ABS) ? CVM_ABS : CVM_OFFSET;
        else            m->mode[d] = CVM_OFFSET;     // pre-stage-3 preset: everything was an offset
    }
    if (!have_modes && m->def_src) {
        // MIGRATION: the knobs this preset relied on were hard-wired in the
        // host; re-create them as ABS entries wherever the destination is still
        // free. A destination already carrying an OFFSET keeps it, single-driven.
        for (int d = 0; d < m->n; d++)
            if (m->def_src[d] >= 0 && m->src[d] < 0) {
                m->src[d] = m->def_src[d];
                m->mode[d] = CVM_ABS;
                m->amt[d] = 0.0f;
            }
    }
    m->rearm = true;
}
