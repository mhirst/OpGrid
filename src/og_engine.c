/*
 * OpGrid engine — 4-operator FM voices, filter, envelopes, voice pool.
 *
 * Realtime rules: nothing below og_create()/og_destroy() allocates, locks,
 * logs or touches files. Expensive maths (exp/pow/sin) is either tabled at
 * create time or run once per 16-sample control tick, never per sample.
 */
#include "og_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FADE_LEN    64          /* ~1.5 ms steal fade */
#define TWO32       4294967296.0f

/* ---- static tables -------------------------------------------------- */

static const float k_ratios[] = {
    0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f,
    6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f
};
#define N_RATIOS ((int)(sizeof(k_ratios) / sizeof(k_ratios[0])))

/* route[algo][src][dst]: op src (1..4) modulates op dst. carriers[algo] is
 * a bitmask of ops summed to the output (bit k = op k). Modulation only
 * flows from higher to lower op numbers, so ops run 4,3,2,1. */
static const uint8_t k_route[OG_ALGOS][5][5] = {
    /* 0: 4>3>2>1 */
    { {0}, {0}, {0,1,0,0,0}, {0,0,1,0,0}, {0,0,0,1,0} },
    /* 1: (3+4)>2>1 */
    { {0}, {0}, {0,1,0,0,0}, {0,0,1,0,0}, {0,0,1,0,0} },
    /* 2: (2+3+4)>1 */
    { {0}, {0}, {0,1,0,0,0}, {0,1,0,0,0}, {0,1,0,0,0} },
    /* 3: 4>3>1, 2>1 */
    { {0}, {0}, {0,1,0,0,0}, {0,1,0,0,0}, {0,0,0,1,0} },
    /* 4: 2>1, 4>3, out 1+3 */
    { {0}, {0}, {0,1,0,0,0}, {0}, {0,0,0,1,0} },
    /* 5: 4>(1,2,3), out 1+2+3 */
    { {0}, {0}, {0}, {0}, {0,1,1,1,0} },
    /* 6: 4>3, out 1+2+3 */
    { {0}, {0}, {0}, {0}, {0,0,0,1,0} },
    /* 7: additive 1+2+3+4 */
    { {0}, {0}, {0}, {0}, {0} },
};
static const uint8_t k_carriers[OG_ALGOS] = {
    0x02, 0x02, 0x02, 0x02, 0x0A, 0x0E, 0x0E, 0x1E
};

typedef struct { int def, max; } pinfo_t;
static const pinfo_t k_pinfo[P_COUNT_USED] = {
    /* FM */   {0, OG_ALGOS - 1}, {3, N_RATIOS - 1}, {3, N_RATIOS - 1},
               {5, N_RATIOS - 1}, {3, N_RATIOS - 1}, {60, 127}, {30, 127}, {0, 127},
    /* FM2 */  {0, 127}, {70, 127}, {60, 127}, {60, 127}, {0, 127}, {64, 127},
               {24, 48}, {0, 127},
    /* FILT */ {110, 127}, {20, 127}, {0, OG_FILTER_TYPES - 1}, {64, 127},
               {0, 127}, {60, 127}, {127, 127}, {50, 127},
    /* AMP */  {0, 127}, {70, 127}, {90, 127}, {50, 127}, {0, 127}, {64, 127},
               {100, 127}, {64, 127},
#define LFO_INFO {0, OG_DESTS - 1}, {0, OG_WAVES - 1}, {1, 1}, {64, 127}, \
                 {64, 127}, {0, 1}, {0, 127}, {0, 127}
    /* LFO1-3 */ LFO_INFO, LFO_INFO, LFO_INFO,
#undef LFO_INFO
    /* SEND */ {0, 127}, {0, 127}, {0, 127}, {0, 16}, {0, OG_MOUTS - 1}, {0, 0}, {0, 0}, {0, 0},
};

/* LFO sync rates: steps per cycle */
static const float k_lfo_sync[OG_LFO_SYNC_RATES] = {
    128, 96, 64, 48, 32, 24, 16, 12, 8, 6, 4, 3, 2, 1.5f, 1, 0.5f, 0.25f
};

/* ---- helpers -------------------------------------------------------- */

static inline int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static inline float sine_lookup(const float *t, uint32_t ph) {
    uint32_t i = ph >> (32 - SINE_BITS);
    float frac = (float)(ph & ((1u << (32 - SINE_BITS)) - 1)) *
                 (1.0f / (float)(1u << (32 - SINE_BITS)));
    return t[i] + (t[i + 1] - t[i]) * frac;
}

/* phase offset in cycles -> wrapped uint32 */
static inline uint32_t cyc(float c) { return (uint32_t)(int64_t)(c * TWO32); }

static inline float soft_clip(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float y = x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    return y > 1.0f ? 1.0f : y < -1.0f ? -1.0f : y;   /* the rational form overshoots near |x|=3 */
}

static float env_seconds(int v) { return 0.0005f * powf(20000.0f, (float)v / 127.0f); }

int og_param_default(int pid) {
    return (pid >= 0 && pid < P_COUNT_USED) ? k_pinfo[pid].def : 0;
}

int og_param_max(int pid) {
    return (pid >= 0 && pid < P_COUNT_USED) ? k_pinfo[pid].max : 127;
}

/* ---- lifecycle ------------------------------------------------------ */

og_t *og_create(float sample_rate) {
    og_t *g = (og_t *)calloc(1, sizeof(og_t));
    if (!g) return NULL;
    g->sr = sample_rate > 0 ? sample_rate : OG_SAMPLE_RATE;
    g->inv_sr = 1.0f / g->sr;
    g->master = 0.8f;

    for (int i = 0; i <= SINE_SIZE; i++)
        g->sine[i] = sinf(2.0f * (float)M_PI * (float)i / (float)SINE_SIZE);

    for (int v = 0; v < 128; v++) {
        float ticks = env_seconds(v) * g->sr / (float)OG_CTRL_RATE;
        if (ticks < 1.0f) ticks = 1.0f;
        g->att_inc[v] = 1.0f / ticks;
        /* reach -80 dB over the time */
        g->dec_coef[v] = expf(logf(0.0001f) / ticks);
        float a = (float)v / 127.0f * (float)M_PI * 0.5f;
        g->pan_l[v] = cosf(a);
        g->pan_r[v] = sinf(a);
    }

    for (int t = 0; t < OG_TRACKS; t++) {
        for (int p = 0; p < P_COUNT_USED; p++) g->params[t][p] = k_pinfo[p].def;
        g->last_pitch[t] = -1.0f;
    }
    og__seq_init(g);
    og__rec_init(g);
    og__song_init(g);
    if (!og__fx_init(g)) { og_destroy(g); return NULL; }
    return g;
}

void og_destroy(og_t *g) {
    if (!g) return;
    og__fx_free(g);
    free(g);
}

void og_set_solo(og_t *g, int track, int on) {
    if (g && track >= 0 && track < OG_TRACKS) g->solo[track] = on ? 1 : 0;
}
int og_get_solo(const og_t *g, int track) {
    return (g && track >= 0 && track < OG_TRACKS) ? g->solo[track] : 0;
}
int og__track_audible(const og_t *g, int track) {
    if (g->ts[track].mute) return 0;
    for (int t = 0; t < OG_TRACKS; t++) if (g->solo[t]) return g->solo[track];
    return 1;
}
void og_set_midi_out(og_t *g, og_midi_out_fn fn, void *ctx) {
    if (!g) return;
    g->midi_out = fn;
    g->midi_ctx = ctx;
}

static void midi_send(og_t *g, uint8_t st, uint8_t d1, uint8_t d2) {
    if (g->midi_out) g->midi_out(g->midi_ctx, st, d1, d2);
}

/* ---- params --------------------------------------------------------- */

void og_set_param(og_t *g, int track, int pid, int value) {
    if (!g || track < 0 || track >= OG_TRACKS || pid < 0 || pid >= P_COUNT_USED) return;
    value = clampi(value, 0, og_param_max(pid));
    g->params[track][pid] = value;
    /* sounding voices follow base edits, except where a step lock holds */
    for (int i = 0; i < OG_VOICES; i++) {
        voice_t *v = &g->v[i];
        if (v->active && v->track == track && !(v->lockmask & (1ull << pid)))
            v->p[pid] = value;
        if (v->pending && v->pend_track == track && !(v->pend_lockmask & (1ull << pid)))
            v->pend_p[pid] = value;
    }
}

int og_get_param(const og_t *g, int track, int pid) {
    if (!g || track < 0 || track >= OG_TRACKS || pid < 0 || pid >= P_COUNT_USED) return 0;
    return g->params[track][pid];
}

void og_set_master(og_t *g, int value) {
    if (!g) return;
    float m = (float)clampi(value, 0, 127) / 127.0f;
    g->master = m * m;
}

int og_debug_voice_param(const og_t *g, int track, int pid) {
    const voice_t *best = NULL;
    for (int i = 0; i < OG_VOICES; i++) {
        const voice_t *v = &g->v[i];
        if (v->active && !v->fade && v->track == track && (!best || v->age > best->age)) best = v;
    }
    return (best && pid >= 0 && pid < P_COUNT_USED) ? best->p[pid] : -1;
}

int og_active_voices(const og_t *g) {
    int n = 0;
    for (int i = 0; i < OG_VOICES; i++) n += g->v[i].active;
    return n;
}

/* ---- voices --------------------------------------------------------- */

static void voice_start(og_t *g, voice_t *v, int track, int note, int vel,
                        const int *params, uint64_t lockmask, int slide) {
    memcpy(v->p, params, sizeof(v->p));
    v->lockmask = lockmask;
    v->slide = slide;
    const int *p = v->p;
    int fresh = !v->active || v->track != track;

    v->active = 1;
    v->track = track;
    v->note = note;
    v->vel = vel;
    v->gate = 1;
    v->age = ++g->clock;
    v->fade = 0;
    v->pending = 0;

    v->target = (float)note + (float)(p[P_TUNE] - 24);
    float last = g->last_pitch[track];
    v->pitch = ((p[P_GLID] > 0 || slide) && last >= 0.0f) ? last : v->target;
    g->last_pitch[track] = v->target;

    v->amp.stage = ENV_ATT;
    v->flt.stage = ENV_ATT;
    if (fresh) {
        v->amp.level = 0.0f;
        v->flt.level = 0.0f;
        v->amp_out = 0.0f;
        memset(v->ph, 0, sizeof(v->ph));
        v->fb1 = v->fb2 = 0.0f;
        v->ic1[0] = v->ic1[1] = v->ic2[0] = v->ic2[1] = 0.0f;
    }
    for (int k = 2; k <= 4; k++) v->mod_env[k] = 1.0f;
    v->lfo_age = 0.0f;
    for (int i = 0; i < OG_LFOS; i++) {
        v->lfo_ph[i] = (float)p[P_LFO(i, LFO_PHAS)] / 128.0f;
        g->rng ^= g->rng << 13; g->rng ^= g->rng >> 17; g->rng ^= g->rng << 5;
        v->lfo_rnd[i] = (float)(g->rng & 0xFFFF) / 32767.5f - 1.0f;
    }
}

static voice_t *find_voice(og_t *g, int track, int note) {
    /* same note on same track: retrigger it */
    for (int i = 0; i < OG_VOICES; i++) {
        voice_t *v = &g->v[i];
        if (v->active && !v->fade && v->track == track && v->note == note) return v;
    }
    for (int i = 0; i < OG_VOICES; i++)
        if (!g->v[i].active) return &g->v[i];
    return NULL;
}

static voice_t *steal_voice(og_t *g) {
    voice_t *best = NULL;
    /* oldest released voice first */
    for (int i = 0; i < OG_VOICES; i++) {
        voice_t *v = &g->v[i];
        if (v->fade) continue;
        if (!v->gate && (!best || v->age < best->age)) best = v;
    }
    if (best) return best;
    for (int i = 0; i < OG_VOICES; i++) {
        voice_t *v = &g->v[i];
        if (v->fade) continue;
        if (!best || v->age < best->age) best = v;
    }
    if (best) return best;
    /* every voice is already fading: replace the newest pending request */
    for (int i = 0; i < OG_VOICES; i++)
        if (!best || g->v[i].age > best->age) best = &g->v[i];
    return best;
}

static void note_on_impl(og_t *g, int track, int note, int velocity,
                         const int *params, uint64_t lockmask, int slide) {
    int ch = g->params[track][P_MCH], mode = g->params[track][P_MOUT];
    if (ch > 0 && mode != OG_MOUT_INT) {
        midi_send(g, (uint8_t)(0x90 | (ch - 1)), (uint8_t)note, (uint8_t)velocity);
        if (mode == OG_MOUT_EXT) return;          /* external gear only */
    }
    voice_t *v = find_voice(g, track, note);
    if (v) { voice_start(g, v, track, note, velocity, params, lockmask, slide); return; }

    v = steal_voice(g);
    if (!v) return;
    if (!v->fade) v->fade = FADE_LEN;
    v->pending = 1;
    v->pend_track = track;
    v->pend_note = note;
    v->pend_vel = velocity;
    v->pend_gate = 1;
    v->pend_slide = slide;
    memcpy(v->pend_p, params, sizeof(v->pend_p));
    v->pend_lockmask = lockmask;
    v->age = ++g->clock;
}

void og_note_on(og_t *g, int track, int note, int velocity) {
    if (!g || track < 0 || track >= OG_TRACKS) return;
    note = clampi(note, 0, 127);
    if (velocity <= 0) { og_note_off(g, track, note); return; }
    velocity = clampi(velocity, 1, 127);
    note_on_impl(g, track, note, velocity, g->params[track], 0, 0);
}

/* Build base params + this step's locks. */
static uint64_t locked_params(const og_t *g, int track, const og_lock_t *locks,
                              int step, int *out) {
    memcpy(out, g->params[track], sizeof(int) * P_COUNT_USED);
    uint64_t mask = 0;
    if (!locks) return 0;
    for (int i = 0; i < OG_LOCKS; i++) {
        const og_lock_t *l = &locks[i];
        if (l->used && l->step == step && l->pid < P_COUNT_USED) {
            out[l->pid] = clampi(l->val, 0, og_param_max(l->pid));
            mask |= 1ull << l->pid;
        }
    }
    return mask;
}

void og__note_on_locked(og_t *g, int track, int note, int vel,
                        const og_lock_t *locks, int step, int slide) {
    int p[P_COUNT_USED];
    uint64_t mask = locked_params(g, track, locks, step, p);
    note_on_impl(g, track, clampi(note, 0, 127), clampi(vel, 1, 127), p, mask, slide);
}

void og__apply_locks_to_track(og_t *g, int track, const og_lock_t *locks, int step) {
    int p[P_COUNT_USED];
    uint64_t mask = locked_params(g, track, locks, step, p);
    if (!mask) return;
    for (int i = 0; i < OG_VOICES; i++) {
        voice_t *v = &g->v[i];
        if (!v->active || v->fade || v->track != track) continue;
        for (int k = 0; k < P_COUNT_USED; k++)
            if (mask & (1ull << k)) v->p[k] = p[k];
        v->lockmask |= mask;
    }
}

void og_note_off(og_t *g, int track, int note) {
    if (!g) return;
    if (track >= 0 && track < OG_TRACKS) {
        int ch = g->params[track][P_MCH], mode = g->params[track][P_MOUT];
        if (ch > 0 && mode != OG_MOUT_INT)
            midi_send(g, (uint8_t)(0x80 | (ch - 1)), (uint8_t)note, 0);
    }
    for (int i = 0; i < OG_VOICES; i++) {
        voice_t *v = &g->v[i];
        if (!v->active) continue;
        if (v->pending && v->pend_track == track && v->pend_note == note)
            v->pend_gate = 0;
        if (!v->fade && v->gate && v->track == track && v->note == note) {
            v->gate = 0;
            v->amp.stage = ENV_REL;
            v->flt.stage = ENV_REL;
        }
    }
}

void og_all_notes_off(og_t *g) {
    if (!g) return;
    uint32_t sent = 0;
    for (int t = 0; t < OG_TRACKS; t++) {
        int ch = g->params[t][P_MCH];
        if (ch > 0 && g->params[t][P_MOUT] != OG_MOUT_INT && !(sent & (1u << ch))) {
            midi_send(g, (uint8_t)(0xB0 | (ch - 1)), 123, 0);   /* all notes off */
            sent |= 1u << ch;
        }
    }
    for (int i = 0; i < OG_VOICES; i++) {
        voice_t *v = &g->v[i];
        v->pending = 0;
        if (v->active && !v->fade) v->fade = FADE_LEN;
    }
}

/* ADSR at control rate. Returns new level. */
static float env_tick(const og_t *g, env_t *e, int a, int d, int s, int r) {
    float sus = (float)s / 127.0f;
    switch (e->stage) {
    case ENV_ATT:
        e->level += g->att_inc[a];
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = ENV_DEC; }
        break;
    case ENV_DEC:
        e->level = sus + (e->level - sus) * g->dec_coef[d];
        if (e->level - sus < 0.0005f) { e->level = sus; e->stage = ENV_SUS; }
        break;
    case ENV_SUS:
        e->level = sus;
        break;
    case ENV_REL:
        e->level *= g->dec_coef[r];
        if (e->level < 0.00005f) { e->level = 0.0f; e->stage = ENV_IDLE; }
        break;
    default:
        e->level = 0.0f;
        break;
    }
    return e->level;
}

static inline float midi_hz(float semis) {
    return 440.0f * exp2f((semis - 69.0f) * (1.0f / 12.0f));
}

/* Render one voice for n samples (n <= OG_CTRL_RATE), adding into out. */
static float lfo_wave(const og_t *g, int wave, float ph, float rnd) {
    switch (wave) {
    case OG_WAVE_TRI:  return 4.0f * fabsf(ph - 0.5f) - 1.0f;
    case OG_WAVE_SQR:  return ph < 0.5f ? 1.0f : -1.0f;
    case OG_WAVE_SAW:  return 2.0f * ph - 1.0f;
    case OG_WAVE_RAMP: return 1.0f - 2.0f * ph;
    case OG_WAVE_RND:  return rnd;
    default:           return g->sine[(int)(ph * SINE_SIZE) & (SINE_SIZE - 1)];
    }
}

/* LFO frequency in cycles per second for a set of LFO params */
static float lfo_hz(const og_t *g, const int *p, int i) {
    int rate = p[P_LFO(i, LFO_RATE)];
    if (p[P_LFO(i, LFO_SYNC)]) {
        float steps = k_lfo_sync[rate * OG_LFO_SYNC_RATES / 128];
        float step_s = 60.0f / g->bpm / 4.0f;
        return 1.0f / (steps * step_s);
    }
    return 0.05f * exp2f((float)rate / 127.0f * 10.0f);
}

static inline float next_rnd(og_t *g) {
    g->rng ^= g->rng << 13; g->rng ^= g->rng >> 17; g->rng ^= g->rng << 5;
    return (float)(g->rng & 0xFFFF) / 32767.5f - 1.0f;
}

/* Apply the voice's 3 LFOs to a copy of its params. Returns pitch offset. */
static float apply_lfos(og_t *g, voice_t *v, int *pm, int n) {
    float pitch = 0.0f;
    float dt = (float)n * g->inv_sr;
    v->lfo_age += dt;
    for (int i = 0; i < OG_LFOS; i++) {
        const int *vp = v->p;
        int dest = vp[P_LFO(i, LFO_DEST)];
        float ph, rnd;
        if (vp[P_LFO(i, LFO_TRIG)]) {
            v->lfo_ph[i] += lfo_hz(g, vp, i) * dt;
            if (v->lfo_ph[i] >= 1.0f) { v->lfo_ph[i] -= floorf(v->lfo_ph[i]); v->lfo_rnd[i] = next_rnd(g); }
            ph = v->lfo_ph[i]; rnd = v->lfo_rnd[i];
        } else {
            ph = g->lfo_free[v->track][i];
            float off = (float)vp[P_LFO(i, LFO_PHAS)] / 128.0f;
            ph += off; if (ph >= 1.0f) ph -= 1.0f;
            rnd = g->lfo_free_rnd[v->track][i];
        }
        if (dest == OG_DEST_OFF) continue;
        float depth = ((float)vp[P_LFO(i, LFO_DPTH)] - 64.0f) / 63.0f;
        float amt = lfo_wave(g, vp[P_LFO(i, LFO_WAVE)], ph, rnd) * depth;
        int fade = vp[P_LFO(i, LFO_FADE)];
        if (fade > 0) {
            float f = v->lfo_age / env_seconds(fade);
            if (f < 1.0f) amt *= f;
        }
        if (dest == OG_DEST_PITCH) { pitch += amt * 12.0f; continue; }
        int pid = dest >= OG_DEST_SEND0 ? P_SDLY + (dest - OG_DEST_SEND0) : dest - OG_DEST_PARAM0;
        if (pid < 0 || pid >= P_COUNT_USED) continue;
        int max = og_param_max(pid);
        pm[pid] = clampi(pm[pid] + (int)lrintf(amt * (float)max), 0, max);
    }
    return pitch;
}

static void voice_render(og_t *g, voice_t *v, og_bus_t *bus, int n) {
    int pm[P_COUNT_USED];
    memcpy(pm, v->p, sizeof(pm));
    float lfo_pitch = apply_lfos(g, v, pm, n);
    const int *p = pm;
    const int algo = p[P_ALGO];
    const float velf = (float)v->vel / 127.0f;

    /* --- control-rate updates --- */
    if ((p[P_GLID] > 0 || v->slide) && v->pitch != v->target) {
        int gl = p[P_GLID] > 0 ? p[P_GLID] : 40;
        float rate = g->dec_coef[gl / 2];   /* shorter than env range */
        v->pitch = v->target + (v->pitch - v->target) * rate;
        if (fabsf(v->pitch - v->target) < 0.001f) v->pitch = v->target;
    } else {
        v->pitch = v->target;
    }
    float base = midi_hz(v->pitch + lfo_pitch);

    float det = (float)p[P_DTUN] / 127.0f * 0.02f;  /* up to ~35 cents */
    float rat[5];
    rat[1] = k_ratios[p[P_RAT1]];
    rat[2] = k_ratios[p[P_RAT2]] * (1.0f + det);
    rat[3] = k_ratios[p[P_RAT3]] * (1.0f - det);
    rat[4] = k_ratios[p[P_RAT4]] * (1.0f + det * 0.5f);
    uint32_t inc[5];
    for (int k = 1; k <= 4; k++) {
        float f = clampf(base * rat[k], 0.0f, g->sr * 0.49f);
        inc[k] = (uint32_t)(f * g->inv_sr * TWO32);
    }

    /* modulator envelopes and depths (in cycles of phase) */
    float velm = 1.0f - ((float)p[P_VELM] / 127.0f) * (1.0f - velf);
    float lvl[5] = {0};
    for (int k = 2; k <= 4; k++) {
        int dec = p[P_DEC2 + (k - 2)];
        if (dec < 127) v->mod_env[k] *= g->dec_coef[dec];
        float l = (float)p[P_LVL2 + (k - 2)] / 127.0f;
        lvl[k] = l * l * 1.5f * v->mod_env[k] * velm;
    }
    float fbk = (float)p[P_FDBK] / 127.0f;
    fbk = fbk * fbk * 0.25f;

    /* modulation coefficients a[src][dst], in uint32 phase units */
    float a43 = k_route[algo][4][3] ? lvl[4] * TWO32 : 0.0f;
    float a42 = k_route[algo][4][2] ? lvl[4] * TWO32 : 0.0f;
    float a41 = k_route[algo][4][1] ? lvl[4] * TWO32 : 0.0f;
    float a32 = k_route[algo][3][2] ? lvl[3] * TWO32 : 0.0f;
    float a31 = k_route[algo][3][1] ? lvl[3] * TWO32 : 0.0f;
    float a21 = k_route[algo][2][1] ? lvl[2] * TWO32 : 0.0f;
    float fbs = fbk * TWO32;

    /* carrier gains: op1 always full; other carriers use their level */
    uint8_t cm = k_carriers[algo];
    int ncar = 0;
    for (int k = 1; k <= 4; k++) ncar += (cm >> k) & 1;
    float norm = 1.0f / sqrtf((float)ncar);
    float c1 = (cm & 0x02) ? norm : 0.0f;
    float c2 = (cm & 0x04) ? norm * (float)p[P_LVL2] / 127.0f : 0.0f;
    float c3 = (cm & 0x08) ? norm * (float)p[P_LVL3] / 127.0f : 0.0f;
    float c4 = (cm & 0x10) ? norm * (float)p[P_LVL4] / 127.0f : 0.0f;

    /* filter */
    float fenv = env_tick(g, &v->flt, p[P_FATK], p[P_FDEC], p[P_FSUS], p[P_FREL]);
    float semis_off = ((float)p[P_FENV] - 64.0f) / 63.0f * 96.0f * fenv;
    float fc = 20.0f * powf(1000.0f, (float)p[P_FREQ] / 127.0f) * exp2f(semis_off / 12.0f);
    fc = clampf(fc, 20.0f, g->sr * 0.45f);
    float gg = tanf((float)M_PI * fc * g->inv_sr);
    float res = (float)p[P_RESO] / 127.0f;
    float k1 = 2.0f - 1.96f * res;
    int ftype = p[P_FTYP];
    float kA = (ftype == OG_FILTER_LP24) ? 1.414f : k1;
    float a1A = 1.0f / (1.0f + gg * (gg + kA)), a2A = gg * a1A, a3A = gg * a2A;
    float a1B = 1.0f / (1.0f + gg * (gg + k1)), a2B = gg * a1B, a3B = gg * a2B;

    /* amp */
    float aenv = env_tick(g, &v->amp, p[P_AATK], p[P_ADEC], p[P_ASUS], p[P_AREL]);
    float vela = 1.0f - ((float)p[P_VELA] / 127.0f) * (1.0f - velf);
    float vol = (float)p[P_VOL] / 127.0f;
    float amp_target = aenv * vela * vol * vol;
    float amp = v->amp_out;
    float amp_step = (amp_target - amp) / (float)n;

    float drive = (float)p[P_DRIV] / 127.0f;
    float dgain = 1.0f + drive * drive * 15.0f;
    float dcomp = drive > 0.0f ? 1.0f / soft_clip(dgain) : 1.0f;

    float pl = g->pan_l[p[P_PAN]], pr = g->pan_r[p[P_PAN]];
    float sd = (float)p[P_SDLY] / 127.0f, sr_ = (float)p[P_SREV] / 127.0f, sc = (float)p[P_SCHO] / 127.0f;
    sd *= sd; sr_ *= sr_; sc *= sc;

    const float *t = g->sine;
    uint32_t ph1 = v->ph[1], ph2 = v->ph[2], ph3 = v->ph[3], ph4 = v->ph[4];
    float fb1 = v->fb1, fb2 = v->fb2;
    float i1a = v->ic1[0], i2a = v->ic2[0], i1b = v->ic1[1], i2b = v->ic2[1];

    for (int s = 0; s < n; s++) {
        float o4 = sine_lookup(t, ph4 + (uint32_t)(int64_t)((fb1 + fb2) * 0.5f * fbs));
        fb2 = fb1; fb1 = o4;
        float o3 = sine_lookup(t, ph3 + (uint32_t)(int64_t)(o4 * a43));
        float o2 = sine_lookup(t, ph2 + (uint32_t)(int64_t)(o4 * a42 + o3 * a32));
        float o1 = sine_lookup(t, ph1 + (uint32_t)(int64_t)(o4 * a41 + o3 * a31 + o2 * a21));
        ph1 += inc[1]; ph2 += inc[2]; ph3 += inc[3]; ph4 += inc[4];

        float x = o1 * c1 + o2 * c2 + o3 * c3 + o4 * c4;

        /* stage A */
        float v3 = x - i2a;
        float v1 = a1A * i1a + a2A * v3;
        float v2 = i2a + a2A * i1a + a3A * v3;
        i1a = 2.0f * v1 - i1a;
        i2a = 2.0f * v2 - i2a;
        float y;
        switch (ftype) {
        case OG_FILTER_HP:    y = x - kA * v1 - v2; break;
        case OG_FILTER_BP:    y = v1; break;
        case OG_FILTER_NOTCH: y = x - kA * v1; break;
        case OG_FILTER_LP24: {
            float w3 = v2 - i2b;
            float w1 = a1B * i1b + a2B * w3;
            float w2 = i2b + a2B * i1b + a3B * w3;
            i1b = 2.0f * w1 - i1b;
            i2b = 2.0f * w2 - i2b;
            y = w2;
            break;
        }
        default:              y = v2; break;
        }

        if (drive > 0.0f) y = soft_clip(y * dgain) * dcomp;

        amp += amp_step;
        float gsm = amp;
        if (v->fade) {
            gsm *= (float)v->fade / (float)FADE_LEN;
            if (--v->fade == 0) {
                /* faded out: stop rendering this voice */
                amp = 0.0f;
                v->active = 0;
                break;
            }
        }
        y *= gsm;
        float yl = y * pl, yr = y * pr;
        bus->dry_l[s] += yl; bus->dry_r[s] += yr;
        bus->dly_l[s] += yl * sd; bus->dly_r[s] += yr * sd;
        bus->rev_l[s] += yl * sr_; bus->rev_r[s] += yr * sr_;
        bus->cho_l[s] += yl * sc; bus->cho_r[s] += yr * sc;
    }

    v->ph[1] = ph1; v->ph[2] = ph2; v->ph[3] = ph3; v->ph[4] = ph4;
    v->fb1 = fb1; v->fb2 = fb2;
    v->ic1[0] = i1a; v->ic2[0] = i2a; v->ic1[1] = i1b; v->ic2[1] = i2b;
    v->amp_out = amp;

    /* denormal guard */
    if (fabsf(v->ic1[0]) < 1e-20f) v->ic1[0] = 0.0f;
    if (fabsf(v->ic2[0]) < 1e-20f) v->ic2[0] = 0.0f;
    if (fabsf(v->ic1[1]) < 1e-20f) v->ic1[1] = 0.0f;
    if (fabsf(v->ic2[1]) < 1e-20f) v->ic2[1] = 0.0f;

    if (v->active && v->amp.stage == ENV_IDLE && v->amp_out < 1e-5f)
        v->active = 0;
}

/* ---- render --------------------------------------------------------- */

static void advance_free_lfos(og_t *g, int n) {
    float dt = (float)n * g->inv_sr;
    for (int t = 0; t < OG_TRACKS; t++)
        for (int i = 0; i < OG_LFOS; i++) {
            float *ph = &g->lfo_free[t][i];
            *ph += lfo_hz(g, g->params[t], i) * dt;
            if (*ph >= 1.0f) { *ph -= floorf(*ph); g->lfo_free_rnd[t][i] = next_rnd(g); }
        }
}

void og_render_float(og_t *g, float *out_lr, int frames) {
    og_bus_t bus;
    float bl[OG_CTRL_RATE], br[OG_CTRL_RATE];
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > OG_CTRL_RATE) n = OG_CTRL_RATE;
        memset(&bus, 0, sizeof(bus));
        og__seq_advance(g, n);
        advance_free_lfos(g, n);

        for (int i = 0; i < OG_VOICES; i++) {
            voice_t *v = &g->v[i];
            if (!v->active && v->pending) {
                int gate = v->pend_gate;
                voice_start(g, v, v->pend_track, v->pend_note, v->pend_vel,
                            v->pend_p, v->pend_lockmask, v->pend_slide);
                if (!gate) og_note_off(g, v->track, v->note);
            }
            if (v->active) voice_render(g, v, &bus, n);
            if (!v->active && v->pending) {
                int gate = v->pend_gate;
                voice_start(g, v, v->pend_track, v->pend_note, v->pend_vel,
                            v->pend_p, v->pend_lockmask, v->pend_slide);
                if (!gate) og_note_off(g, v->track, v->note);
            }
        }

        og__fx_process(g, &bus, bl, br, n);
        og__click_render(g, bl, br, n);
        float m = g->master * 0.5f;   /* headroom for 12 voices */
        for (int s = 0; s < n; s++) {
            out_lr[2 * (done + s)]     = soft_clip(bl[s] * m);
            out_lr[2 * (done + s) + 1] = soft_clip(br[s] * m);
        }
        done += n;
    }
}

void og_render(og_t *g, int16_t *out_lr, int frames) {
    float buf[2 * 256];
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > 256) n = 256;
        og_render_float(g, buf, n);
        for (int i = 0; i < 2 * n; i++) {
            float x = buf[i] * 32767.0f;
            out_lr[2 * done + i] = (int16_t)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : x);
        }
        done += n;
    }
}
