// Shared parameter table — see ptab.h. No drawing in here: rows are painted by
// setup_menu, knobs are taken over by cvmtx.
#include "ptab.h"
#include <stdio.h>

uint16_t ptab_state(const ptab_t *t)
{
    return (t && t->state) ? t->state() : 0;
}

static inline bool met(uint16_t when, uint16_t unless, uint16_t st)
{ return (st & when) == when && !(st & unless); }

static const pt_row_t *row(const ptab_t *t, int i)
{
    return (t && i >= 0 && i < t->n_rows) ? &t->rows[i] : NULL;
}

bool ptab_row_live(const ptab_t *t, int i)
{
    const pt_row_t *r = row(t, i);
    return r && ((!r->when && !r->unless) || met(r->when, r->unless, ptab_state(t)));
}

setup_kind_t ptab_row_kind(const ptab_t *t, int i)
{
    const pt_row_t *r = row(t, i);
    if (!r) return ST_ACTION;
    switch (r->type) {
        case PT_BOOL: case PT_TOGGLE: return ST_TOGGLE;
        case PT_INT:  case PT_RANGE:  return ST_RANGE;
        default:                      return ST_ACTION;
    }
}

void ptab_row_render(const ptab_t *t, int i, char *buf, size_t n)
{
    const pt_row_t *r = row(t, i);
    if (n) buf[0] = 0;
    if (!r) return;
    if (r->render) { r->render(buf, n); return; }
    if (r->type == PT_BOOL && r->p)     snprintf(buf, n, "%s", *(volatile bool *)r->p ? "ON" : "OFF");
    else if (r->type == PT_INT && r->p) snprintf(buf, n, "%d", *(volatile int *)r->p);
}

void ptab_row_adjust(const ptab_t *t, int i, int dir)
{
    const pt_row_t *r = row(t, i);
    if (!r || r->type == PT_ACTION || !ptab_row_live(t, i)) return;
    if (r->adjust) { r->adjust(dir); return; }
    if (r->type == PT_BOOL && r->p) {
        volatile bool *b = (volatile bool *)r->p;
        *b = !*b;
    } else if (r->type == PT_INT && r->p) {
        volatile int *v = (volatile int *)r->p;
        int x = *v + (dir < 0 ? -1 : +1);
        *v = x < r->lo ? r->lo : (x > r->hi ? r->hi : x);
    }
}

int ptab_row_action(const ptab_t *t, int i)
{
    const pt_row_t *r = row(t, i);
    if (!r || r->type != PT_ACTION || !r->action || !ptab_row_live(t, i)) return 0;
    return r->action();
}

void ptab_save(const ptab_t *t, cJSON *o)
{
    if (!t || !o) return;
    for (int i = 0; i < t->n_rows; i++) {
        const pt_row_t *r = &t->rows[i];
        if (!r->key || !r->p) continue;
        if (r->type == PT_BOOL)     cJSON_AddBoolToObject(o, r->key, *(volatile bool *)r->p);
        else if (r->type == PT_INT) cJSON_AddNumberToObject(o, r->key, *(volatile int *)r->p);
    }
}

void ptab_load(const ptab_t *t, const cJSON *node)
{
    if (!t || !node) return;
    for (int i = 0; i < t->n_rows; i++) {
        const pt_row_t *r = &t->rows[i];
        if (!r->key || !r->p) continue;
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(node, r->key);
        if (!j) continue;
        if (r->type == PT_BOOL) {
            *(volatile bool *)r->p = cJSON_IsTrue(j);
        } else if (r->type == PT_INT && cJSON_IsNumber(j)) {
            int x = j->valueint;
            *(volatile int *)r->p = x < r->lo ? r->lo : (x > r->hi ? r->hi : x);
        }
    }
}

static int n_dests(const ptab_t *t)
{
    return !t ? 0 : (t->n_dests < 0 ? 0 : (t->n_dests > CVMTX_MAX ? CVMTX_MAX : t->n_dests));
}

void ptab_mtx_init(const ptab_t *t, cvmtx_t *m, ptab_rt_t *rt)
{
    int n = n_dests(t);
    for (int d = 0; d < CVMTX_MAX; d++) {
        rt->labels[d] = (d < n) ? t->dests[d].label : "";
        rt->defs[d]   = (d < n) ? t->dests[d].def_src : -1;
    }
    cvmtx_init(m, rt->labels, n, rt->defs);
    for (int d = 0; d < n; d++) cvmtx_set_takeover(m, d, t->dests[d].tk);
}

void ptab_knobs(const ptab_t *t, cvmtx_t *m, const int cvm[8])
{
    int n = n_dests(t);
    uint16_t st = ptab_state(t);
    for (int d = 0; d < n; d++)
        cvmtx_hold(m, d, !met(t->dests[d].when, t->dests[d].unless, st));
    cvmtx_track(m, cvm);
    for (int d = 0; d < n; d++) {
        float k;
        if (t->dests[d].apply && cvmtx_abs(m, d, &k)) t->dests[d].apply(k);
    }
}
