/* OpGrid milestone 6 engine tests: recording (quantized, unquantized,
 * chords, lengths, overdub/replace, knob locks), undo, new growing clips,
 * count-in and click, reset, clip dump/load, plugin protocol. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api_v1.h"
#include "og_engine.h"

#define SR 44100.0
#define TOL 16
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double SL;
static double energy_run(og_t *g, double samples) {
    float buf[256]; double e = 0;
    long n = (long)samples;
    while (n > 0) {
        int k = n > 128 ? 128 : (int)n;
        og_render_float(g, buf, k);
        for (int i = 0; i < 2 * k; i++) e += (double)buf[i] * buf[i];
        n -= k;
    }
    return e;
}
static void run(og_t *g, double samples) { energy_run(g, samples); }
static og_t *fresh(void) {
    og_t *g = og_create((float)SR);
    og_set_tempo(g, 120.0f);
    og_set_click(g, OG_CLICK_OFF);
    return g;
}

static void test_record_notes(void) {
    og_t *g = fresh();
    og_play(g, 1);
    og_rec_set_track(g, 0);
    og_rec_arm(g, 1);
    run(g, SL * 4.3);
    og_live_note_on(g, 0, 64, 90);
    og_live_note_on(g, 0, 67, 90);              /* chord on the same step */
    run(g, SL * 2.0);
    og_live_note_off(g, 0, 64);
    og_live_note_off(g, 0, 67);
    CHECK(og_step_get(g, 0, 4, OG_F_FLAGS) & OG_STEP_NOTE, "quantized note lands on step 5");
    CHECK(og_step_get(g, 0, 4, OG_F_NOTE0) == 64 && og_step_get(g, 0, 4, OG_F_NOTE1) == 67, "chord recorded (%d %d)",
          og_step_get(g, 0, 4, OG_F_NOTE0), og_step_get(g, 0, 4, OG_F_NOTE1));
    CHECK(og_step_get(g, 0, 4, OG_F_MICRO) == 0, "quantized: no micro-timing");
    CHECK(fabs(og_len_steps(og_step_get(g, 0, 4, OG_F_LEN)) - 2.0) < 0.01, "length ~2 steps (%.3f)",
          og_len_steps(og_step_get(g, 0, 4, OG_F_LEN)));

    /* unquantized: keeps the offset as micro-timing */
    og_rec_set_quant(g, 0);
    run(g, SL * (9.3 - 6.3));                    /* now at 9.3 */
    og_live_note_on(g, 0, 70, 100);
    run(g, SL * 0.5);
    og_live_note_off(g, 0, 70);
    int micro = og_step_get(g, 0, 9, OG_F_MICRO);
    CHECK(og_step_get(g, 0, 9, OG_F_NOTE0) == 70 && micro >= 12 && micro <= 16, "unquantized: step 10 micro %d", micro);

    /* knob move while recording becomes a lock on the current step */
    run(g, SL * (12.0 - 9.8));
    og_rec_param(g, 0, P_FREQ, 33);
    CHECK(og_lock_get(g, 0, 12, P_FREQ) == 33, "knob recorded as lock on step 13 (%d)", og_lock_get(g, 0, 12, P_FREQ));
    CHECK(og_step_get(g, 0, 12, OG_F_FLAGS) & OG_STEP_LOCK, "empty step became lock-only");
    CHECK(og_get_param(g, 0, P_FREQ) == og_param_default(P_FREQ), "base value untouched while recording");

    /* the recorded notes play back on the next loop */
    og_rec_arm(g, 0);
    og_debug_log(g, 1);
    run(g, SL * 20);
    const og_event_t *e; int n = og_debug_events(g, &e), saw64 = 0, saw70 = 0;
    for (int i = 0; i < n; i++) if (e[i].on) { saw64 |= e[i].note == 64; saw70 |= e[i].note == 70; }
    CHECK(saw64 && saw70, "recorded notes play back");

    /* not recording: knob changes the base */
    og_rec_param(g, 0, P_FREQ, 44);
    CHECK(og_get_param(g, 0, P_FREQ) == 44, "rec_param sets base when not recording");
    og_destroy(g);
}

static void test_overdub_replace_undo(void) {
    og_t *g = fresh();
    for (int s = 0; s < 16; s += 2) og_step_set(g, 0, s, OG_F_FLAGS, OG_STEP_NOTE);
    og_play(g, 1);
    og_rec_set_track(g, 0);
    og_rec_arm(g, 1);                            /* overdub */
    run(g, SL * 3.0);
    og_live_note_on(g, 0, 72, 100); run(g, SL * 0.4); og_live_note_off(g, 0, 72);
    run(g, SL * 13);
    int count = 0;
    for (int s = 0; s < 16; s++) count += (og_step_get(g, 0, s, OG_F_FLAGS) & OG_STEP_NOTE) != 0;
    CHECK(count == 9, "overdub keeps the 8 old steps + 1 new (%d)", count);
    CHECK(og_undo(g), "undo available");
    count = 0;
    for (int s = 0; s < 16; s++) count += (og_step_get(g, 0, s, OG_F_FLAGS) & OG_STEP_NOTE) != 0;
    CHECK(count == 8 && !(og_step_get(g, 0, 3, OG_F_FLAGS) & OG_STEP_NOTE), "undo restores the take (%d)", count);
    CHECK(!og_undo(g), "only one level of undo");

    og_rec_arm(g, 0);
    og_rec_set_mode(g, 1);                       /* replace */
    og_rec_arm(g, 1);
    run(g, SL * 0.2);                            /* now just past a loop start? position is arbitrary */
    run(g, SL * 3.0);
    og_live_note_on(g, 0, 74, 100); run(g, SL * 0.4); og_live_note_off(g, 0, 74);
    run(g, SL * 15);                             /* finish this pass, stop before the step comes round again */
    count = 0;
    int kept = -1;
    for (int s = 0; s < 16; s++) if (og_step_get(g, 0, s, OG_F_FLAGS) & OG_STEP_NOTE) { count++; kept = s; }
    CHECK(count == 1 && og_step_get(g, 0, kept, OG_F_NOTE0) == 74, "replace leaves only the new note (%d steps)", count);
    og_destroy(g);
}

static void test_new_clip(void) {
    og_t *g = fresh();
    og_play(g, 1);
    run(g, SL * 2);
    og_rec_new_clip(g, 1, 3);                    /* launches on the next bar */
    CHECK(og_rec_growing(g) == 1 && og_rec_armed(g), "growing + armed");
    run(g, SL * 14 + SL * 0.1);                  /* clip starts at step 16 */
    CHECK(og_playing_clip(g, 1) == 3, "new clip launched");
    run(g, SL * 3);
    og_live_note_on(g, 1, 60, 100); run(g, SL * 0.5); og_live_note_off(g, 1, 60);
    run(g, SL * (20.5 - 3.5));                   /* ~20.6 steps into the clip */
    og_live_note_on(g, 1, 62, 100); run(g, SL * 0.5); og_live_note_off(g, 1, 62);
    run(g, SL * 3);
    og_rec_close_clip(g, 1);                     /* ~24 steps in -> 2 bars */
    og_set_edit_clip(g, 1, 3);
    CHECK(og_clip_length(g, 1) == 32, "closed at 2 bars (%d)", og_clip_length(g, 1));
    CHECK(og_rec_growing(g) == -1, "no longer growing");
    CHECK(og_step_get(g, 1, 3, OG_F_NOTE0) == 60 && og_step_get(g, 1, 21, OG_F_NOTE0) == 62,
          "both notes in the new clip (%d %d)", og_step_get(g, 1, 3, OG_F_NOTE0), og_step_get(g, 1, 21, OG_F_NOTE0));
    CHECK(og_undo(g) && og_clip_state(g, 1, 3) == OG_CLIP_EMPTY, "undo empties the new clip");
    og_destroy(g);
}

static void test_countin_and_click(void) {
    og_t *g = fresh();
    og_set_click(g, OG_CLICK_COUNTIN);
    og_step_set(g, 0, 0, OG_F_FLAGS, OG_STEP_NOTE);
    og_debug_log(g, 1);
    og_play_countin(g);
    CHECK(og_counting_in(g) && !og_is_playing(g), "counting in");
    double e = energy_run(g, SL * 15);
    CHECK(e > 1.0, "count-in clicks are audible (%.3f)", e);
    CHECK(!og_is_playing(g), "not playing yet");
    run(g, SL * 2);
    CHECK(og_is_playing(g) && !og_counting_in(g), "playing after one bar");
    const og_event_t *ev; int n = og_debug_events(g, &ev);
    CHECK(n >= 1 && ev[0].on && fabs((double)ev[0].t - 16 * SL) <= TOL, "first note right after the count-in (%.0f want %.0f)",
          n ? (double)ev[0].t : -1.0, 16 * SL);
    og_destroy(g);

    /* click off: count-in is skipped */
    og_t *h = fresh();
    og_play_countin(h);
    CHECK(og_is_playing(h), "click off plays immediately");
    /* click on while recording */
    og_set_click(h, OG_CLICK_ON);
    og_rec_arm(h, 1);
    CHECK(energy_run(h, SL * 8) > 1.0, "metronome while recording");
    og_rec_arm(h, 0);
    run(h, SL * 4);
    CHECK(energy_run(h, SL * 8) < 1e-6, "no metronome when not recording");
    og_destroy(h);
}

static void test_dump_load_reset(void) {
    og_t *g = fresh();
    og_set_edit_clip(g, 2, 5);
    for (int s = 0; s < 64; s += 3) {
        og_step_set(g, 2, s, OG_F_FLAGS, OG_STEP_NOTE | (s % 2 ? OG_STEP_SLIDE : 0));
        og_step_set(g, 2, s, OG_F_NOTE0, 30 + s);
        og_step_set(g, 2, s, OG_F_NOTE2, s % 5 ? 0 : 90);
        og_step_set(g, 2, s, OG_F_MICRO, (s % 47) - 23);
        og_step_set(g, 2, s, OG_F_COND, s % OG_CONDS);
        og_step_set(g, 2, s, OG_F_RAT, 1 + s % 8);
    }
    for (int i = 0; i < 40; i++) og_lock_set(g, 2, i, i % P_COUNT_USED, (i * 7) % 128);
    og_clip_set_length(g, 2, 48); og_clip_set_speed(g, 2, 2); og_clip_set_swing(g, 2, 61);
    static char a[OG_CLIP_HEX_MAX + 1], b[OG_CLIP_HEX_MAX + 1];
    int na = og_clip_dump(g, 2, 5, a, sizeof(a));
    CHECK(na > 0 && na <= OG_CLIP_HEX_MAX, "dump size %d", na);
    CHECK(og_clip_load(g, 9, 1, a), "load");
    og_clip_dump(g, 9, 1, b, sizeof(b));
    CHECK(!strcmp(a, b), "round trip identical");
    CHECK(!og_clip_load(g, 9, 1, "zz"), "bad hex rejected");
    CHECK(!og_clip_load(g, 9, 1, "0102"), "short payload rejected");
    og_clip_dump(g, 9, 1, b, sizeof(b));
    CHECK(!strcmp(a, b), "rejected load leaves the clip alone");

    og_set_param(g, 3, P_FREQ, 5);
    og_set_fx(g, FX_RSIZ, 3);
    og_set_tempo(g, 90);
    og_play(g, 1);
    og_reset(g);
    CHECK(og_get_param(g, 3, P_FREQ) == og_param_default(P_FREQ) && og_get_fx(g, FX_RSIZ) == og_fx_default(FX_RSIZ),
          "reset restores defaults");
    CHECK(og_clip_state(g, 2, 5) == OG_CLIP_EMPTY && og_get_tempo(g) == 120.0f && !og_is_playing(g), "reset clears clips/tempo/transport");
    og_destroy(g);
}

static void test_plugin(void) {
    extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);
    host_api_v1_t host; memset(&host, 0, sizeof(host));
    host.sample_rate = (int)SR; host.frames_per_block = 128;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *in = api->create_instance("/tmp", NULL);
    static char buf[OG_CLIP_HEX_MAX + 64], cmd[OG_CLIP_HEX_MAX + 128];
    api->set_param(in, "edit", "4 2");
    api->set_param(in, "stog", "4 7");
    api->set_param(in, "lock", "4 7 17 99");
    int n = api->get_param(in, "cdump:4:2", buf, sizeof(buf));
    CHECK(n > 0, "cdump");
    snprintf(cmd, sizeof(cmd), "11 6 %s", buf);
    api->set_param(in, "cload", cmd);
    api->set_param(in, "edit", "11 6");
    api->get_param(in, "locks:11:7", buf, sizeof(buf));
    CHECK(!strcmp(buf, "17=99"), "cload restores locks: %s", buf);
    api->set_param(in, "rquant", "0");
    api->set_param(in, "rmode", "1");
    api->set_param(in, "click", "2");
    api->set_param(in, "rec", "1");
    api->get_param(in, "rec", buf, sizeof(buf));
    CHECK(!strcmp(buf, "1,0,-1,0,1,2"), "rec status: %s", buf);
    api->set_param(in, "reset", "1");
    api->get_param(in, "clips", buf, sizeof(buf));
    CHECK(buf[11 * 8 + 6] == '0', "reset via plugin");
    api->destroy_instance(in);
}

int main(void) {
    SL = SR * 60.0 / 120.0 / 4.0;
    test_record_notes();
    test_overdub_replace_undo();
    test_new_clip();
    test_countin_and_click();
    test_dump_load_reset();
    test_plugin();
    if (failures) { printf("%d recording test(s) FAILED\n", failures); return 1; }
    printf("all recording/project tests passed\n");
    return 0;
}
