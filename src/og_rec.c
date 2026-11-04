/*
 * OpGrid recording: live notes and knob moves into the playing clip,
 * overdub/replace, new clips that grow until closed, count-in and click,
 * one level of take undo, project reset, and clip (de)serialization.
 *
 * Everything here runs on the audio thread: no allocation, no I/O.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "og_internal.h"

static inline int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static int track_ok(int t) { return t >= 0 && t < OG_TRACKS; }

void og__rec_init(og_t *g) {
    g->rec = 0;
    g->rec_track = 0;
    g->rec_quant = 1;
    g->rec_mode = 0;
    g->click = OG_CLICK_COUNTIN;
    g->countin = 0.0;
    g->rec_grow = -1;
    g->undo_ok = 0;
    for (int s = 0; s < OG_STEPS; s++) g->rec_mark[s] = -1;
    memset(g->rec_hold, 0, sizeof(g->rec_hold));
    g->click_inc = (uint32_t)(1000.0f / g->sr * 4294967296.0f);
}

/* the clip recording writes to: the record track's playing clip */
static og_clip_t *rec_clip(og_t *g) {
    int t = g->rec_track;
    if (!track_ok(t) || g->ts[t].clip < 0) return NULL;
    return &g->clips[t][g->ts[t].clip];
}

static void snapshot(og_t *g, int t, int c) {
    if (!track_ok(t) || c < 0 || c >= OG_CLIPS) return;
    g->undo_clip = g->clips[t][c];
    g->undo_t = t;
    g->undo_c = c;
    g->undo_ok = 1;
}

void og_rec_arm(og_t *g, int on) {
    if (!g) return;
    on = on ? 1 : 0;
    if (on && !g->rec) {
        int t = g->rec_track;
        int c = g->ts[t].clip >= 0 ? g->ts[t].clip : g->ts[t].edit;
        snapshot(g, t, c);
        for (int s = 0; s < OG_STEPS; s++) g->rec_mark[s] = -1;
    }
    if (!on) memset(g->rec_hold, 0, sizeof(g->rec_hold));
    g->rec = on;
}
int  og_rec_armed(const og_t *g) { return g ? g->rec : 0; }
int  og_rec_quant(const og_t *g) { return g ? g->rec_quant : 1; }
int  og_rec_mode(const og_t *g) { return g ? g->rec_mode : 0; }
void og_rec_set_quant(og_t *g, int on) { if (g) g->rec_quant = on ? 1 : 0; }
void og_rec_set_mode(og_t *g, int replace) { if (g) g->rec_mode = replace ? 1 : 0; }
void og_set_click(og_t *g, int mode) { if (g) g->click = clampi(mode, 0, OG_CLICK_ON); }
int  og_get_click(const og_t *g) { return g ? g->click : 0; }
int  og_rec_growing(const og_t *g) { return g ? g->rec_grow : -1; }
int  og_counting_in(const og_t *g) { return g && g->countin > 0.0; }

void og_rec_set_track(og_t *g, int track) {
    if (!g || !track_ok(track) || track == g->rec_track) return;
    g->rec_track = track;
    for (int s = 0; s < OG_STEPS; s++) g->rec_mark[s] = -1;
    memset(g->rec_hold, 0, sizeof(g->rec_hold));
    if (g->rec) {
        int c = g->ts[track].clip >= 0 ? g->ts[track].clip : g->ts[track].edit;
        snapshot(g, track, c);
    }
}

int og_undo(og_t *g) {
    if (!g || !g->undo_ok) return 0;
    g->clips[g->undo_t][g->undo_c] = g->undo_clip;
    g->undo_ok = 0;
    if (g->rec_grow == g->undo_t) g->rec_grow = -1;
    return 1;
}

/* ---- count-in and click ---------------------------------------------- */

static void click(og_t *g, int accent) {
    g->click_amp = accent ? 0.5f : 0.3f;
    g->click_inc = (uint32_t)((accent ? 1500.0f : 1000.0f) / g->sr * 4294967296.0f);
    g->click_ph = 0;
}

void og_play_countin(og_t *g) {
    if (!g || g->playing || g->countin > 0.0) return;
    if (g->click == OG_CLICK_OFF) { og_play(g, 1); return; }
    g->countin = 16.0;
}

int og__countin_advance(og_t *g, int frames, double step_samples) {
    if (g->countin <= 0.0) return 0;
    double before = g->countin, after = before - (double)frames / step_samples;
    double e0 = 16.0 - before, e1 = 16.0 - after;
    long b0 = (long)floor(e0 / 4.0), b1 = (long)floor(e1 / 4.0);
    if (e0 == 0.0) click(g, 1);
    else if (b1 > b0 && b1 < 4) click(g, 0);
    if (after <= 0.0) {
        g->countin = 0.0;
        og_play(g, 1);
        return 0;
    }
    g->countin = after;
    return 1;
}

void og__click_beats(og_t *g, double gs, double ge) {
    if (g->click != OG_CLICK_ON || !g->rec) return;
    long b0 = (long)ceil(gs / 4.0 - 1e-9), b1 = (long)ceil(ge / 4.0 - 1e-9);
    if (b1 > b0) click(g, (b0 % 4) == 0);
}

void og__click_render(og_t *g, float *l, float *r, int n) {
    if (g->click_amp < 1e-4f) return;
    float decay = 0.9985f;
    for (int s = 0; s < n; s++) {
        float x = g->sine[g->click_ph >> (32 - SINE_BITS)] * g->click_amp;
        g->click_ph += g->click_inc;
        g->click_amp *= decay;
        l[s] += x;
        r[s] += x;
    }
}

/* ---- recording notes and knobs ---------------------------------------- */

static void clear_step(og_clip_t *c, int s) {
    og_step_t *st = &c->steps[s];
    memset(st, 0, sizeof(*st));
    st->notes[0] = 60; st->vel = 100; st->len = OG_LEN_1STEP; st->rat = 1;
    for (int i = 0; i < OG_LOCKS; i++) if (c->locks[i].used && c->locks[i].step == s) c->locks[i].used = 0;
}

/* where "now" lands in the record track's clip: absolute step n + micro */
static int rec_position(og_t *g, long *n_out, int *micro_out) {
    og_trackseq_t *ts = &g->ts[g->rec_track];
    double pos = ts->pos;
    long n;
    int micro = 0;
    if (g->rec_quant) n = (long)floor(pos + 0.5);
    else {
        n = (long)floor(pos + 0.5);
        micro = (int)lrint((pos - (double)n) * 48.0);
        micro = clampi(micro, -23, 23);
    }
    if (n < 0) n = 0;
    *n_out = n;
    *micro_out = micro;
    return 1;
}

static int recording_now(og_t *g, int track) {
    return g->rec && g->playing && track == g->rec_track && g->ts[track].clip >= 0;
}

void og_live_note_on(og_t *g, int track, int note, int vel) {
    if (!g || !track_ok(track)) return;
    note = clampi(note, 1, 127);
    og_note_on(g, track, note, vel);
    if (!recording_now(g, track)) return;
    og_clip_t *c = rec_clip(g);
    long n; int micro;
    rec_position(g, &n, &micro);
    int s = (int)(n % c->length);
    og_step_t *st = &c->steps[s];
    if (g->rec_mark[s] == n && (st->flags & OG_STEP_NOTE)) {
        /* same step in this pass: build a chord */
        int k;
        for (k = 0; k < OG_CHORD; k++) if (st->notes[k] == note) break;
        if (k == OG_CHORD)
            for (k = 1; k < OG_CHORD; k++) if (!st->notes[k]) { st->notes[k] = (uint8_t)note; break; }
    } else {
        int keep_locks = (st->flags & (OG_STEP_NOTE | OG_STEP_LOCK)) && g->rec_mode == 0;
        if (!keep_locks) clear_step(c, s);
        st->flags = (uint8_t)((st->flags & OG_STEP_SLIDE) | OG_STEP_NOTE);
        st->notes[0] = (uint8_t)note;
        st->notes[1] = st->notes[2] = st->notes[3] = 0;
        st->vel = (uint8_t)clampi(vel, 1, 127);
        st->len = OG_LEN_1STEP;
        st->rat = 1;
        st->cond = OG_COND_NONE;
        st->micro = (int8_t)micro;
        g->rec_mark[s] = n;
    }
    c->used = 1;
    g->rec_hold[note].on = 1;
    g->rec_hold[note].track = track;
    g->rec_hold[note].step = s;
    g->rec_hold[note].start = g->ts[track].pos;
}

void og_live_note_off(og_t *g, int track, int note) {
    if (!g || !track_ok(track)) return;
    note = clampi(note, 1, 127);
    og_note_off(g, track, note);
    if (!g->rec_hold[note].on || g->rec_hold[note].track != track) return;
    g->rec_hold[note].on = 0;
    if (!recording_now(g, track)) return;
    og_clip_t *c = rec_clip(g);
    double held = g->ts[track].pos - g->rec_hold[note].start;
    if (held < 0.0) return;
    /* nearest length in the table */
    int best = 0;
    for (int i = 1; i < OG_LENS; i++)
        if (fabs(og_len_steps(i) - held) < fabs(og_len_steps(best) - held)) best = i;
    og_step_t *st = &c->steps[g->rec_hold[note].step];
    if (st->flags & OG_STEP_NOTE) st->len = (uint8_t)best;
}

void og_rec_param(og_t *g, int track, int pid, int value) {
    if (!g || !track_ok(track)) return;
    if (!recording_now(g, track)) { og_set_param(g, track, pid, value); return; }
    og_clip_t *c = rec_clip(g);
    long n; int micro;
    int q = g->rec_quant; g->rec_quant = 1;
    rec_position(g, &n, &micro);
    g->rec_quant = q;
    int s = (int)(n % c->length);
    value = clampi(value, 0, og__pmax(g, track, pid));
    /* a lock needs a step to live on: an empty step becomes lock-only */
    if (!(c->steps[s].flags & (OG_STEP_NOTE | OG_STEP_LOCK))) c->steps[s].flags |= OG_STEP_LOCK;
    og_lock_t *slot = NULL;
    for (int i = 0; i < OG_LOCKS; i++) {
        og_lock_t *l = &c->locks[i];
        if (l->used && l->step == s && l->pid == pid) { slot = l; break; }
        if (!l->used && !slot) slot = l;
    }
    if (!slot) return;                    /* pool full */
    slot->used = 1; slot->step = (uint8_t)s; slot->pid = (uint8_t)pid; slot->val = (uint8_t)value;
    c->used = 1;
    /* hear it now on the sounding voices */
    for (int i = 0; i < OG_VOICES; i++) {
        voice_t *v = &g->v[i];
        if (v->active && !v->fade && v->track == track) { v->p[pid] = value; v->lockmask |= 1ull << pid; }
    }
}

void og__rec_cross(og_t *g, int track, double start, double end) {
    if (!g->rec || g->rec_mode != 1 || track != g->rec_track || g->ts[track].clip < 0) return;
    og_clip_t *c = &g->clips[track][g->ts[track].clip];
    for (long n = (long)ceil(start); (double)n < end; n++) {
        if (n < 0) continue;
        int s = (int)(n % c->length);
        if (g->rec_mark[s] != n) clear_step(c, s);
    }
}

void og_rec_new_clip(og_t *g, int track, int clip) {
    if (!g || !track_ok(track) || clip < 0 || clip >= OG_CLIPS) return;
    snapshot(g, track, clip);
    og__clip_reset(&g->clips[track][clip]);
    g->clips[track][clip].length = OG_STEPS;
    g->clips[track][clip].used = 1;
    g->rec_track = track;
    for (int s = 0; s < OG_STEPS; s++) g->rec_mark[s] = -1;
    g->rec = 1;
    g->rec_grow = track;
    og_set_edit_clip(g, track, clip);
    og_clip_launch(g, track, clip);
}

void og_rec_close_clip(og_t *g, int track) {
    if (!g || track != g->rec_grow) return;
    og_trackseq_t *ts = &g->ts[track];
    g->rec_grow = -1;
    if (ts->clip < 0) return;
    long steps = (long)ceil(ts->pos - 0.5);           /* round to the nearest step boundary */
    long bars = (steps + 15) / 16;
    if (bars < 1) bars = 1;
    if (bars > 4) bars = 4;
    og_clip_t *c = &g->clips[track][ts->clip];
    c->length = (uint8_t)(bars * 16);
    /* keep looping in phase: fold the position into the new length */
    /* (no fmod: the cross toolchain's fmod needs a newer glibc than the Move has) */
    ts->pos -= floor(ts->pos / (double)c->length) * (double)c->length;
}

/* ---- reset ------------------------------------------------------------ */

void og_reset(og_t *g) {
    if (!g) return;
    og_play(g, 0);
    g->countin = 0.0;
    og_all_notes_off(g);
    for (int t = 0; t < OG_TRACKS; t++) {
        for (int p = 0; p < P_COUNT_USED; p++) og_set_param(g, t, p, og_param_default(p));
        g->ts[t].mute = 0;
        g->solo[t] = 0;
    }
    for (int i = 0; i < FX_COUNT; i++) og_set_fx(g, i, og_fx_default(i));
    og__seq_init(g);
    og__rec_init(g);
    og__song_init(g);
    og_set_tempo(g, 120.0f);
    g->fill = 0;
}

/* ---- clip serialization ---------------------------------------------- */

static const char k_hex[] = "0123456789abcdef";

int og_clip_dump(const og_t *g, int track, int clip, char *out, int out_len) {
    if (!g || !track_ok(track) || clip < 0 || clip >= OG_CLIPS || out_len < OG_CLIP_HEX_MAX + 1) return -1;
    const og_clip_t *c = &g->clips[track][clip];
    uint8_t b[4 + OG_STEPS * 10 + 1 + OG_LOCKS * 3];
    int k = 0;
    b[k++] = c->length; b[k++] = c->speed; b[k++] = c->swing; b[k++] = c->used;
    for (int s = 0; s < OG_STEPS; s++) {
        const og_step_t *st = &c->steps[s];
        b[k++] = st->flags;
        for (int i = 0; i < OG_CHORD; i++) b[k++] = st->notes[i];
        b[k++] = st->vel; b[k++] = st->len; b[k++] = st->rat; b[k++] = st->cond;
        b[k++] = (uint8_t)(st->micro + 64);
    }
    int nl = 0;
    for (int i = 0; i < OG_LOCKS; i++) nl += c->locks[i].used;
    b[k++] = (uint8_t)nl;
    for (int i = 0; i < OG_LOCKS; i++)
        if (c->locks[i].used) { b[k++] = c->locks[i].step; b[k++] = c->locks[i].pid; b[k++] = c->locks[i].val; }
    for (int i = 0; i < k; i++) { out[2 * i] = k_hex[b[i] >> 4]; out[2 * i + 1] = k_hex[b[i] & 15]; }
    out[2 * k] = 0;
    return 2 * k;
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int og_clip_load(og_t *g, int track, int clip, const char *hex) {
    if (!g || !track_ok(track) || clip < 0 || clip >= OG_CLIPS || !hex) return 0;
    uint8_t b[4 + OG_STEPS * 10 + 1 + OG_LOCKS * 3];
    int n = 0;
    for (const char *p = hex; p[0] && p[1] && n < (int)sizeof(b); p += 2) {
        int hi = hexval(p[0]), lo = hexval(p[1]);
        if (hi < 0 || lo < 0) return 0;
        b[n++] = (uint8_t)(hi << 4 | lo);
    }
    int fixed = 4 + OG_STEPS * 10 + 1;
    if (n < fixed) return 0;
    int nl = b[fixed - 1];
    if (nl > OG_LOCKS || n < fixed + nl * 3) return 0;
    og_clip_t c;
    memset(&c, 0, sizeof(c));
    c.length = (uint8_t)clampi(b[0], 1, OG_STEPS);
    c.speed = (uint8_t)clampi(b[1], 0, OG_SPEEDS - 1);
    c.swing = (uint8_t)clampi(b[2], 50, 80);
    c.used = b[3] ? 1 : 0;
    int k = 4;
    for (int s = 0; s < OG_STEPS; s++) {
        og_step_t *st = &c.steps[s];
        st->flags = b[k++] & 7;
        for (int i = 0; i < OG_CHORD; i++) st->notes[i] = (uint8_t)clampi(b[k++], 0, 127);
        if (!st->notes[0]) st->notes[0] = 60;
        st->vel = (uint8_t)clampi(b[k++], 1, 127);
        st->len = (uint8_t)clampi(b[k++], 0, OG_LENS - 1);
        st->rat = (uint8_t)clampi(b[k++], 1, 8);
        st->cond = (uint8_t)clampi(b[k++], 0, OG_CONDS - 1);
        st->micro = (int8_t)clampi((int)b[k++] - 64, -23, 23);
    }
    k++;
    for (int i = 0; i < nl; i++) {
        int s = b[k++], pid = b[k++], v = b[k++];
        if (s >= OG_STEPS || pid >= P_COUNT_USED) continue;
        c.locks[i].used = 1; c.locks[i].step = (uint8_t)s; c.locks[i].pid = (uint8_t)pid;
        c.locks[i].val = (uint8_t)clampi(v, 0, og_param_max(pid));
    }
    g->clips[track][clip] = c;
    return 1;
}
