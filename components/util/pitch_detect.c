// Monophonic pitch detection (see pitch_detect.h). YIN, run coarse on a 4x
// decimated copy of the window and refined at the native rate around the coarse
// lag: the coarse pass is what costs (a 27.5 Hz..1.5 kHz lag sweep), and running
// it at 11 kHz makes it ~16x cheaper than full rate, while the refine pass buys
// the accuracy back for a few thousand extra multiplies. Everything is windowed
// at 4096 native frames (93 ms at 44.1k) — long enough for two periods of the
// lowest note we claim, short enough to stay inside a vibrato.
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include "pitch_detect.h"

#define PD_DEC      4                        // decimation factor for the coarse pass
#define PD_WIN      4096                     // native-rate analysis window (frames)
#define PD_DWIN     (PD_WIN / PD_DEC)        // decimated window
#define PD_TAUMAX   (PD_DWIN / 2)            // longest lag the window supports
#define PD_FMIN     27.5f                    // A0
#define PD_FMAX     1500.0f                  // coarse ceiling (instrument fundamentals)
#define PD_THRESH   0.15f                    // YIN absolute threshold
#define PD_APERIOD  0.60f                    // CMND above this = not pitched
#define PD_RMS_MIN  60.0f                    // int16 scale, ~-55 dBFS = silence
#define PD_MAXWIN   6                        // windows per whole-sample scan

size_t pitch_scratch_bytes(void)
{
    return (size_t)(PD_DWIN + 2 * (PD_TAUMAX + 1)) * sizeof(float);
}

void pitch_from_hz(float hz, pitch_result_t *out)
{
    if (!out) return;
    if (hz <= 0.0f) { out->midi = 0; out->cents = 0.0f; return; }
    float m = 69.0f + 12.0f * log2f(hz / 440.0f);
    int   n = (int)lroundf(m);
    out->midi  = n;
    out->cents = (m - (float)n) * 100.0f;
}

// Note name out of a sample id. Sample ids are FatFS 8.3 short (<=8 chars), so
// the note is usually crammed against the instrument name ("PNOC4") — hence the
// `isolated` report rather than a hard accept/reject: the caller decides how far
// to trust a match that might just be the tail of a word.
int pitch_name_hint(const char *id, bool *isolated)
{
    static const int SEMI[7] = { 9, 11, 0, 2, 4, 5, 7 };    // A B C D E F G
    if (isolated) *isolated = false;
    if (!id) return -1;

    int best = -1;
    bool best_iso = false;
    for (int i = 0; id[i]; i++) {
        int c = toupper((unsigned char)id[i]);
        if (c < 'A' || c > 'G') continue;
        int j = i + 1, acc = 0;
        int nx = toupper((unsigned char)id[j]);
        if (nx == '#' || nx == 'S') { acc = 1; j++; }
        else if (nx == 'B' && isdigit((unsigned char)id[j + 1])) { acc = -1; j++; }
        if (!isdigit((unsigned char)id[j])) continue;        // no octave = no hint
        int oct = id[j] - '0';
        j++;
        if (isalnum((unsigned char)id[j])) continue;         // token must end here
        int midi = (oct + 1) * 12 + SEMI[c - 'A'] + acc;
        if (midi < 0 || midi > 127) continue;
        // a letter glued to the left means it may just be part of a word
        bool iso = (i == 0) || !isalpha((unsigned char)id[i - 1]);
        // isolated beats not; among equals the LAST wins ("EP_C4" ends in it)
        if (best < 0 || iso >= best_iso) { best = midi; best_iso = iso; }
    }
    if (best >= 0 && isolated) *isolated = best_iso;
    return best;
}

void pitch_note_name(int midi, char *out, size_t n)
{
    static const char *const NAMES[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (!out || n == 0) return;
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    snprintf(out, n, "%s%d", NAMES[midi % 12], midi / 12 - 1);
}

int pitch_detect_window(const int16_t *buf, uint32_t n, float rate,
                        void *scratch, pitch_result_t *out)
{
    if (!buf || !out || rate < 1000.0f || n < PD_WIN / 2) return -1;

    void *mine = NULL;
    if (!scratch) {
        mine = malloc(pitch_scratch_bytes());
        if (!mine) return -1;                    // fail soft: no pitch, no crash
        scratch = mine;
    }
    float *dw = (float *)scratch;                // decimated window
    float *d  = dw + PD_DWIN;                    // difference function
    float *cm = d + (PD_TAUMAX + 1);             // cumulative mean normalized d
    int rc = -1;

    uint32_t nw = n < PD_WIN ? n : PD_WIN;       // native frames consumed
    int dn = (int)(nw / PD_DEC);
    if (dn > PD_DWIN) dn = PD_DWIN;
    if (dn < 128) goto done;

    // decimate by 4 with a 4-tap box average (nulls at fs/4 and fs/2 — enough
    // anti-aliasing for a lag search), then remove DC and gate on level
    double sum = 0.0;
    for (int i = 0; i < dn; i++) {
        const int16_t *p = buf + (size_t)i * PD_DEC;
        float v = ((float)p[0] + (float)p[1] + (float)p[2] + (float)p[3]) * 0.25f;
        dw[i] = v;
        sum += v;
    }
    float mean = (float)(sum / dn);
    double energy = 0.0;
    for (int i = 0; i < dn; i++) { dw[i] -= mean; energy += (double)dw[i] * dw[i]; }
    if (sqrtf((float)(energy / dn)) < PD_RMS_MIN) goto done;    // silent window

    float drate = rate / (float)PD_DEC;
    int tmin = (int)(drate / PD_FMAX); if (tmin < 2) tmin = 2;
    int tmax = (int)(drate / PD_FMIN);
    if (tmax > PD_TAUMAX) tmax = PD_TAUMAX;
    if (tmax > dn / 2)    tmax = dn / 2;
    if (tmax < tmin + 4) goto done;

    // difference function from lag 1 (the running mean below needs every lag)
    for (int tau = 1; tau <= tmax; tau++) {
        float acc = 0.0f;
        int lim = dn - tau;
        for (int j = 0; j < lim; j++) { float dif = dw[j] - dw[j + tau]; acc += dif * dif; }
        d[tau] = acc / (float)lim;               // count-normalized: no long-lag bias
    }
    float run = 0.0f;
    cm[0] = 1.0f;
    for (int tau = 1; tau <= tmax; tau++) {
        run += d[tau];
        cm[tau] = run > 0.0f ? d[tau] * (float)tau / run : 1.0f;
    }

    // first dip under the absolute threshold, walked down to its local minimum
    // (that is what keeps YIN off the octave-below harmonic); else global min
    int best = -1;
    for (int tau = tmin; tau <= tmax; tau++) {
        if (cm[tau] < PD_THRESH) {
            while (tau + 1 <= tmax && cm[tau + 1] < cm[tau]) tau++;
            best = tau;
            break;
        }
    }
    if (best < 0) {
        float bv = 1e30f;
        for (int tau = tmin; tau <= tmax; tau++) if (cm[tau] < bv) { bv = cm[tau]; best = tau; }
    }
    if (best < 0 || cm[best] > PD_APERIOD) goto done;    // noise / chord / percussion
    float conf = 1.0f - cm[best];
    if (conf < 0.0f) conf = 0.0f; else if (conf > 1.0f) conf = 1.0f;

    // refine at the NATIVE rate: the coarse lag is only quantized to PD_DEC
    // samples, so sweep +/-(PD_DEC+1) around it and parabola-fit the minimum
    int t0 = best * PD_DEC;
    int rmin = t0 - (PD_DEC + 1), rmax = t0 + (PD_DEC + 1);
    if (rmin < 2) rmin = 2;
    if (rmax > (int)nw / 2) rmax = (int)nw / 2;
    if (rmax < rmin) goto done;
    float fd[2 * (PD_DEC + 1) + 1];
    int nf = rmax - rmin + 1;
    if (nf > (int)(sizeof fd / sizeof fd[0])) { nf = (int)(sizeof fd / sizeof fd[0]); rmax = rmin + nf - 1; }
    for (int k = 0; k < nf; k++) {
        int tau = rmin + k;
        float acc = 0.0f;
        int lim = (int)nw - tau;
        for (int j = 0; j < lim; j++) { float dif = (float)buf[j] - (float)buf[j + tau]; acc += dif * dif; }
        fd[k] = acc / (float)lim;
    }
    int bk = 0;
    for (int k = 1; k < nf; k++) if (fd[k] < fd[bk]) bk = k;
    float tau_f = (float)(rmin + bk);
    if (bk > 0 && bk < nf - 1) {
        float a = fd[bk - 1], b = fd[bk], c = fd[bk + 1];
        float den = a - 2.0f * b + c;
        if (fabsf(den) > 1e-9f) {
            float adj = 0.5f * (a - c) / den;
            if (adj > -1.0f && adj < 1.0f) tau_f += adj;
        }
    }
    if (tau_f < 1.0f) goto done;

    float hz = rate / tau_f;
    if (hz < PD_FMIN * 0.9f || hz > PD_FMAX * 1.5f) goto done;
    out->hz = hz;
    out->conf = conf;
    pitch_from_hz(hz, out);
    rc = 0;

done:
    if (mine) free(mine);
    return rc;
}

static int cmp_f(const void *a, const void *b)
{
    float d = *(const float *)a - *(const float *)b;
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

int pitch_detect_buf(const int16_t *buf, uint32_t frames, float rate, pitch_result_t *out)
{
    if (!buf || !out || frames < PD_WIN / 2) return -1;

    void *sc = malloc(pitch_scratch_bytes());
    if (!sc) return -1;

    uint32_t win = frames < PD_WIN ? frames : PD_WIN;
    // skip the first 5% of a long sample: the attack transient is the one part
    // of a note that is reliably inharmonic
    uint32_t start0 = (frames > win * 4) ? frames / 20 : 0;
    uint32_t span   = (frames > start0 + win) ? frames - start0 - win : 0;

    float hzs[PD_MAXWIN], cfs[PD_MAXWIN];
    int nres = 0, tries = 0;
    for (int i = 0; i < PD_MAXWIN; i++) {
        uint32_t off = start0 + (span ? (uint32_t)((uint64_t)span * i / (PD_MAXWIN - 1)) : 0);
        pitch_result_t r;
        tries++;
        if (pitch_detect_window(buf + off, win, rate, sc, &r) == 0) {
            hzs[nres] = r.hz;
            cfs[nres] = r.conf;
            nres++;
        }
        if (!span) break;                        // short sample: one window is all there is
    }
    free(sc);
    if (!nres) return -1;

    float sorted[PD_MAXWIN];
    memcpy(sorted, hzs, (size_t)nres * sizeof(float));
    qsort(sorted, (size_t)nres, sizeof(float), cmp_f);
    float med = sorted[nres / 2];

    // average the windows that agree with the median (within 3% ~ half a
    // semitone); disagreement is what drags confidence down on chords/noise
    float acc = 0.0f, csum = 0.0f;
    int agree = 0;
    for (int i = 0; i < nres; i++) {
        if (fabsf(hzs[i] - med) < med * 0.03f) { acc += hzs[i]; csum += cfs[i]; agree++; }
    }
    if (!agree) return -1;

    out->hz   = acc / (float)agree;
    out->conf = (csum / (float)agree) * ((float)agree / (float)tries);
    pitch_from_hz(out->hz, out);
    return 0;
}
