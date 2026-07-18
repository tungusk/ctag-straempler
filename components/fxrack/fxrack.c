// fxrack — shared curated FX slot rack. See fxrack.h.
#include <math.h>
#include "fxrack.h"
#include "fxchain.h"
#include "audio.h"       // audio_proc_us() for the cost-guard

static inline float clampf(float x, float lo, float hi){ return x < lo ? lo : x > hi ? hi : x; }

// ---- per-effect param descriptors (operate through the rack's pointers) -------
typedef struct { const char *label; setup_kind_t kind;
                 void (*fmt)(const fxrack_t *, char *, size_t);
                 void (*adj)(const fxrack_t *, int); } fx_p_t;
typedef struct { const char *name; const fx_p_t *p; int np; } fx_desc_t;

// overdrive
static void od_fd(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->od->drive*100);}
static void od_ad(const fxrack_t*r,int d){r->od->drive=clampf(r->od->drive+d*0.05f,0,1);}
static void od_ft(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->od->tone*100);}
static void od_at(const fxrack_t*r,int d){r->od->tone=clampf(r->od->tone+d*0.05f,0,1);}
static void od_fb(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%+.0f%%",r->od->bias*100);}
static void od_ab(const fxrack_t*r,int d){r->od->bias=clampf(r->od->bias+d*0.05f,-1,1);}
static void od_fl(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->od->level*100);}
static void od_al(const fxrack_t*r,int d){r->od->level=clampf(r->od->level+d*0.05f,0,1);}
static const fx_p_t od_params[]={{"Drive",ST_RANGE,od_fd,od_ad},{"Tone",ST_RANGE,od_ft,od_at},
    {"Bias",ST_RANGE,od_fb,od_ab},{"Level",ST_RANGE,od_fl,od_al}};

// flanger
static void fl_fr(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.2f Hz",r->flg->rate);}
static void fl_ar(const fxrack_t*r,int d){r->flg->rate=clampf(r->flg->rate+d*0.05f,0.01f,10);}
static void fl_fd(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->flg->depth*100);}
static void fl_ad(const fxrack_t*r,int d){r->flg->depth=clampf(r->flg->depth+d*0.05f,0,1);}
static void fl_ff(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%+.0f%%",r->flg->fb*100);}
static void fl_af(const fxrack_t*r,int d){r->flg->fb=clampf(r->flg->fb+d*0.05f,-0.95f,0.95f);}
static void fl_fm(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->flg->wet*100);}
static void fl_am(const fxrack_t*r,int d){r->flg->wet=clampf(r->flg->wet+d*0.05f,0,1);}
static const fx_p_t flg_params[]={{"Rate",ST_RANGE,fl_fr,fl_ar},{"Depth",ST_RANGE,fl_fd,fl_ad},
    {"Fdbk",ST_RANGE,fl_ff,fl_af},{"Mix",ST_RANGE,fl_fm,fl_am}};

// tremolo
static void tr_fr(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.2f Hz",r->trem->rate);}
static void tr_ar(const fxrack_t*r,int d){r->trem->rate=clampf(r->trem->rate+d*0.25f,0.05f,20);}
static void tr_fd(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->trem->depth*100);}
static void tr_ad(const fxrack_t*r,int d){r->trem->depth=clampf(r->trem->depth+d*0.05f,0,1);}
static void tr_fs(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%s",r->trem->shape==TREM_TRI?"Tri":r->trem->shape==TREM_SQR?"Sqr":"Sine");}
static void tr_as(const fxrack_t*r,int d){(void)d;r->trem->shape=(r->trem->shape+1)%3;}
static void tr_fst(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%s",r->trem->stereo?"ON":"OFF");}
static void tr_ast(const fxrack_t*r,int d){(void)d;r->trem->stereo=!r->trem->stereo;}
static const fx_p_t trem_params[]={{"Rate",ST_RANGE,tr_fr,tr_ar},{"Depth",ST_RANGE,tr_fd,tr_ad},
    {"Shape",ST_TOGGLE,tr_fs,tr_as},{"Stereo",ST_TOGGLE,tr_fst,tr_ast}};

// delay
static void dl_ft(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%d ms",(int)(fxdelay_time_ms(r->dly)+0.5f));}
static void dl_at(const fxrack_t*r,int d){if(r->dly->bufL)fxdelay_set_time_ms(r->dly,fxdelay_time_ms(r->dly)+d*10.0f);}
static void dl_ff(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->dly->fb*100);}
static void dl_af(const fxrack_t*r,int d){if(r->dly->bufL)fxdelay_set_feedback(r->dly,r->dly->fb+d*0.05f);}
static void dl_fm(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->dly->wet*100);}
static void dl_am(const fxrack_t*r,int d){if(r->dly->bufL)fxdelay_set_mix(r->dly,r->dly->wet+d*0.05f);}
static void dl_fto(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->dly->damp*100);}
static void dl_ato(const fxrack_t*r,int d){if(r->dly->bufL)fxdelay_set_damp(r->dly,r->dly->damp+d*0.05f);}
static void dl_fp(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%s",r->dly->pingpong?"Ping-Pong":"Stereo");}
static void dl_ap(const fxrack_t*r,int d){(void)d;if(r->dly->bufL)fxdelay_set_pingpong(r->dly,!r->dly->pingpong);}
static const fx_p_t dly_params[]={{"Time",ST_RANGE,dl_ft,dl_at},{"Fdbk",ST_RANGE,dl_ff,dl_af},
    {"Mix",ST_RANGE,dl_fm,dl_am},{"Tone",ST_RANGE,dl_fto,dl_ato},{"Ping",ST_TOGGLE,dl_fp,dl_ap}};

// filter
static void ft_fm(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%s",r->filt->mode==FILT_LP?"LP":r->filt->mode==FILT_HP?"HP":"BP");}
static void ft_am(const fxrack_t*r,int d){(void)d;r->filt->mode=(r->filt->mode+1)%FILT_NMODE;}
static void ft_fc(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->filt->cutoff*100);}
static void ft_ac(const fxrack_t*r,int d){r->filt->cutoff=clampf(r->filt->cutoff+d*0.05f,0,1);}
static void ft_fq(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->filt->reso*100);}
static void ft_aq(const fxrack_t*r,int d){r->filt->reso=clampf(r->filt->reso+d*0.05f,0,1);}
static const fx_p_t filt_params[]={{"Mode",ST_TOGGLE,ft_fm,ft_am},{"Cutoff",ST_RANGE,ft_fc,ft_ac},
    {"Reso",ST_RANGE,ft_fq,ft_aq}};

// band filter (base/width) — reuses band->cutoff as base, band->reso as width
static void bn_fb(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->band->cutoff*100);}
static void bn_ab(const fxrack_t*r,int d){r->band->cutoff=clampf(r->band->cutoff+d*0.05f,0,1);}
static void bn_fw(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->band->reso*100);}
static void bn_aw(const fxrack_t*r,int d){r->band->reso=clampf(r->band->reso+d*0.05f,0,1);}
static const fx_p_t band_params[]={{"Base",ST_RANGE,bn_fb,bn_ab},{"Width",ST_RANGE,bn_fw,bn_aw}};

// reverb (FX3)
static void rv_fx(const fxrack_t*r,char*v,size_t n){snprintf(v,n,"%.0f%%",r->rv->wet*100);}
static void rv_ax(const fxrack_t*r,int d){reverb_set_mix(r->rv,clampf(r->rv->wet+d*0.05f,0,1));}
static const fx_p_t rev_params[]={{"Rev Mix",ST_RANGE,rv_fx,rv_ax}};
static const fx_desc_t rev_desc={"Reverb",rev_params,1};

static const fx_desc_t gen_desc[FXK_NGEN]={
    [FXK_OFF]={"Off",NULL,0},           [FXK_OD]={"Overdrive",od_params,4},
    [FXK_FLG]={"Flanger",flg_params,4}, [FXK_TREM]={"Tremolo",trem_params,4},
    [FXK_DLY]={"Delay",dly_params,5},   [FXK_FILT]={"Filter",filt_params,3},
    [FXK_BAND]={"Band",band_params,2},
};

// ---- process ------------------------------------------------------------------
void fxrack_process_i32(const fxrack_t *rk, int32_t *out, int frames)
{
    float fb[FX_SCRATCH_N];
    fx_unpack_i32(out, fb, frames * 2);
    for (int s = 0; s < FX_NSLOT_GEN; s++) {
        switch (rk->slot[s]) {
            case FXK_OD:   overdrive_block_f(rk->od, fb, frames); break;
            case FXK_FLG:  if (rk->flg->bufL) flanger_block_f(rk->flg, fb, frames); break;
            case FXK_TREM: tremolo_block_f(rk->trem, fb, frames); break;
            case FXK_DLY:  if (rk->dly->bufL) fxdelay_block_f(rk->dly, fb, frames); break;
            case FXK_FILT: fxfilter_block_f(rk->filt, fb, frames); break;
            case FXK_BAND: fxfilter_band_block_f(rk->band, fb, frames); break;
            default: break;
        }
    }
    if (rk->rv->mode != RV_OFF && rk->rv->slab) reverb_block_f(rk->rv, fb, frames);
    fx_pack_softclip(fb, out, frames * 2);
}

// ---- menu ---------------------------------------------------------------------
const char *fxrack_slot_name(const fxrack_t *rk, int slot)
{
    if (slot == 2) return rk->rv->mode != RV_OFF ? reverb_mode_name(rk->rv->mode) : "Off";
    return gen_desc[rk->slot[slot]].name;
}

// assign effect `kind` to `slot`: lazy-init buffers + audible defaults, enforce
// one-effect-per-slot.
static void slot_set(const fxrack_t *rk, int slot, int kind)
{
    if (kind == FXK_DLY) { if (!rk->dly->bufL) fxdelay_init(rk->dly);
        if (!rk->dly->bufL) kind = FXK_OFF; else if (rk->dly->wet < 0.01f) fxdelay_set_mix(rk->dly, 0.30f); }
    else if (kind == FXK_FLG) { if (!rk->flg->bufL) flanger_init(rk->flg);
        if (!rk->flg->bufL) kind = FXK_OFF; else if (rk->flg->wet < 0.01f) rk->flg->wet = 0.5f; }
    else if (kind == FXK_OD) { if (rk->od->level < 0.01f) { rk->od->drive = 0.4f; rk->od->tone = 0.5f; rk->od->level = 0.8f; } overdrive_reset(rk->od); }
    else if (kind == FXK_TREM) { if (rk->trem->depth < 0.01f) { rk->trem->depth = 0.5f; rk->trem->rate = 5.0f; } }
    for (int s = 0; s < FX_NSLOT_GEN; s++) if (s != slot && rk->slot[s] == kind && kind != FXK_OFF) rk->slot[s] = FXK_OFF;
    rk->slot[slot] = kind;
}

int fxrack_menu_rows(const fxrack_t *rk, int slot, setup_item_t *items, int8_t *pr)
{
    int r = 0;
    items[r].label = "Effect"; items[r].kind = ST_TOGGLE; pr[r] = -1; r++;
    if (slot == 2) {
        if (rk->rv->mode != RV_OFF)
            for (int p = 0; p < rev_desc.np && r < FXRACK_MAXROWS; p++) {
                items[r].label = rev_desc.p[p].label; items[r].kind = rev_desc.p[p].kind; pr[r] = p; r++; }
    } else {
        int k = rk->slot[slot];
        if (k != FXK_OFF)
            for (int p = 0; p < gen_desc[k].np && r < FXRACK_MAXROWS; p++) {
                items[r].label = gen_desc[k].p[p].label; items[r].kind = gen_desc[k].p[p].kind; pr[r] = p; r++; }
    }
    return r;
}

void fxrack_menu_val(const fxrack_t *rk, int slot, int param, char *v, size_t n)
{
    v[0] = 0;
    if (param < 0) {                                   // effect-select row
        snprintf(v, n, "%s", slot == 2 ? reverb_mode_name(rk->rv->mode) : gen_desc[rk->slot[slot]].name);
        return;
    }
    if (slot == 2) rev_desc.p[param].fmt(rk, v, n);
    else gen_desc[rk->slot[slot]].p[param].fmt(rk, v, n);
}

void fxrack_menu_adj(const fxrack_t *rk, int slot, int param, int dir)
{
    if (param < 0) {                                   // change the effect
        if (slot == 2) {
            int m = rk->rv->mode + dir;
            if (m < 0) m = RV_N_MODES - 1;
            if (m >= RV_N_MODES) m = RV_OFF;
            if (m != RV_OFF && !rk->rv->slab && reverb_init(rk->rv) != ESP_OK) m = RV_OFF;
            reverb_set_mode(rk->rv, m);
        } else {
            int k = rk->slot[slot] + dir;
            if (k < 0) k = FXK_NGEN - 1;
            if (k >= FXK_NGEN) k = FXK_OFF;
            slot_set(rk, slot, k);
        }
        return;
    }
    if (slot == 2) rev_desc.p[param].adj(rk, dir);
    else gen_desc[rk->slot[slot]].p[param].adj(rk, dir);
}

// ---- autosave (owns the whole FX serialization) -------------------------------
void fxrack_save(const fxrack_t *rk, cJSON *o)
{
    cJSON *sl = cJSON_AddArrayToObject(o, "fxsl");
    for (int s = 0; s < FX_NSLOT_GEN; s++) cJSON_AddItemToArray(sl, cJSON_CreateNumber(rk->slot[s]));
    cJSON_AddNumberToObject(o, "rv", rk->rv->mode);
    cJSON_AddNumberToObject(o, "rvmx", (int)(rk->rv->wet * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "dlyt", (int)(fxdelay_time_ms(rk->dly) + 0.5f));
    cJSON_AddNumberToObject(o, "dlyfb", (int)(rk->dly->fb * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "dlymx", (int)(rk->dly->wet * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "dlytn", (int)(rk->dly->damp * 100 + 0.5f));
    cJSON_AddBoolToObject(o, "dlypp", rk->dly->pingpong);
    cJSON_AddNumberToObject(o, "oddr", (int)(rk->od->drive * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "odtn", (int)(rk->od->tone * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "odbs", (int)(rk->od->bias * 100 + (rk->od->bias < 0 ? -0.5f : 0.5f)));
    cJSON_AddNumberToObject(o, "odlv", (int)(rk->od->level * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "flgrt", (int)(rk->flg->rate * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "flgdp", (int)(rk->flg->depth * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "flgfb", (int)(rk->flg->fb * 100 + (rk->flg->fb < 0 ? -0.5f : 0.5f)));
    cJSON_AddNumberToObject(o, "flgmx", (int)(rk->flg->wet * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "trmrt", (int)(rk->trem->rate * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "trmdp", (int)(rk->trem->depth * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "trmsh", rk->trem->shape);
    cJSON_AddBoolToObject(o, "trmst", rk->trem->stereo);
    cJSON_AddNumberToObject(o, "fim", rk->filt->mode);
    cJSON_AddNumberToObject(o, "fic", (int)(rk->filt->cutoff * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "fiq", (int)(rk->filt->reso * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "bnb", (int)(rk->band->cutoff * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "bnw", (int)(rk->band->reso * 100 + 0.5f));
}

void fxrack_load(const fxrack_t *rk, const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rv")) && cJSON_IsNumber(j)) {
        int m = j->valueint; if (m < 0 || m >= RV_N_MODES) m = RV_OFF;
        if (m != RV_OFF && !rk->rv->slab && reverb_init(rk->rv) != ESP_OK) m = RV_OFF;
        reverb_set_mode(rk->rv, m);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rvmx")) && cJSON_IsNumber(j)) reverb_set_mix(rk->rv, (float)j->valueint / 100.0f);
    // slots, or migrate from legacy on/off bools
    cJSON *sl = cJSON_GetObjectItemCaseSensitive(node, "fxsl");
    if (cJSON_IsArray(sl)) {
        for (int s = 0; s < FX_NSLOT_GEN; s++) {
            cJSON *si = cJSON_GetArrayItem(sl, s);
            int v = cJSON_IsNumber(si) ? si->valueint : FXK_OFF;
            rk->slot[s] = (v < 0 || v >= FXK_NGEN) ? FXK_OFF : (int8_t)v;
        }
    } else {
        int s = 0;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node, "od"))   && s < FX_NSLOT_GEN) rk->slot[s++] = FXK_OD;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node, "flg"))  && s < FX_NSLOT_GEN) rk->slot[s++] = FXK_FLG;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node, "trem")) && s < FX_NSLOT_GEN) rk->slot[s++] = FXK_TREM;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node, "dly"))  && s < FX_NSLOT_GEN) rk->slot[s++] = FXK_DLY;
        while (s < FX_NSLOT_GEN) rk->slot[s++] = FXK_OFF;
    }
    for (int s = 0; s < FX_NSLOT_GEN; s++) {   // ensure buffers exist for slots that need them
        if (rk->slot[s] == FXK_DLY && !rk->dly->bufL && fxdelay_init(rk->dly) != ESP_OK) rk->slot[s] = FXK_OFF;
        if (rk->slot[s] == FXK_FLG && !rk->flg->bufL && flanger_init(rk->flg) != ESP_OK) rk->slot[s] = FXK_OFF;
    }
    if (rk->dly->bufL) {
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlyt"))  && cJSON_IsNumber(j)) fxdelay_set_time_ms(rk->dly, (float)j->valueint);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlyfb")) && cJSON_IsNumber(j)) fxdelay_set_feedback(rk->dly, (float)j->valueint / 100.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlymx")) && cJSON_IsNumber(j)) fxdelay_set_mix(rk->dly, (float)j->valueint / 100.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlytn")) && cJSON_IsNumber(j)) fxdelay_set_damp(rk->dly, (float)j->valueint / 100.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlypp")) && cJSON_IsBool(j))   fxdelay_set_pingpong(rk->dly, cJSON_IsTrue(j));
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "oddr")) && cJSON_IsNumber(j)) rk->od->drive = clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "odtn")) && cJSON_IsNumber(j)) rk->od->tone = clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "odbs")) && cJSON_IsNumber(j)) rk->od->bias = clampf((float)j->valueint / 100.0f, -1, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "odlv")) && cJSON_IsNumber(j)) rk->od->level = clampf((float)j->valueint / 100.0f, 0, 1);
    if (rk->flg->bufL) {
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgrt")) && cJSON_IsNumber(j)) rk->flg->rate = clampf((float)j->valueint / 100.0f, 0.01f, 10);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgdp")) && cJSON_IsNumber(j)) rk->flg->depth = clampf((float)j->valueint / 100.0f, 0, 1);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgfb")) && cJSON_IsNumber(j)) rk->flg->fb = clampf((float)j->valueint / 100.0f, -0.95f, 0.95f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgmx")) && cJSON_IsNumber(j)) rk->flg->wet = clampf((float)j->valueint / 100.0f, 0, 1);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmrt")) && cJSON_IsNumber(j)) rk->trem->rate = clampf((float)j->valueint / 100.0f, 0.05f, 20);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmdp")) && cJSON_IsNumber(j)) rk->trem->depth = clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmsh")) && cJSON_IsNumber(j)) { int s = j->valueint; rk->trem->shape = (s < 0 || s >= TREM_NSHAPE) ? TREM_SINE : s; }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmst")) && cJSON_IsBool(j)) rk->trem->stereo = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fim")) && cJSON_IsNumber(j)) { int m = j->valueint; rk->filt->mode = (m < 0 || m >= FILT_NMODE) ? FILT_LP : m; }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fic")) && cJSON_IsNumber(j)) rk->filt->cutoff = clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fiq")) && cJSON_IsNumber(j)) rk->filt->reso = clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "bnb")) && cJSON_IsNumber(j)) rk->band->cutoff = clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "bnw")) && cJSON_IsNumber(j)) rk->band->reso = clampf((float)j->valueint / 100.0f, 0, 1);
}
