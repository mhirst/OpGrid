/*
 * OpGrid shared effects: tempo-synced ping-pong delay, FDN reverb, chorus,
 * and the master bus (drive, compressor, volume).
 *
 * Buffers are allocated once in og__fx_init(). Processing runs per 16-sample
 * control tick; per-sample work is a handful of multiply-adds per line.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "og_internal.h"

typedef struct { int def, max; } fxinfo_t;
static const fxinfo_t k_fx[FX_COUNT] = {
    /* DTIM  DFB      DPP      DFLT     RSIZ     RDEC     RPRE     RDMP */
    {7, OG_DELAY_TIMES - 1}, {50, 127}, {64, 127}, {64, 127}, {80, 127}, {70, 127}, {10, 127}, {60, 127},
    /* CDEP  CRAT     CWID     MTHR     MRAT     MGAIN    MDRV     MVOL */
    {60, 127}, {30, 127}, {100, 127}, {127, 127}, {32, 127}, {0, 127}, {0, 127}, {100, 127},
};

/* delay times in steps (16ths): 1/32, 1/16T, 1/16, 1/8T, 1/16D, 1/8, 1/4T,
 * 1/8D, 1/4, 1/2T, 1/4D, 1/2, 1 bar */
static const float k_dtimes[OG_DELAY_TIMES] = {
    0.5f, 0.6667f, 1.0f, 1.3333f, 1.5f, 2.0f, 2.6667f, 3.0f, 4.0f, 5.3333f, 6.0f, 8.0f, 16.0f
};

static const int k_rev_len[4] = { 1557, 1617, 1491, 1422 };
static const int k_ap_len[2] = { 225, 556 };

static inline int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

int og_fx_default(int id) { return (id >= 0 && id < FX_COUNT) ? k_fx[id].def : 0; }
int og_fx_max(int id) { return (id >= 0 && id < FX_COUNT) ? k_fx[id].max : 0; }

void og_set_fx(og_t *g, int id, int value) {
    if (!g || id < 0 || id >= FX_COUNT) return;
    g->fx.p[id] = clampi(value, 0, k_fx[id].max);
}
int og_get_fx(const og_t *g, int id) {
    return (g && id >= 0 && id < FX_COUNT) ? g->fx.p[id] : 0;
}

int og__fx_init(og_t *g) {
    og_fx_t *f = &g->fx;
    for (int i = 0; i < FX_COUNT; i++) f->p[i] = k_fx[i].def;
    f->dly_len = (int)(g->sr * OG_DELAY_MAX_S) + 4;
    f->dly[0] = calloc((size_t)f->dly_len, sizeof(float));
    f->dly[1] = calloc((size_t)f->dly_len, sizeof(float));
    f->pre_len = (int)(g->sr * 0.25f) + 4;
    f->pre = calloc((size_t)f->pre_len, sizeof(float));
    for (int i = 0; i < 4; i++) {
        f->rev_cap[i] = (int)(k_rev_len[i] * 1.6f * g->sr / 44100.0f) + 4;
        f->rev[i] = calloc((size_t)f->rev_cap[i], sizeof(float));
    }
    for (int i = 0; i < 2; i++) {
        f->ap_len[i] = (int)(k_ap_len[i] * g->sr / 44100.0f);
        f->ap[i] = calloc((size_t)f->ap_len[i] + 1, sizeof(float));
    }
    f->cho_len = 4096;
    f->cho[0] = calloc((size_t)f->cho_len, sizeof(float));
    f->cho[1] = calloc((size_t)f->cho_len, sizeof(float));
    f->dly_cur = -1.0f;
    f->comp_gain = 1.0f;
    int ok = f->dly[0] && f->dly[1] && f->pre && f->cho[0] && f->cho[1] && f->ap[0] && f->ap[1];
    for (int i = 0; i < 4; i++) ok = ok && f->rev[i];
    return ok;
}

void og__fx_free(og_t *g) {
    og_fx_t *f = &g->fx;
    free(f->dly[0]); free(f->dly[1]); free(f->pre);
    for (int i = 0; i < 4; i++) free(f->rev[i]);
    for (int i = 0; i < 2; i++) free(f->ap[i]);
    free(f->cho[0]); free(f->cho[1]);
}

static inline float read_frac(const float *buf, int len, int w, float d) {
    float r = (float)w - d;
    while (r < 0.0f) r += (float)len;
    int i0 = (int)r;
    if (i0 >= len) { i0 -= len; r -= (float)len; }   /* -tiny + len rounds to len */
    float fr = r - (float)i0;
    int i1 = i0 + 1;
    if (i1 >= len) i1 -= len;
    return buf[i0] + (buf[i1] - buf[i0]) * fr;
}

static inline float soft_clip(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float y = x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    return y > 1.0f ? 1.0f : y < -1.0f ? -1.0f : y;   /* the rational form overshoots near |x|=3 */
}

void og__fx_process(og_t *g, const og_bus_t *b, float *out_l, float *out_r, int n) {
    og_fx_t *f = &g->fx;
    const int *p = f->p;
    const float sr = g->sr;

    /* ---- delay parameters (control rate) ---- */
    double step_samples = (double)sr * 60.0 / (double)g->bpm / 4.0;
    float target = (float)(k_dtimes[p[FX_DTIM]] * step_samples);
    target = clampf(target, 16.0f, (float)(f->dly_len - 4));
    if (f->dly_cur < 0.0f) f->dly_cur = target;
    float fb = (float)p[FX_DFB] / 127.0f * 0.95f;
    float pp = (float)p[FX_DPP] / 127.0f;
    /* tone: below 64 low-pass, above 64 high-pass, 64 flat */
    float tone = ((float)p[FX_DFLT] - 64.0f) / 63.0f;
    float lp_a = 1.0f, hp_a = 0.0f;
    if (tone < 0.0f) {
        float fc = 20000.0f * powf(0.025f, -tone);        /* 20 kHz .. 500 Hz */
        lp_a = 1.0f - expf(-2.0f * (float)M_PI * fc / sr);
    } else if (tone > 0.0f) {
        float fc = 20.0f * powf(100.0f, tone);             /* 20 Hz .. 2 kHz */
        hp_a = 1.0f - expf(-2.0f * (float)M_PI * fc / sr);
    }

    /* ---- reverb parameters ---- */
    float size = 0.4f + (float)p[FX_RSIZ] / 127.0f * 1.1f;
    float t60 = 0.3f * powf(40.0f, (float)p[FX_RDEC] / 127.0f);
    float damp = (float)p[FX_RDMP] / 127.0f * 0.85f;
    int rlen[4];
    float rg[4];
    for (int i = 0; i < 4; i++) {
        rlen[i] = (int)((float)k_rev_len[i] * size * sr / 44100.0f);
        if (rlen[i] > f->rev_cap[i] - 1) rlen[i] = f->rev_cap[i] - 1;
        if (rlen[i] < 16) rlen[i] = 16;
        rg[i] = powf(10.0f, -3.0f * (float)rlen[i] / (t60 * sr));
    }
    int pre_d = (int)((float)p[FX_RPRE] / 127.0f * 0.2f * sr);
    if (pre_d > f->pre_len - 1) pre_d = f->pre_len - 1;

    /* ---- chorus parameters ---- */
    float c_depth = (float)p[FX_CDEP] / 127.0f * 0.006f * sr;
    float c_rate = 0.05f * powf(100.0f, (float)p[FX_CRAT] / 127.0f);
    float c_inc = c_rate / sr;
    float c_off = (float)p[FX_CWID] / 127.0f * 0.25f;   /* L/R phase offset */
    float c_base = 0.012f * sr;

    for (int s = 0; s < n; s++) {
        /* delay */
        f->dly_cur += (target - f->dly_cur) * 0.0005f;
        float dl = read_frac(f->dly[0], f->dly_len, f->dly_w, f->dly_cur);
        float dr = read_frac(f->dly[1], f->dly_len, f->dly_w, f->dly_cur);
        float in_l = b->dly_l[s], in_r = b->dly_r[s];
        float fl = (1.0f - pp) * dl + pp * dr;     /* ping-pong cross feed */
        float fr = (1.0f - pp) * dr + pp * dl;
        float wl = in_l * (1.0f - pp) + (in_l + in_r) * 0.5f * pp + fl * fb;  /* pp: mono in, left first */
        float wr = in_r * (1.0f - pp) + fr * fb;
        /* tone in the feedback path */
        f->dly_lp[0] += (wl - f->dly_lp[0]) * lp_a; wl = f->dly_lp[0];
        f->dly_lp[1] += (wr - f->dly_lp[1]) * lp_a; wr = f->dly_lp[1];
        f->dly_hp[0] += (wl - f->dly_hp[0]) * hp_a; wl -= f->dly_hp[0];
        f->dly_hp[1] += (wr - f->dly_hp[1]) * hp_a; wr -= f->dly_hp[1];
        f->dly[0][f->dly_w] = soft_clip(wl);
        f->dly[1][f->dly_w] = soft_clip(wr);
        if (++f->dly_w >= f->dly_len) f->dly_w = 0;

        /* reverb: predelay -> 2 allpass diffusers -> 4-line FDN */
        float rin = (b->rev_l[s] + b->rev_r[s]) * 0.5f;
        f->pre[f->pre_w] = rin;
        int pr = f->pre_w - pre_d; if (pr < 0) pr += f->pre_len;
        float x = f->pre[pr];
        if (++f->pre_w >= f->pre_len) f->pre_w = 0;
        for (int a = 0; a < 2; a++) {
            float *ap = f->ap[a];
            float bufv = ap[f->ap_w[a]];
            float y = -0.5f * x + bufv;
            ap[f->ap_w[a]] = x + 0.5f * y;
            if (++f->ap_w[a] >= f->ap_len[a]) f->ap_w[a] = 0;
            x = y;
        }
        float o[4];
        for (int i = 0; i < 4; i++) {
            int r = f->rev_w[i] - rlen[i]; if (r < 0) r += f->rev_cap[i];
            float v = f->rev[i][r];
            f->rev_lp[i] += (v - f->rev_lp[i]) * (1.0f - damp);
            o[i] = f->rev_lp[i] * rg[i];
        }
        float sum = (o[0] + o[1] + o[2] + o[3]) * 0.5f;
        for (int i = 0; i < 4; i++) {
            f->rev[i][f->rev_w[i]] = o[i] - sum + x * 0.35f;
            if (++f->rev_w[i] >= f->rev_cap[i]) f->rev_w[i] = 0;
        }
        float rv_l = (o[0] + o[2]) * 0.7f, rv_r = (o[1] + o[3]) * 0.7f;

        /* chorus */
        f->cho[0][f->cho_w] = b->cho_l[s];
        f->cho[1][f->cho_w] = b->cho_r[s];
        f->cho_ph += c_inc; if (f->cho_ph >= 1.0f) f->cho_ph -= 1.0f;
        float ph_r = f->cho_ph + c_off; if (ph_r >= 1.0f) ph_r -= 1.0f;
        float ml = g->sine[(int)(f->cho_ph * SINE_SIZE)];
        float mr = g->sine[(int)(ph_r * SINE_SIZE)];
        float cl = read_frac(f->cho[0], f->cho_len, f->cho_w, c_base + ml * c_depth);
        float cr = read_frac(f->cho[1], f->cho_len, f->cho_w, c_base + mr * c_depth);
        if (++f->cho_w >= f->cho_len) f->cho_w = 0;

        out_l[s] = b->dry_l[s] + dl + rv_l + cl;
        out_r[s] = b->dry_r[s] + dr + rv_r + cr;
    }

    /* denormal guard on recursive state */
    for (int i = 0; i < 4; i++) if (fabsf(f->rev_lp[i]) < 1e-20f) f->rev_lp[i] = 0.0f;
    for (int i = 0; i < 2; i++) {
        if (fabsf(f->dly_lp[i]) < 1e-20f) f->dly_lp[i] = 0.0f;
        if (fabsf(f->dly_hp[i]) < 1e-20f) f->dly_hp[i] = 0.0f;
    }

    /* ---- master: drive -> compressor -> volume ---- */
    float drive = (float)p[FX_MDRV] / 127.0f;
    float dgain = 1.0f + drive * drive * 8.0f;
    float dcomp = drive > 0.0f ? 1.0f / soft_clip(dgain) : 1.0f;
    float thr_db = -40.0f + (float)p[FX_MTHR] / 127.0f * 40.0f;
    float ratio = 1.0f + (float)p[FX_MRAT] / 127.0f * 19.0f;
    float makeup = powf(10.0f, ((float)p[FX_MGAIN] / 127.0f * 18.0f) / 20.0f);
    float vol = (float)p[FX_MVOL] / 127.0f;
    vol = vol * vol;

    float peak = 0.0f;
    for (int s = 0; s < n; s++) {
        if (drive > 0.0f) {
            out_l[s] = soft_clip(out_l[s] * dgain) * dcomp;
            out_r[s] = soft_clip(out_r[s] * dgain) * dcomp;
        }
        float a = fabsf(out_l[s]) > fabsf(out_r[s]) ? fabsf(out_l[s]) : fabsf(out_r[s]);
        if (a > peak) peak = a;
    }
    /* envelope: fast attack, ~150 ms release, at control rate */
    float att = 1.0f - expf(-(float)n / (0.003f * sr));
    float rel = 1.0f - expf(-(float)n / (0.150f * sr));
    f->comp_env += (peak - f->comp_env) * (peak > f->comp_env ? att : rel);
    float target_gain = 1.0f;
    if (p[FX_MTHR] < 127 && f->comp_env > 1e-6f) {
        float env_db = 20.0f * log10f(f->comp_env);
        if (env_db > thr_db) {
            float red = (env_db - thr_db) * (1.0f - 1.0f / ratio);
            target_gain = powf(10.0f, -red / 20.0f);
        }
    }
    float g0 = f->comp_gain, gstep = (target_gain - g0) / (float)n;
    for (int s = 0; s < n; s++) {
        g0 += gstep;
        float m = g0 * makeup * vol;
        out_l[s] *= m;
        out_r[s] *= m;
    }
    f->comp_gain = target_gain;
    f->last_gr = target_gain;
}

float og_debug_comp_gain(const og_t *g) { return g ? g->fx.last_gr : 1.0f; }
