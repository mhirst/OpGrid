/*
 * OpGrid sequencer — runs inside the audio callback, one control tick
 * (16 samples) at a time, so every event lands within 0.36 ms.
 *
 * Each track keeps its own position in steps (a double), advanced by the
 * tempo and the clip's speed. Events are found by scanning the steps whose
 * trigger time (step + swing + micro-timing + ratchet offset) falls inside
 * the window this tick covers. No allocation, no I/O.
 */
#include <math.h>
#include <string.h>

#include "og_internal.h"

static const float k_lens[OG_LENS] = {
    0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f
};
static const float k_speeds[OG_SPEEDS] = { 0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };

static inline int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

float og_len_steps(int idx) { return k_lens[clampi(idx, 0, OG_LENS - 1)]; }
float og_speed_mul(int idx) { return k_speeds[clampi(idx, 0, OG_SPEEDS - 1)]; }

/* the clip the UI is editing */
static og_clip_t *cur_clip(og_t *g, int track) {
    og_clip_t *c = &g->clips[track][g->ts[track].edit];
    return c;
}
static const og_clip_t *cur_clip_c(const og_t *g, int track) {
    return &g->clips[track][g->ts[track].edit];
}
/* the clip that is playing (caller checks ts.clip >= 0) */
static og_clip_t *play_clip(og_t *g, int track) {
    return &g->clips[track][g->ts[track].clip];
}
static int track_ok(int t) { return t >= 0 && t < OG_TRACKS; }
static int step_ok(int s) { return s >= 0 && s < OG_STEPS; }

static void step_reset(og_step_t *st) {
    memset(st, 0, sizeof(*st));
    st->notes[0] = 60;
    st->vel = 100;
    st->len = OG_LEN_1STEP;
    st->rat = 1;
}

void og__clip_reset(og_clip_t *c) {
    memset(c, 0, sizeof(*c));
    for (int s = 0; s < OG_STEPS; s++) step_reset(&c->steps[s]);
    c->length = 16;
    c->speed = OG_SPEED_1X;
    c->swing = 50;
}

void og__seq_init(og_t *g) {
    for (int t = 0; t < OG_TRACKS; t++) {
        for (int c = 0; c < OG_CLIPS; c++) og__clip_reset(&g->clips[t][c]);
        g->ts[t].cur_step = -1;
        g->ts[t].clip = 0;
        g->ts[t].edit = 0;
        g->ts[t].queued = OG_Q_NONE;
    }
    g->launch_q = 3;            /* 1 bar */
    g->bpm = 120.0f;
    g->rng = 0x9E3779B9u;
}

static void log_ev(og_t *g, int track, int note, int vel, int on);

static uint32_t rnd(og_t *g) {
    uint32_t x = g->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g->rng = x;
}

/* ---- transport ------------------------------------------------------- */

static void offs_flush(og_t *g, int track) {
    for (int i = 0; i < OG_NOTEOFFS; i++) {
        og_noteoff_t *o = &g->offs[i];
        if (o->used && (track < 0 || o->track == track)) {
            og_note_off(g, o->track, o->note);
            log_ev(g, o->track, o->note, 0, 0);
            o->used = 0;
        }
    }
}

void og_play(og_t *g, int on) {
    if (!g) return;
    on = on ? 1 : 0;
    if (on == g->playing) return;
    if (on) {
        g->gpos = 0.0;
        for (int t = 0; t < OG_TRACKS; t++) {
            og_trackseq_t *ts = &g->ts[t];
            if (ts->queued != OG_Q_NONE) {   /* queued while stopped: take effect now */
                ts->clip = ts->queued == OG_Q_STOP ? -1 : ts->queued;
                ts->queued = OG_Q_NONE;
            }
            ts->pos = 0.0;
            ts->cur_step = -1;
            ts->pre = 0;
        }
    } else {
        offs_flush(g, -1);
        for (int t = 0; t < OG_TRACKS; t++) g->ts[t].cur_step = -1;
    }
    g->playing = on;
}

int   og_is_playing(const og_t *g) { return g ? g->playing : 0; }
void  og_set_tempo(og_t *g, float bpm) { if (g) g->bpm = bpm < 30.0f ? 30.0f : bpm > 300.0f ? 300.0f : bpm; }
float og_get_tempo(const og_t *g) { return g ? g->bpm : 120.0f; }
void  og_set_fill(og_t *g, int on) { if (g) g->fill = on ? 1 : 0; }
void  og_set_mute(og_t *g, int track, int on) { if (g && track_ok(track)) g->ts[track].mute = on ? 1 : 0; }
int   og_get_mute(const og_t *g, int track) { return (g && track_ok(track)) ? g->ts[track].mute : 0; }

int og_track_step(const og_t *g, int track) {
    if (!g || !track_ok(track) || !g->playing) return -1;
    const og_trackseq_t *ts = &g->ts[track];
    if (ts->clip < 0 || ts->clip != ts->edit) return -1;   /* playhead only on the edited clip */
    const og_clip_t *c = cur_clip_c(g, track);
    long n = (long)floor(g->ts[track].pos);
    if (n < 0) return -1;
    return (int)(n % c->length);
}

/* ---- steps ----------------------------------------------------------- */

void og_step_set(og_t *g, int track, int step, int field, int value) {
    if (!g || !track_ok(track) || !step_ok(step)) return;
    og_clip_t *clip = cur_clip(g, track);
    clip->used = 1;
    og_step_t *st = &clip->steps[step];
    switch (field) {
    case OG_F_FLAGS: st->flags = (uint8_t)clampi(value, 0, 7); break;
    case OG_F_NOTE0: st->notes[0] = (uint8_t)clampi(value, 1, 127); break;
    case OG_F_NOTE1: case OG_F_NOTE2: case OG_F_NOTE3:
        st->notes[field - OG_F_NOTE0] = (uint8_t)clampi(value, 0, 127); break;
    case OG_F_VEL:   st->vel = (uint8_t)clampi(value, 1, 127); break;
    case OG_F_LEN:   st->len = (uint8_t)clampi(value, 0, OG_LENS - 1); break;
    case OG_F_RAT:   st->rat = (uint8_t)clampi(value, 1, 8); break;
    case OG_F_COND:  st->cond = (uint8_t)clampi(value, 0, OG_CONDS - 1); break;
    case OG_F_MICRO: st->micro = (int8_t)clampi(value, -23, 23); break;
    default: break;
    }
}

int og_step_get(const og_t *g, int track, int step, int field) {
    if (!g || !track_ok(track) || !step_ok(step)) return 0;
    const og_step_t *st = &cur_clip_c(g, track)->steps[step];
    switch (field) {
    case OG_F_FLAGS: return st->flags;
    case OG_F_NOTE0: case OG_F_NOTE1: case OG_F_NOTE2: case OG_F_NOTE3:
        return st->notes[field - OG_F_NOTE0];
    case OG_F_VEL:   return st->vel;
    case OG_F_LEN:   return st->len;
    case OG_F_RAT:   return st->rat;
    case OG_F_COND:  return st->cond;
    case OG_F_MICRO: return st->micro;
    default: return 0;
    }
}

void og_step_clear(og_t *g, int track, int step) {
    if (!g || !track_ok(track) || !step_ok(step)) return;
    og_clip_t *c = cur_clip(g, track);
    step_reset(&c->steps[step]);
    for (int i = 0; i < OG_LOCKS; i++)
        if (c->locks[i].used && c->locks[i].step == step) c->locks[i].used = 0;
}

int og_lock_set(og_t *g, int track, int step, int pid, int value) {
    if (!g || !track_ok(track) || !step_ok(step) || pid < 0 || pid >= P_COUNT_USED) return 0;
    og_clip_t *c = cur_clip(g, track);
    value = clampi(value, 0, og_param_max(pid));
    og_lock_t *free_slot = NULL;
    for (int i = 0; i < OG_LOCKS; i++) {
        og_lock_t *l = &c->locks[i];
        if (l->used && l->step == step && l->pid == pid) { l->val = (uint8_t)value; return 1; }
        if (!l->used && !free_slot) free_slot = l;
    }
    if (!free_slot) return 0;
    c->used = 1;
    free_slot->used = 1;
    free_slot->step = (uint8_t)step;
    free_slot->pid = (uint8_t)pid;
    free_slot->val = (uint8_t)value;
    return 1;
}

void og_lock_clear(og_t *g, int track, int step, int pid) {
    if (!g || !track_ok(track)) return;
    og_clip_t *c = cur_clip(g, track);
    for (int i = 0; i < OG_LOCKS; i++) {
        og_lock_t *l = &c->locks[i];
        if (l->used && l->step == step && l->pid == pid) l->used = 0;
    }
}

int og_lock_get(const og_t *g, int track, int step, int pid) {
    if (!g || !track_ok(track)) return -1;
    const og_clip_t *c = cur_clip_c(g, track);
    for (int i = 0; i < OG_LOCKS; i++) {
        const og_lock_t *l = &c->locks[i];
        if (l->used && l->step == step && l->pid == pid) return l->val;
    }
    return -1;
}

uint64_t og_lock_steps(const og_t *g, int track) {
    if (!g || !track_ok(track)) return 0;
    const og_clip_t *c = cur_clip_c(g, track);
    uint64_t m = 0;
    for (int i = 0; i < OG_LOCKS; i++) if (c->locks[i].used) m |= 1ull << c->locks[i].step;
    return m;
}

int og_lock_count(const og_t *g, int track) {
    if (!g || !track_ok(track)) return 0;
    const og_clip_t *c = cur_clip_c(g, track);
    int n = 0;
    for (int i = 0; i < OG_LOCKS; i++) n += c->locks[i].used;
    return n;
}

/* ---- clip settings --------------------------------------------------- */

void og_clip_set_length(og_t *g, int t, int len) { if (g && track_ok(t)) { cur_clip(g, t)->length = (uint8_t)clampi(len, 1, OG_STEPS); cur_clip(g, t)->used = 1; } }
void og_clip_set_speed(og_t *g, int t, int sp)   { if (g && track_ok(t)) { cur_clip(g, t)->speed = (uint8_t)clampi(sp, 0, OG_SPEEDS - 1); cur_clip(g, t)->used = 1; } }
void og_clip_set_swing(og_t *g, int t, int sw)   { if (g && track_ok(t)) { cur_clip(g, t)->swing = (uint8_t)clampi(sw, 50, 80); cur_clip(g, t)->used = 1; } }

/* ---- clips and scenes ------------------------------------------------ */

static const int k_launch_q[OG_LAUNCH_QS] = { 0, 1, 4, 16, 32, 64 };
static int clip_ok(int c) { return c >= 0 && c < OG_CLIPS; }

void og_set_launch_quant(og_t *g, int q) { if (g) g->launch_q = clampi(q, 0, OG_LAUNCH_QS - 1); }
int  og_get_launch_quant(const og_t *g) { return g ? g->launch_q : 3; }

static void queue(og_t *g, int track, int what) {
    og_trackseq_t *ts = &g->ts[track];
    if (!g->playing) {                 /* stopped transport: switch now */
        ts->clip = what == OG_Q_STOP ? -1 : what;
        ts->queued = OG_Q_NONE;
        ts->pos = 0.0;
        return;
    }
    int q = k_launch_q[g->launch_q];
    ts->queued = what;
    ts->queue_at = q ? ceil(g->gpos / q - 1e-9) * q : g->gpos;
}

void og_clip_launch(og_t *g, int track, int clip) {
    if (!g || !track_ok(track) || !clip_ok(clip)) return;
    queue(g, track, clip);
}
void og_track_stop(og_t *g, int track) {
    if (!g || !track_ok(track)) return;
    if (g->ts[track].clip < 0 && g->ts[track].queued == OG_Q_NONE) return;
    queue(g, track, OG_Q_STOP);
}
void og_scene_launch(og_t *g, int scene) {
    if (!g || !clip_ok(scene)) return;
    for (int t = 0; t < OG_TRACKS; t++) {
        if (g->clips[t][scene].used) og_clip_launch(g, t, scene);
        else og_track_stop(g, t);
    }
}
void og_stop_all(og_t *g) {
    if (!g) return;
    for (int t = 0; t < OG_TRACKS; t++) og_track_stop(g, t);
}

int og_clip_state(const og_t *g, int track, int clip) {
    if (!g || !track_ok(track) || !clip_ok(clip)) return OG_CLIP_EMPTY;
    const og_trackseq_t *ts = &g->ts[track];
    if (ts->clip == clip) return ts->queued == OG_Q_STOP || (ts->queued >= 0 && ts->queued != clip)
                                 ? OG_CLIP_STOPPING : OG_CLIP_PLAYING;
    if (ts->queued == clip) return OG_CLIP_QUEUED;
    return g->clips[track][clip].used ? OG_CLIP_STOPPED : OG_CLIP_EMPTY;
}
int og_playing_clip(const og_t *g, int track) { return (g && track_ok(track)) ? g->ts[track].clip : -1; }
void og_set_edit_clip(og_t *g, int track, int clip) {
    if (g && track_ok(track) && clip_ok(clip)) g->ts[track].edit = clip;
}
int og_get_edit_clip(const og_t *g, int track) { return (g && track_ok(track)) ? g->ts[track].edit : 0; }

void og_clip_copy(og_t *g, int st, int sc, int dt, int dc) {
    if (!g || !track_ok(st) || !track_ok(dt) || !clip_ok(sc) || !clip_ok(dc)) return;
    if (st == dt && sc == dc) return;
    g->clips[dt][dc] = g->clips[st][sc];
}
void og_clip_clear(og_t *g, int track, int clip) {
    if (!g || !track_ok(track) || !clip_ok(clip)) return;
    og__clip_reset(&g->clips[track][clip]);
}
int og_clip_length(const og_t *g, int t) { return (g && track_ok(t)) ? cur_clip_c(g, t)->length : 16; }
int og_clip_speed(const og_t *g, int t)  { return (g && track_ok(t)) ? cur_clip_c(g, t)->speed : OG_SPEED_1X; }
int og_clip_swing(const og_t *g, int t)  { return (g && track_ok(t)) ? cur_clip_c(g, t)->swing : 50; }

/* ---- test log -------------------------------------------------------- */

static void log_ev(og_t *g, int track, int note, int vel, int on) {
    if (!g->log_on || g->log_n >= OG_LOG_MAX) return;
    og_event_t *e = &g->log[g->log_n++];
    e->t = g->sample_clock;
    e->track = track; e->note = note; e->vel = vel; e->on = on;
}

void og_debug_log(og_t *g, int enable) { if (g) { g->log_on = enable; g->log_n = 0; } }
int og_debug_events(const og_t *g, const og_event_t **out) {
    if (!g) return 0;
    if (out) *out = g->log;
    return g->log_n;
}

/* ---- playback -------------------------------------------------------- */

static void off_schedule(og_t *g, int track, int note, double at) {
    og_noteoff_t *slot = NULL, *oldest = NULL;
    for (int i = 0; i < OG_NOTEOFFS; i++) {
        og_noteoff_t *o = &g->offs[i];
        if (!o->used) { if (!slot) slot = o; continue; }
        if (!oldest || o->at < oldest->at) oldest = o;
    }
    if (!slot) {           /* queue full: release the earliest now */
        og_note_off(g, oldest->track, oldest->note);
        log_ev(g, oldest->track, oldest->note, 0, 0);
        slot = oldest;
    }
    slot->used = 1;
    slot->track = track;
    slot->note = note;
    slot->at = at;
}

/* A new note replaces any pending release of the same note on that track,
 * so an old long note can't cut the new one short. */
static void off_cancel(og_t *g, int track, int note) {
    for (int i = 0; i < OG_NOTEOFFS; i++) {
        og_noteoff_t *o = &g->offs[i];
        if (o->used && o->track == track && o->note == note) o->used = 0;
    }
}

static int cond_eval(og_t *g, int track, long n, const og_clip_t *c, int cond) {
    og_trackseq_t *ts = &g->ts[track];
    long loop = n / c->length;
    int ok;
    switch (cond) {
    case OG_COND_NONE:   return 1;
    case OG_COND_PRE:    return ts->pre;
    case OG_COND_NPRE:   return !ts->pre;
    case OG_COND_FILL:   ok = g->fill; break;
    case OG_COND_NFILL:  ok = !g->fill; break;
    case OG_COND_FIRST:  ok = loop == 0; break;
    case OG_COND_NFIRST: ok = loop > 0; break;
    default:
        if (cond >= OG_COND_PROB0) {
            int pct = (cond - OG_COND_PROB0 + 1) * 5;
            ok = (int)(rnd(g) % 100u) < pct;
        } else {
            int idx = cond - OG_COND_AB0, b = 2, a = 1;
            while (idx >= b && b < 8) { idx -= b; b++; }
            a = idx + 1;
            ok = (loop % b) == (a - 1);
        }
        break;
    }
    ts->pre = ok;
    return ok;
}

static void fire(og_t *g, int track, long n, int s, int j, double at) {
    og_trackseq_t *ts = &g->ts[track];
    og_clip_t *c = play_clip(g, track);
    og_step_t *st = &c->steps[s];

    /* the condition is decided once per step, ratchets follow it */
    if (j == 0) ts->last_ok = cond_eval(g, track, n, c, st->cond);
    if (!ts->last_ok) return;

    ts->cur_step = s;
    if (!og__track_audible(g, track)) return;

    if (st->flags & OG_STEP_NOTE) {
        int rat = st->rat < 1 ? 1 : st->rat;
        double len = og_len_steps(st->len);
        if (rat > 1 && len > 0.75 / rat) len = 0.75 / rat;
        int vel = st->vel - (j * st->vel) / (2 * rat);
        if (vel < 1) vel = 1;
        for (int k = 0; k < OG_CHORD; k++) {
            int note = st->notes[k];
            if (!note) continue;
            off_cancel(g, track, note);
            log_ev(g, track, note, vel, 1);
            og__note_on_locked(g, track, note, vel, c->locks, s,
                               (st->flags & OG_STEP_SLIDE) ? 1 : 0);
            off_schedule(g, track, note, at + len);
        }
    } else if (st->flags & OG_STEP_LOCK) {
        og__apply_locks_to_track(g, track, c->locks, s);
    }
}

void og__seq_advance(og_t *g, int frames) {
    double step_samples = (double)g->sr * 60.0 / (double)g->bpm / 4.0;
    if (!g->playing) {
        /* a count-in that ends inside this tick starts playback right here */
        if (og__countin_advance(g, frames, step_samples) || !g->playing) {
            g->sample_clock += (uint64_t)frames;
            return;
        }
    }
    double gs = g->gpos, ge = gs + (double)frames / step_samples;
    og__click_beats(g, gs, ge);

    for (int t = 0; t < OG_TRACKS; t++) {
        og_trackseq_t *ts = &g->ts[t];

        /* a queued launch or stop lands inside this tick */
        if (ts->queued != OG_Q_NONE && ts->queue_at < ge) {
            double b = ts->queue_at > gs ? ts->queue_at : gs;
            if (ts->queued == OG_Q_STOP) {
                offs_flush(g, t);
                ts->clip = -1;
            } else {
                /* keep pending releases in step with the restarted position */
                double old_at_b = ts->clip >= 0
                    ? ts->pos + (b - gs) * og_speed_mul(play_clip(g, t)->speed) : 0.0;
                for (int i = 0; i < OG_NOTEOFFS; i++)
                    if (g->offs[i].used && g->offs[i].track == t) g->offs[i].at -= old_at_b;
                ts->clip = ts->queued;
                ts->pos = (gs - b) * og_speed_mul(play_clip(g, t)->speed);
                ts->pre = 0;
                ts->cur_step = -1;
            }
            ts->queued = OG_Q_NONE;
        }
        if (ts->clip < 0) {
            /* stopped track: still let earlier notes end */
            for (int i = 0; i < OG_NOTEOFFS; i++)
                if (g->offs[i].used && g->offs[i].track == t) {
                    og_note_off(g, t, g->offs[i].note);
                    log_ev(g, t, g->offs[i].note, 0, 0);
                    g->offs[i].used = 0;
                }
            continue;
        }

        og_clip_t *c = play_clip(g, t);
        double start = ts->pos;
        double end = start + (double)frames / step_samples * og_speed_mul(c->speed);
        og__rec_cross(g, t, start, end);
        if (g->rec_grow == t && end >= (double)OG_STEPS) g->rec_grow = -1;   /* full 4 bars: done growing */

        for (int i = 0; i < OG_NOTEOFFS; i++) {
            og_noteoff_t *o = &g->offs[i];
            if (o->used && o->track == t && o->at < end) {
                og_note_off(g, t, o->note);
                log_ev(g, t, o->note, 0, 0);
                o->used = 0;
            }
        }

        double swing = 2.0 * (double)c->swing / 100.0 - 1.0;
        long n0 = (long)floor(start) - 1, n1 = (long)floor(end) + 1;
        if (n0 < 0) n0 = 0;
        for (long n = n0; n <= n1; n++) {
            int s = (int)(n % c->length);
            og_step_t *st = &c->steps[s];
            if (!(st->flags & (OG_STEP_NOTE | OG_STEP_LOCK))) continue;
            double base = (double)n + ((n & 1) ? swing : 0.0) + (double)st->micro / 48.0;
            int rat = st->rat < 1 ? 1 : st->rat;
            for (int j = 0; j < rat; j++) {
                double at = base + (double)j / (double)rat;
                if (at >= start && at < end) fire(g, t, n, s, j, at);
            }
        }
        ts->pos = end;
    }
    g->gpos = ge;
    g->sample_clock += (uint64_t)frames;
}
