/*
 * OpGrid machines: the oscillator stage of a voice. Each machine fills
 * osc[0..n) for one control tick; the shared filter / drive / amp / sends
 * stage in og_engine.c follows. Machine-specific parameters are ids 0..11;
 * the rest are shared by every machine.
 *
 *   FM Poly  4-operator FM, 8 algorithms                      (og_engine.c)
 *   FM Perc  2-op body with pitch sweep, noise and click       (drums)
 *   Morph    2 morphing wavetable oscillators, phase distortion, ring, sub
 *   Stack    1-8 detuned copies of one band-limited wave, plus a sub
 */
#include <math.h>
#include <string.h>

#include "og_internal.h"

static inline int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
#define TWO32 4294967296.0f

/* ---- parameter tables for ids 0..11 (FM Poly's live in og_engine.c) ---- */

typedef struct { int def, max; } mp_t;
static const mp_t k_mp[OG_MACHINES][OG_MACH_PARAMS] = {
    /* FM Poly: filled from og_param_* at lookup */
    { {0, 0} },
    /* FM Perc: SWP, SWPT, RAT, FM, FMD, NOIS, NDEC, NCOL | FDBK, CLIK, PTCH, - */
    { {40, 127}, {55, 127}, {3, 21}, {30, 127}, {50, 127}, {0, 127}, {55, 127}, {64, 127},
      {0, 127}, {0, 127}, {60, 127}, {0, 0} },
    /* Morph: WAVA, WAVB, MIX, PD, TUNB, RING, NOIS, DRFT | SUBL, SUBO, -, - */
    { {0, 127}, {36, 127}, {0, 127}, {0, 127}, {64, 127}, {0, 127}, {0, 127}, {0, 127},
      {0, 127}, {0, 1}, {0, 0}, {0, 0} },
    /* Stack: WAVE, VOIC, DTUN, ANIM, SUBL, SUBO, NOIS, MIX | -, -, -, - */
    { {0, 3}, {5, 7}, {40, 127}, {30, 127}, {0, 127}, {0, 1}, {0, 127}, {100, 127},
      {0, 0}, {0, 0}, {0, 0}, {0, 0} },
};

int og_machine_default(int m, int pid) {
    if (m <= 0 || m >= OG_MACHINES || pid < 0 || pid >= OG_MACH_PARAMS) return og_param_default(pid);
    return k_mp[m][pid].def;
}
int og_machine_max(int m, int pid) {
    if (m <= 0 || m >= OG_MACHINES || pid < 0 || pid >= OG_MACH_PARAMS) return og_param_max(pid);
    return k_mp[m][pid].max;
}
int og__pmax(const og_t *g, int track, int pid) {
    int m = (track >= 0 && track < OG_TRACKS) ? g->params[track][P_MACH] : 0;
    return pid < OG_MACH_PARAMS ? og_machine_max(m, pid) : og_param_max(pid);
}

void og_set_machine(og_t *g, int track, int m) {
    if (!g || track < 0 || track >= OG_TRACKS) return;
    m = clampi(m, 0, OG_MACHINES - 1);
    og_set_param(g, track, P_MACH, m);
    for (int pid = 0; pid < OG_MACH_PARAMS; pid++) og_set_param(g, track, pid, og_machine_default(m, pid));
    if (m == OG_MACH_PERC) {          /* drums want a short, percussive amp */
        og_set_param(g, track, P_AATK, 0);
        og_set_param(g, track, P_ADEC, 78);
        og_set_param(g, track, P_ASUS, 0);
        og_set_param(g, track, P_AREL, 45);
    }
}

/* ---- wavetables for Morph (and Stack's triangle/sine) ----------------- */

/* up to 32 harmonics: cheap to build, and only 6 band-limit levels needed */
static float harmonic_amp(int wave, int h) {
    switch (wave) {
    case 0: return h == 1 ? 1.0f : 0.0f;                                   /* sine */
    case 1: return (h & 1) ? ((((h - 1) / 2) & 1) ? -1.0f : 1.0f) / (float)(h * h) : 0.0f; /* triangle */
    case 2: return 1.0f / (float)h;                                        /* saw */
    case 3: return (h & 1) ? 1.0f / (float)h : 0.0f;                       /* square */
    case 4: return sinf((float)M_PI * (float)h * 0.2f) / (float)h;         /* narrow pulse */
    case 5: {                                                              /* organ drawbars */
        static const float bars[9] = {0, 1.0f, 0.8f, 0.6f, 0.7f, 0, 0.5f, 0, 0.45f};
        return h <= 8 ? bars[h] : 0.0f;
    }
    case 6: {                                                              /* vocal formant */
        float d = ((float)h - 7.0f) / 2.5f;
        return expf(-d * d) + (h == 1 ? 0.4f : 0.0f);
    }
    default: {                                                             /* glass: sparse odd */
        static const float b[12] = {0, 1.0f, 0, 0.5f, 0, 0, 0, 0.35f, 0, 0, 0, 0.25f};
        return h < 12 ? b[h] : 0.0f;
    }
    }
}

void og__wavetables_init(og_t *g) {
    for (int w = 0; w < OG_WAVES_N; w++)
        for (int lv = 0; lv < OG_WT_LEVELS; lv++) {
            int hmax = 32 >> lv;                               /* 32, 16, 8, 4, 2, 1 */
            float *t = g->wt[w][lv];
            memset(t, 0, sizeof(float) * (OG_WT_SIZE + 1));
            for (int h = 1; h <= hmax; h++) {
                float a = harmonic_amp(w, h);
                if (a == 0.0f) continue;
                for (int i = 0; i < OG_WT_SIZE; i++)
                    t[i] += a * g->sine[(i * h * (SINE_SIZE / OG_WT_SIZE)) & (SINE_SIZE - 1)];
            }
            float peak = 0.0f;
            for (int i = 0; i < OG_WT_SIZE; i++) if (fabsf(t[i]) > peak) peak = fabsf(t[i]);
            if (peak > 0.0f) for (int i = 0; i < OG_WT_SIZE; i++) t[i] /= peak;
            t[OG_WT_SIZE] = t[0];
        }
}

/* band-limit level for a frequency: most harmonics that stay under Nyquist */
static inline int wt_level(const og_t *g, float hz) {
    float hmax = g->sr * 0.5f / (hz > 1.0f ? hz : 1.0f);
    int lv = 0;
    while (lv < OG_WT_LEVELS - 1 && (float)(32 >> lv) > hmax) lv++;
    return lv;
}

static inline float wt_read(const float *t, uint32_t ph) {
    uint32_t i = ph >> (32 - OG_WT_BITS);
    float fr = (float)(ph & ((1u << (32 - OG_WT_BITS)) - 1)) * (1.0f / (float)(1u << (32 - OG_WT_BITS)));
    return t[i] + (t[i + 1] - t[i]) * fr;
}

/* morph position 0..127 across waves 0..7 */
static inline float wt_morph(const og_t *g, int lv, float pos, uint32_t ph) {
    float x = pos * (float)(OG_WAVES_N - 1);
    int a = (int)x;
    if (a >= OG_WAVES_N - 1) return wt_read(g->wt[OG_WAVES_N - 1][lv], ph);
    float f = x - (float)a;
    float va = wt_read(g->wt[a][lv], ph);
    return f > 0.0f ? va + (wt_read(g->wt[a + 1][lv], ph) - va) * f : va;
}

static inline float noise(voice_t *v) {
    uint32_t x = v->nrng ? v->nrng : 0x12345u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    v->nrng = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* PolyBLEP residual for band-limited saw/square */
static inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

/* ---- FM Perc ---------------------------------------------------------- */

void og__osc_perc(og_t *g, voice_t *v, const int *p, float base, float *osc, int n) {
    const float velf = (float)v->vel / 127.0f;
    float tick = (float)n;
    /* envelopes at control rate */
    /* same 0.5 ms .. 10 s range as every other envelope */
    float swp_c = g->dec_coef[clampi(p[1], 0, 127)];
    float fm_c = g->dec_coef[clampi(p[4], 0, 127)];
    float nz_c = g->dec_coef[clampi(p[6], 0, 127)];
    float sweep = (float)p[0] / 127.0f * 48.0f * v->perc_swp;      /* semitones above base */
    v->perc_swp *= swp_c;
    v->perc_fm *= fm_c;
    v->perc_nz *= nz_c;
    /* flush decayed envelopes: denormal floats make every multiply slow */
    if (v->perc_swp < 1e-6f) v->perc_swp = 0.0f;
    if (v->perc_fm < 1e-6f) v->perc_fm = 0.0f;
    if (v->perc_nz < 1e-6f) v->perc_nz = 0.0f;
    if (fabsf(v->perc_nlp) < 1e-20f) v->perc_nlp = 0.0f;
    float f1 = clampf(base * exp2f(sweep / 12.0f), 0.0f, g->sr * 0.45f);
    float f2 = clampf(f1 * og__ratio(p[2]), 0.0f, g->sr * 0.45f);
    uint32_t inc1 = (uint32_t)(f1 * g->inv_sr * TWO32), inc2 = (uint32_t)(f2 * g->inv_sr * TWO32);
    float velm = 1.0f - ((float)p[13] / 127.0f) * (1.0f - velf);
    float fm = (float)p[3] / 127.0f; fm = fm * fm * 2.0f * v->perc_fm * velm * TWO32;
    float fb = (float)p[8] / 127.0f; fb = fb * fb * 0.2f * TWO32;
    float nl = (float)p[5] / 127.0f; nl *= nl;
    float col = ((float)p[7] - 64.0f) / 63.0f;                      /* <0 darker, >0 brighter */
    float nlp_a = col < 0.0f ? 0.05f + 0.95f * (1.0f + col) : 1.0f;
    float click = (float)p[9] / 127.0f;
    const float *t = g->sine;
    uint32_t ph1 = v->ph[1], ph2 = v->ph[2];
    float fb1 = v->fb1;
    for (int s = 0; s < n; s++) {
        float m = t[ph2 >> (32 - SINE_BITS)];
        uint32_t pm = (uint32_t)(int64_t)(m * fm + fb1 * fb);
        float body = t[(ph1 + pm) >> (32 - SINE_BITS)];
        fb1 = body;
        ph1 += inc1; ph2 += inc2;
        float nz = noise(v);
        v->perc_nlp += (nz - v->perc_nlp) * nlp_a;
        float nzc = col > 0.0f ? nz - v->perc_nlp * (1.0f - col) : v->perc_nlp;
        float x = body + nzc * nl * v->perc_nz * 1.5f;
        if (v->perc_click > 0.0f) { x += nz * click * v->perc_click; v->perc_click -= 1.0f / 90.0f; }
        osc[s] = x;
    }
    (void)tick;
    v->ph[1] = ph1; v->ph[2] = ph2; v->fb1 = fb1;
}

/* ---- Morph ------------------------------------------------------------ */

void og__osc_morph(og_t *g, voice_t *v, const int *p, float base, float *osc, int n) {
    /* slow random drift, at control rate */
    float drift = (float)p[7] / 127.0f;
    if (drift > 0.0f) {
        v->drift[0] += (noise(v) * 0.5f - v->drift[0]) * 0.002f;
        v->drift[1] += (noise(v) * 0.5f - v->drift[1]) * 0.002f;
    }
    float fa = base * exp2f(v->drift[0] * drift * 0.3f / 12.0f);
    float semis_b = ((float)p[4] - 64.0f) / 63.0f * 24.0f;
    float fb = base * exp2f((semis_b + v->drift[1] * drift * 0.3f) / 12.0f);
    float fs = base * (p[9] ? 0.25f : 0.5f);
    fa = clampf(fa, 1.0f, g->sr * 0.45f); fb = clampf(fb, 1.0f, g->sr * 0.45f);
    uint32_t ia = (uint32_t)(fa * g->inv_sr * TWO32), ib = (uint32_t)(fb * g->inv_sr * TWO32);
    uint32_t is = (uint32_t)(fs * g->inv_sr * TWO32);
    int la = wt_level(g, fa), lb = wt_level(g, fb);
    float pa = (float)p[0] / 127.0f, pb = (float)p[1] / 127.0f;
    float mix = (float)p[2] / 127.0f;
    float pd = (float)p[3] / 127.0f;
    float d = 0.5f - pd * 0.45f;                       /* phase-distortion knee */
    float ring = (float)p[5] / 127.0f;
    float nl = (float)p[6] / 127.0f; nl *= nl;
    float sub = (float)p[8] / 127.0f;
    uint32_t pha = v->ph[1], phb = v->ph[2], phs = v->ph[3];
    for (int s = 0; s < n; s++) {
        uint32_t qa = pha;
        if (pd > 0.0f) {                               /* warp the phase of oscillator A */
            float x = (float)pha * (1.0f / TWO32);
            x = x < d ? x * 0.5f / d : 0.5f + (x - d) * 0.5f / (1.0f - d);
            qa = (uint32_t)(int64_t)(x * TWO32);
        }
        float a = wt_morph(g, la, pa, qa);
        float b = wt_morph(g, lb, pb, phb);
        float x = a * (1.0f - mix) + b * mix;
        if (ring > 0.0f) x = x * (1.0f - ring) + a * b * ring * 1.4f;
        if (sub > 0.0f) x += g->sine[phs >> (32 - SINE_BITS)] * sub * 0.7f;
        if (nl > 0.0f) x += noise(v) * nl;
        osc[s] = x * 0.8f;
        pha += ia; phb += ib; phs += is;
    }
    v->ph[1] = pha; v->ph[2] = phb; v->ph[3] = phs;
}

/* ---- Stack ------------------------------------------------------------ */

void og__osc_stack(og_t *g, voice_t *v, const int *p, float base, float *osc, int n) {
    int wave = p[0];
    int nv = p[1] + 1;                                   /* 1..8 copies */
    float spread = (float)p[2] / 127.0f * 0.6f;          /* up to +-60 cents */
    float anim = (float)p[3] / 127.0f;
    float sub = (float)p[4] / 127.0f;
    float nl = (float)p[6] / 127.0f; nl *= nl;
    float side = (float)p[7] / 127.0f;                   /* level of the detuned copies vs the centre */
    float inc[OG_STACK_MAX], dt[OG_STACK_MAX], gain[OG_STACK_MAX];
    float norm = 0.0f;
    for (int k = 0; k < nv; k++) {
        /* symmetric detune, centre copy (or two) at 0 */
        float pos = nv > 1 ? ((float)k / (float)(nv - 1)) * 2.0f - 1.0f : 0.0f;
        if (anim > 0.0f) v->drift[k] += (noise(v) * 0.5f - v->drift[k]) * 0.004f * anim;
        float cents = pos * spread * 100.0f + v->drift[k] * anim * 6.0f;
        float f = clampf(base * exp2f(cents / 1200.0f), 1.0f, g->sr * 0.45f);
        inc[k] = f * g->inv_sr;
        dt[k] = inc[k];
        gain[k] = fabsf(pos) < 0.34f ? 1.0f : side;
        norm += gain[k] * gain[k];
    }
    /* detuned copies add like uncorrelated signals: keep the power constant */
    norm = norm > 0.0f ? 1.2f / sqrtf(norm) : 1.0f;
    int lv = wt_level(g, base);
    uint32_t is = (uint32_t)(clampf(base * (p[5] ? 0.25f : 0.5f), 1.0f, g->sr * 0.45f) * g->inv_sr * TWO32);
    for (int s = 0; s < n; s++) {
        float x = 0.0f;
        for (int k = 0; k < nv; k++) {
            float t = v->stk[k];
            float y;
            switch (wave) {
            case 0: y = 2.0f * t - 1.0f - blep(t, dt[k]); break;             /* saw */
            case 1: {                                                         /* square */
                float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
                y = (t < 0.5f ? 1.0f : -1.0f) + blep(t, dt[k]) - blep(t2, dt[k]);
                break;
            }
            case 2: y = wt_read(g->wt[1][lv], (uint32_t)(int64_t)(t * TWO32)); break;  /* triangle */
            default: y = g->sine[(int)(t * SINE_SIZE) & (SINE_SIZE - 1)]; break;
            }
            x += y * gain[k];
            t += inc[k];
            if (t >= 1.0f) t -= 1.0f;
            v->stk[k] = t;
        }
        x *= norm;
        if (sub > 0.0f) { x += g->sine[v->ph[3] >> (32 - SINE_BITS)] * sub * 0.7f; v->ph[3] += is; }
        if (nl > 0.0f) x += noise(v) * nl;
        osc[s] = x;
    }
}
