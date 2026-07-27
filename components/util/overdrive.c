// Shared overdrive / saturation — see overdrive.h.
//
// Per sample per channel: y = tanh(g*x + b) - tanh(b), where b = 2*bias  (the
// second term cancels the DC the asymmetry bias injects), then a one-pole-ish svf
// low-pass "tone" tilt, then the output trim. Float in the middle; the machine
// format (int32<<16) is converted at the block edges only.
// NOTE b is deliberately independent of the drive gain g — see the comment in
// overdrive_block_f; scaling it by g silenced the stage past bias 40%.

#include <math.h>
#include <stddef.h>
#include "overdrive.h"
#include "fxchain.h"

#define OD_RATE 44100.0f

// Fast tanh: a Padé-style approximation, saturating to +/-1 past |x|>3. Called
// per sample per channel, so this replaces libm tanhf (~20x cheaper) to keep the
// FX chain inside the audio block budget — audibly identical for saturation.
static inline float fast_tanh(float x)
{
    if (x < -3.0f) return -1.0f;
    if (x >  3.0f) return  1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// float worker: reads/writes the interleaved float scratch, NO output clamp
// (the chain's single soft limiter handles the ceiling). buf units are samples
// (±32768), matching the >>16 machine format.
// Output trim on the WHOLE stage (Arlo 2026-07-25: "the overdrive level needs to
// come down"). tanh saturates to +/-1 by construction, so the shaper's output
// sits near FULL SCALE whatever goes in, while typical program material peaks
// well below it — switching overdrive in was a big jump in loudness rather than
// a change in character. Trimming here rather than lowering the default `level`
// brings EXISTING patches down too (a default only applies to a freshly picked
// slot), and keeps the Level knob spanning its full 0..1 over a saner range.
#define OD_OUT_TRIM 0.5f

void overdrive_block_f(overdrive_t *o, float *buf, int frames)
{
    float drive = o->drive; if (drive < 0) drive = 0; else if (drive > 1) drive = 1;
    float tone  = o->tone;  if (tone < 0)  tone = 0;  else if (tone > 1)  tone = 1;
    float bias  = o->bias;  if (bias < -1) bias = -1; else if (bias > 1)  bias = 1;
    float level = o->level; if (level < 0) level = 0; else if (level > 1) level = 1;

    float g   = 1.0f + drive * 29.0f;             // input gain 1..30
    // BIAS IS NOT SCALED BY g (fixed 2026-07-26). It used to enter as
    // tanh(g*(x+bias)), so at drive 80% (g=24) a bias of 0.8 landed at tanh(19):
    // the shaper pinned at +1 for every input above about -72% of full scale and
    // the DC subtraction below left ZERO. Measured silent output at bias >= 40%
    // for drive 20/50/80 — over half the knob's travel was a dead zone. Offsetting
    // by up to +/-2 inside the shaper instead keeps the control asymmetric (which
    // is its job — even harmonics) while always passing signal.
    const float b = bias * 2.0f;
    float dc  = fast_tanh(b);                      // DC the bias injects; subtract it
    float fc  = 300.0f * powf(30.0f, tone);        // tone: 300 Hz (dark) .. 9 kHz (bright)
    float cf  = svf_coef(fc, OD_RATE, 1.0f);

    for (int f = 0; f < frames; f++) {
        for (int ch = 0; ch < 2; ch++) {
            svf_t *flt = ch ? &o->tr : &o->tl;
            float x  = buf[f * 2 + ch] / 32768.0f;
            float sh = fast_tanh(g * x + b) - dc;
            float lp; svf_step(flt, sh, cf, 1.0f, &lp, NULL, NULL);
            buf[f * 2 + ch] = lp * level * OD_OUT_TRIM * 32767.0f;
        }
    }
}

// int32<<16 wrapper (single-FX callers): unpack -> worker -> hard-clamp pack.
void overdrive_block_i32(overdrive_t *o, int32_t *out, int frames)
{
    float buf[FX_SCRATCH_N];
    if (frames * 2 > FX_SCRATCH_N) frames = FX_SCRATCH_N / 2;
    fx_unpack_i32(out, buf, frames * 2);
    overdrive_block_f(o, buf, frames);
    for (int i = 0; i < frames * 2; i++) {
        int32_t s = (int32_t)buf[i];
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        out[i] = s << 16;
    }
}
