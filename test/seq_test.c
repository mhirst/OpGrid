/* OpGrid sequencer tests: timing, swing, micro-timing, ratchets,
 * conditions, locks, chords, mutes, long-run drift, plugin protocol. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api_v1.h"
#include "og_engine.h"

#define SR 44100.0
#define TOL 16                       /* control tick resolution, samples */
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run(og_t *g, double samples) {
    float buf[2 * 128];
    long n = (long)samples;
    while (n > 0) { int k = n > 128 ? 128 : (int)n; og_render_float(g, buf, k); n -= k; }
}

static double step_len(double bpm) { return SR * 60.0 / bpm / 4.0; }

/* collect note-on times for a track */
static int ons(og_t *g, int track, double *t, int *vel, int max) {
    const og_event_t *e;
    int n = og_debug_events(g, &e), k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (e[i].on && e[i].track == track) { t[k] = (double)e[i].t; if (vel) vel[k] = e[i].vel; k++; }
    return k;
}

static og_t *fresh(void) {
    og_t *g = og_create((float)SR);
    og_set_tempo(g, 120.0f);
    og_debug_log(g, 1);
    return g;
}

static void note_step(og_t *g, int t, int s) { og_step_set(g, t, s, OG_F_FLAGS, OG_STEP_NOTE); }

static void test_basic_timing(void) {
    og_t *g = fresh();
    for (int s = 0; s < 16; s += 4) note_step(g, 0, s);
    og_play(g, 1);
    run(g, step_len(120) * 16 - 100);
    double t[16]; int n = ons(g, 0, t, NULL, 16);
    CHECK(n == 4, "expected 4 notes, got %d", n);
    for (int i = 0; i < n; i++)
        CHECK(fabs(t[i] - i * 4 * step_len(120)) <= TOL, "note %d at %.0f, want %.0f", i, t[i], i * 4 * step_len(120));
    /* note-offs: one per note, ~1 step after */
    const og_event_t *e; int ne = og_debug_events(g, &e), offs = 0;
    for (int i = 0; i < ne; i++) if (!e[i].on) offs++;
    CHECK(offs == 4, "expected 4 note-offs, got %d", offs);
    og_destroy(g);
}

static void test_length_and_speed(void) {
    og_t *g = fresh();
    note_step(g, 1, 0);
    og_clip_set_length(g, 1, 3);           /* every 3 steps */
    note_step(g, 2, 0);
    og_clip_set_speed(g, 2, 6);            /* 2x: every 8 steps of track 2 = 8 real steps? no: 16 steps at 2x */
    og_clip_set_length(g, 2, 16);
    og_play(g, 1);
    run(g, step_len(120) * 12 - 100);
    double t[16]; int n = ons(g, 1, t, NULL, 16);
    CHECK(n == 4, "len 3: expected 4 notes in 12 steps, got %d", n);
    for (int i = 0; i < n; i++) CHECK(fabs(t[i] - i * 3 * step_len(120)) <= TOL, "len3 note %d", i);
    n = ons(g, 2, t, NULL, 16);
    CHECK(n == 2, "2x speed: expected 2 loops in 12 steps (8 each), got %d", n);
    if (n == 2) CHECK(fabs(t[1] - 8 * step_len(120)) <= TOL, "2x loop at %.0f", t[1]);
    og_destroy(g);
}

static void test_swing_and_micro(void) {
    og_t *g = fresh();
    note_step(g, 0, 0); note_step(g, 0, 1);
    og_clip_set_swing(g, 0, 66);
    note_step(g, 1, 4);
    og_step_set(g, 1, 4, OG_F_MICRO, 12);
    note_step(g, 2, 4);
    og_step_set(g, 2, 4, OG_F_MICRO, -12);
    og_play(g, 1);
    run(g, step_len(120) * 6);
    double t[8];
    int n = ons(g, 0, t, NULL, 8);
    CHECK(n == 2 && fabs(t[1] - 1.32 * step_len(120)) <= TOL, "swing 66%%: second step at %.0f want %.0f", t[1], 1.32 * step_len(120));
    n = ons(g, 1, t, NULL, 8);
    CHECK(n == 1 && fabs(t[0] - 4.25 * step_len(120)) <= TOL, "micro +12: %.0f", t[0]);
    n = ons(g, 2, t, NULL, 8);
    CHECK(n == 1 && fabs(t[0] - 3.75 * step_len(120)) <= TOL, "micro -12: %.0f", t[0]);
    og_destroy(g);
}

static void test_ratchet(void) {
    og_t *g = fresh();
    note_step(g, 0, 2);
    og_step_set(g, 0, 2, OG_F_RAT, 4);
    og_step_set(g, 0, 2, OG_F_VEL, 120);
    og_play(g, 1);
    run(g, step_len(120) * 4);
    double t[8]; int v[8];
    int n = ons(g, 0, t, v, 8);
    CHECK(n == 4, "ratchet 4: got %d", n);
    for (int i = 0; i < n; i++) {
        CHECK(fabs(t[i] - (2 + i * 0.25) * step_len(120)) <= TOL, "ratchet %d time %.0f", i, t[i]);
        if (i) CHECK(v[i] < v[i - 1], "ratchet velocity fades (%d !< %d)", v[i], v[i - 1]);
    }
    og_destroy(g);
}

static int count_loops(int cond, int loops, int fill) {
    og_t *g = fresh();
    note_step(g, 0, 0);
    og_step_set(g, 0, 0, OG_F_COND, cond);
    og_set_fill(g, fill);
    og_play(g, 1);
    run(g, step_len(120) * 16 * loops - 100);
    double t[1024]; int n = ons(g, 0, t, NULL, 1024);
    og_destroy(g);
    return n;
}

/* cond id for A:B */
static int ab(int a, int b) { int id = OG_COND_AB0; for (int bb = 2; bb < b; bb++) id += bb; return id + a - 1; }

static void test_conditions(void) {
    CHECK(count_loops(OG_COND_NONE, 8, 0) == 8, "none");
    CHECK(count_loops(ab(1, 2), 8, 0) == 4, "1:2 -> %d", count_loops(ab(1, 2), 8, 0));
    CHECK(count_loops(ab(2, 2), 8, 0) == 4, "2:2");
    CHECK(count_loops(ab(3, 4), 8, 0) == 2, "3:4");
    CHECK(count_loops(ab(8, 8), 16, 0) == 2, "8:8");
    CHECK(ab(8, 8) == OG_COND_PROB0 - 1, "A:B table ends right before probability (%d)", ab(8, 8));
    CHECK(count_loops(OG_COND_FIRST, 8, 0) == 1, "first");
    CHECK(count_loops(OG_COND_NFIRST, 8, 0) == 7, "not first");
    CHECK(count_loops(OG_COND_FILL, 8, 0) == 0, "fill off");
    CHECK(count_loops(OG_COND_FILL, 8, 1) == 8, "fill on");
    CHECK(count_loops(OG_COND_NFILL, 8, 1) == 0, "not fill with fill on");
    int p50 = count_loops(OG_COND_PROB0 + 9, 400, 0);   /* 50% */
    CHECK(p50 > 150 && p50 < 250, "50%% probability gave %d/400", p50);
    int p5 = count_loops(OG_COND_PROB0, 400, 0);
    CHECK(p5 < 50, "5%% probability gave %d/400", p5);

    /* PRE follows the previous conditional step on the same track */
    og_t *g = fresh();
    note_step(g, 0, 0); og_step_set(g, 0, 0, OG_F_COND, ab(1, 2));
    note_step(g, 0, 1); og_step_set(g, 0, 1, OG_F_NOTE0, 62); og_step_set(g, 0, 1, OG_F_COND, OG_COND_PRE);
    note_step(g, 0, 2); og_step_set(g, 0, 2, OG_F_NOTE0, 64); og_step_set(g, 0, 2, OG_F_COND, OG_COND_NPRE);
    og_play(g, 1);
    run(g, step_len(120) * 16 * 4 - 100);
    const og_event_t *e; int ne = og_debug_events(g, &e), c60 = 0, c62 = 0, c64 = 0;
    for (int i = 0; i < ne; i++) if (e[i].on) { c60 += e[i].note == 60; c62 += e[i].note == 62; c64 += e[i].note == 64; }
    CHECK(c60 == 2 && c62 == 2 && c64 == 2, "pre/!pre: %d %d %d", c60, c62, c64);
    og_destroy(g);
}

static void test_locks(void) {
    og_t *g = fresh();
    note_step(g, 0, 0);
    note_step(g, 0, 4);
    og_step_set(g, 0, 0, OG_F_LEN, 12);           /* long notes so voices stay up */
    og_step_set(g, 0, 4, OG_F_LEN, 12);
    og_step_set(g, 0, 4, OG_F_NOTE0, 67);
    CHECK(og_lock_set(g, 0, 0, P_FREQ, 10), "lock set");
    CHECK(og_lock_get(g, 0, 0, P_FREQ) == 10, "lock get");
    og_play(g, 1);
    run(g, step_len(120) * 1);
    CHECK(og_debug_voice_param(g, 0, P_FREQ) == 10, "step 0 voice uses lock (%d)", og_debug_voice_param(g, 0, P_FREQ));
    CHECK(og_get_param(g, 0, P_FREQ) == og_param_default(P_FREQ), "base untouched");
    og_set_param(g, 0, P_FREQ, 50);
    og_set_param(g, 0, P_RESO, 99);
    CHECK(og_debug_voice_param(g, 0, P_FREQ) == 10, "base edit must not override a lock");
    CHECK(og_debug_voice_param(g, 0, P_RESO) == 99, "base edit reaches unlocked param");
    run(g, step_len(120) * 4);
    CHECK(og_debug_voice_param(g, 0, P_FREQ) == 50, "step 4 voice uses base (%d)", og_debug_voice_param(g, 0, P_FREQ));

    /* lock-only step changes the sounding voice */
    og_step_set(g, 0, 8, OG_F_FLAGS, OG_STEP_LOCK);
    og_lock_set(g, 0, 8, P_RESO, 7);
    run(g, step_len(120) * 4);
    CHECK(og_debug_voice_param(g, 0, P_RESO) == 7, "lock-only step applied (%d)", og_debug_voice_param(g, 0, P_RESO));

    /* clearing a step removes its locks; pool limit */
    og_step_clear(g, 0, 0);
    CHECK(og_lock_get(g, 0, 0, P_FREQ) == -1, "locks cleared with step");
    int ok = 1;
    for (int i = 0; i < OG_LOCKS + 5; i++) ok = og_lock_set(g, 1, i % OG_STEPS, (i / OG_STEPS) % P_COUNT_USED, 1);
    CHECK(!ok && og_lock_count(g, 1) == OG_LOCKS, "pool full after %d locks", og_lock_count(g, 1));
    og_destroy(g);
}

static void test_chord_mute_long_notes(void) {
    og_t *g = fresh();
    note_step(g, 0, 0);
    og_step_set(g, 0, 0, OG_F_NOTE1, 64);
    og_step_set(g, 0, 0, OG_F_NOTE2, 67);
    note_step(g, 1, 0);
    og_set_mute(g, 1, 1);
    /* track 2: 8-step notes every 4 steps, same pitch: must not be cut short */
    note_step(g, 2, 0); og_step_set(g, 2, 0, OG_F_LEN, 10);
    note_step(g, 2, 4); og_step_set(g, 2, 4, OG_F_LEN, 10);
    og_play(g, 1);
    run(g, step_len(120) * 16 - 100);
    double t[8];
    CHECK(ons(g, 0, t, NULL, 8) == 3, "chord plays 3 notes");
    CHECK(ons(g, 1, t, NULL, 8) == 0, "muted track silent");
    const og_event_t *e; int ne = og_debug_events(g, &e);
    double off2 = -1;
    for (int i = 0; i < ne; i++) if (!e[i].on && e[i].track == 2) off2 = (double)e[i].t;
    CHECK(fabs(off2 - 12 * step_len(120)) <= TOL, "track 2 releases once at step 12, got %.0f", off2);
    og_play(g, 0);
    run(g, SR * 6);
    CHECK(og_active_voices(g) == 0, "stop releases everything (%d voices)", og_active_voices(g));
    og_destroy(g);
}

/* Ten minutes of a 16-track pattern: no drift, no stuck voices. */
static void test_long_run(void) {
    og_t *g = fresh();
    for (int t = 0; t < 16; t++) {
        og_set_param(g, t, P_ADEC, 30);
        og_set_param(g, t, P_ASUS, 0);
        og_set_param(g, t, P_AREL, 20);
        for (int s = t % 4; s < 16; s += 4) {
            note_step(g, t, s);
            og_step_set(g, t, s, OG_F_NOTE0, 36 + t * 3);
        }
        og_clip_set_length(g, t, 16 - (t % 3));   /* some polymeters */
    }
    og_debug_log(g, 0);
    og_play(g, 1);
    double total = SR * 600.0;
    run(g, total - SR);
    og_debug_log(g, 1);
    run(g, SR);
    double t[64]; int n = ons(g, 0, t, NULL, 64);
    CHECK(n > 0, "track 0 still playing after 10 minutes");
    double sl = step_len(120);
    double nearest = round(t[0] / sl) * sl;
    CHECK(fabs(t[0] - nearest) <= TOL, "drift after 10 min: %.1f samples off the grid", t[0] - nearest);
    CHECK(og_active_voices(g) <= OG_VOICES, "voice count sane");
    og_play(g, 0);
    run(g, SR * 3);
    CHECK(og_active_voices(g) == 0, "no stuck voices after 10 min (%d)", og_active_voices(g));
    og_destroy(g);
}

static void test_plugin_protocol(void) {
    extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);
    host_api_v1_t host; memset(&host, 0, sizeof(host));
    host.sample_rate = 44100; host.frames_per_block = 128;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *in = api->create_instance("/tmp", NULL);
    char buf[512];
    api->set_param(in, "stog", "5 3");
    api->set_param(in, "sf", "5 3 9 -7");        /* micro */
    api->set_param(in, "lock", "5 3 16 33");
    api->get_param(in, "steps:5", buf, sizeof(buf));
    CHECK(strlen(buf) == 64 && buf[3] == '0' + (OG_STEP_NOTE | 8) && buf[0] == '0', "steps string: %.8s", buf);
    api->get_param(in, "step:5:3", buf, sizeof(buf));
    CHECK(!strcmp(buf, "1,60,0,0,0,100,4,1,0,-7"), "step fields: %s", buf);
    api->get_param(in, "locks:5:3", buf, sizeof(buf));
    CHECK(!strcmp(buf, "16=33"), "locks: %s", buf);
    api->set_param(in, "clen", "5 12");
    api->set_param(in, "cswg", "5 60");
    api->get_param(in, "clip:5", buf, sizeof(buf));
    CHECK(!strcmp(buf, "12,4,60,1,0"), "clip: %s", buf);
    api->set_param(in, "tempo", "97");
    api->get_param(in, "tempo", buf, sizeof(buf));
    CHECK(!strcmp(buf, "97.0"), "tempo: %s", buf);
    api->get_param(in, "sync", buf, sizeof(buf));
    CHECK(!strcmp(buf, "0"), "setting tempo turns sync off");
    api->set_param(in, "play", "1");
    int16_t out[256];
    for (int i = 0; i < 200; i++) api->render_block(in, out, 128);
    api->get_param(in, "pos", buf, sizeof(buf));
    CHECK(buf[0] != '-', "playheads move: %s", buf);
    api->set_param(in, "stog", "5 3");
    api->get_param(in, "steps:5", buf, sizeof(buf));
    CHECK(buf[3] == '0' + 8, "toggle off keeps locks marker: %c", buf[3]);
    api->set_param(in, "sclr", "5 3");
    api->get_param(in, "steps:5", buf, sizeof(buf));
    CHECK(buf[3] == '0', "cleared step: %c", buf[3]);
    api->destroy_instance(in);
}

/* A sequenced demo: kick-ish FM drum, bass with locks + slide, swung chords,
 * ratchets and a 1:2 condition. Written to build/seq_demo.wav. */
static void demo(const char *path) {
    og_t *g = og_create((float)SR);
    og_set_tempo(g, 122.0f);
    /* t0 kick: pitched sine with fast pitch-like mod decay */
    og_set_param(g, 0, P_ALGO, 0); og_set_param(g, 0, P_RAT2, 0); og_set_param(g, 0, P_LVL2, 90);
    og_set_param(g, 0, P_DEC2, 30); og_set_param(g, 0, P_ADEC, 45); og_set_param(g, 0, P_ASUS, 0);
    og_set_param(g, 0, P_AREL, 30); og_set_param(g, 0, P_FREQ, 70); og_set_param(g, 0, P_VOL, 120);
    for (int s = 0; s < 16; s += 4) { og_step_set(g, 0, s, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 0, s, OG_F_NOTE0, 36); }
    /* t1 hat: high ratio noise-ish FM, ratchet on step 14, 1:2 on step 7 */
    og_set_param(g, 1, P_ALGO, 0); og_set_param(g, 1, P_RAT1, 17); og_set_param(g, 1, P_RAT2, 21);
    og_set_param(g, 1, P_LVL2, 127); og_set_param(g, 1, P_FDBK, 127); og_set_param(g, 1, P_LVL4, 127);
    og_set_param(g, 1, P_FTYP, OG_FILTER_HP); og_set_param(g, 1, P_FREQ, 105);
    og_set_param(g, 1, P_ADEC, 25); og_set_param(g, 1, P_ASUS, 0); og_set_param(g, 1, P_AREL, 15);
    og_set_param(g, 1, P_VOL, 70); og_set_param(g, 1, P_PAN, 80);
    for (int s = 2; s < 16; s += 4) { og_step_set(g, 1, s, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 1, s, OG_F_NOTE0, 72); }
    og_step_set(g, 1, 7, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 1, 7, OG_F_NOTE0, 72); og_step_set(g, 1, 7, OG_F_COND, ab(1, 2));
    og_step_set(g, 1, 15, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 1, 15, OG_F_NOTE0, 72); og_step_set(g, 1, 15, OG_F_RAT, 3);
    og_clip_set_swing(g, 1, 60);
    /* t2 bass: locks open the filter on some steps, slide into step 10 */
    og_set_param(g, 2, P_ALGO, 0); og_set_param(g, 2, P_LVL2, 70); og_set_param(g, 2, P_DEC2, 50);
    og_set_param(g, 2, P_FTYP, OG_FILTER_LP24); og_set_param(g, 2, P_FREQ, 55); og_set_param(g, 2, P_RESO, 70);
    og_set_param(g, 2, P_FENV, 90); og_set_param(g, 2, P_FDEC, 45); og_set_param(g, 2, P_ASUS, 80);
    const int bass[16] = {36,0,0,36, 0,0,48,0, 36,0,39,0, 0,43,0,46};
    for (int s = 0; s < 16; s++) if (bass[s]) {
        og_step_set(g, 2, s, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 2, s, OG_F_NOTE0, bass[s]);
        og_step_set(g, 2, s, OG_F_LEN, 3);
    }
    og_lock_set(g, 2, 6, P_FREQ, 95); og_lock_set(g, 2, 13, P_FREQ, 85); og_lock_set(g, 2, 15, P_RESO, 115);
    og_step_set(g, 2, 10, OG_F_FLAGS, OG_STEP_NOTE | OG_STEP_SLIDE);
    /* t3 chords, 32-step clip at half speed */
    og_set_param(g, 3, P_ALGO, 4); og_set_param(g, 3, P_RAT2, 13); og_set_param(g, 3, P_LVL2, 40);
    og_set_param(g, 3, P_LVL3, 80); og_set_param(g, 3, P_AATK, 40); og_set_param(g, 3, P_AREL, 80);
    og_set_param(g, 3, P_DTUN, 40); og_set_param(g, 3, P_VOL, 75); og_set_param(g, 3, P_PAN, 45);
    og_clip_set_speed(g, 3, 2);
    og_step_set(g, 3, 0, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 3, 0, OG_F_NOTE0, 60);
    og_step_set(g, 3, 0, OG_F_NOTE1, 63); og_step_set(g, 3, 0, OG_F_NOTE2, 67); og_step_set(g, 3, 0, OG_F_LEN, 9);
    og_step_set(g, 3, 8, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 3, 8, OG_F_NOTE0, 58);
    og_step_set(g, 3, 8, OG_F_NOTE1, 62); og_step_set(g, 3, 8, OG_F_NOTE2, 65); og_step_set(g, 3, 8, OG_F_LEN, 9);

    /* milestone 4: sends, an LFO sweeping the chords' filter, a slow one on bass resonance */
    og_set_param(g, 1, P_SDLY, 60);
    og_set_param(g, 2, P_SREV, 30);
    og_set_param(g, 3, P_SREV, 90); og_set_param(g, 3, P_SDLY, 50); og_set_param(g, 3, P_SCHO, 100);
    og_set_param(g, 3, P_FTYP, OG_FILTER_LP24); og_set_param(g, 3, P_FREQ, 70);
    og_set_param(g, 3, P_L1DEST, OG_DEST_PARAM0 + P_FREQ); og_set_param(g, 3, P_L1WAVE, OG_WAVE_TRI);
    og_set_param(g, 3, P_L1RATE, 6 * 128 / OG_LFO_SYNC_RATES + 1); og_set_param(g, 3, P_L1DPTH, 100);
    og_set_param(g, 2, P_L1DEST, OG_DEST_PARAM0 + P_RESO); og_set_param(g, 2, P_L1RATE, 2 * 128 / OG_LFO_SYNC_RATES + 1);
    og_set_param(g, 2, P_L1DPTH, 90);
    og_set_fx(g, FX_MTHR, 90); og_set_fx(g, FX_MRAT, 40); og_set_fx(g, FX_MGAIN, 25);
    og_play(g, 1);
    long frames = (long)(step_len(122) * 16 * 8);
    float *out = calloc((size_t)frames * 2, sizeof(float));
    for (long i = 0; i < frames; i += 128) og_render_float(g, out + i * 2, (int)(frames - i < 128 ? frames - i : 128));
    FILE *f = fopen(path, "wb");
    if (f) {
        int sr = 44100; unsigned data = (unsigned)(frames * 4), riff = 36 + data, br = (unsigned)sr * 4, fmt = 16;
        unsigned short pcm = 1, ch = 2, ba = 4, bps = 16;
        fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
        fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
        fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
        for (long i = 0; i < frames * 2; i++) { float x = out[i] * 32767.0f; short v = (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); fwrite(&v, 2, 1, f); }
        fclose(f);
        printf("seq demo: wrote %s (%.1f s)\n", path, frames / SR);
    }
    free(out);
    og_destroy(g);
}

int main(int argc, char **argv) {
    test_basic_timing();
    test_length_and_speed();
    test_swing_and_micro();
    test_ratchet();
    test_conditions();
    test_locks();
    test_chord_mute_long_notes();
    test_plugin_protocol();
    test_long_run();
    if (argc > 1) demo(argv[1]);
    if (failures) { printf("%d sequencer test(s) FAILED\n", failures); return 1; }
    printf("all sequencer tests passed\n");
    return 0;
}
