/* OpGrid milestone 5 tests: clip launching, quantize, scenes, stops,
 * edit-clip independence, copy/clear, gapless switching, plugin protocol. */
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

static double SL;   /* samples per step at 120 bpm */
static void run(og_t *g, double samples) {
    float buf[256];
    long n = (long)samples;
    while (n > 0) { int k = n > 128 ? 128 : (int)n; og_render_float(g, buf, k); n -= k; }
}
static og_t *fresh(void) {
    og_t *g = og_create((float)SR);
    og_set_tempo(g, 120.0f);
    og_debug_log(g, 1);
    return g;
}
/* put a note at step s of clip c on track t (via the edit clip) */
static void put(og_t *g, int t, int c, int s, int note) {
    og_set_edit_clip(g, t, c);
    og_step_set(g, t, s, OG_F_FLAGS, OG_STEP_NOTE);
    og_step_set(g, t, s, OG_F_NOTE0, note);
}
static int collect(og_t *g, int track, int note, double *t, int max) {
    const og_event_t *e; int n = og_debug_events(g, &e), k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (e[i].on && e[i].track == track && (note < 0 || e[i].note == note)) t[k++] = (double)e[i].t;
    return k;
}

static void test_states_and_edit(void) {
    og_t *g = fresh();
    CHECK(og_playing_clip(g, 0) == 0 && og_clip_state(g, 0, 0) == OG_CLIP_EMPTY, "clip 1 plays by default but shows empty until edited");
    CHECK(og_clip_state(g, 0, 3) == OG_CLIP_EMPTY, "empty slot");
    put(g, 0, 3, 0, 70);
    CHECK(og_clip_state(g, 0, 3) == OG_CLIP_STOPPED, "edited slot has content");
    CHECK(og_get_edit_clip(g, 0) == 3, "edit clip follows");
    CHECK(og_step_get(g, 0, 0, OG_F_NOTE0) == 70, "edit clip read back");
    og_set_edit_clip(g, 0, 0);
    CHECK(og_step_get(g, 0, 0, OG_F_FLAGS) == 0, "clip 1 untouched by edits to clip 4");
    /* launching while stopped switches immediately */
    og_clip_launch(g, 0, 3);
    CHECK(og_clip_state(g, 0, 3) == OG_CLIP_PLAYING && og_playing_clip(g, 0) == 3, "launch while stopped");
    og_play(g, 1);
    run(g, SL * 2);
    double t[8];
    CHECK(collect(g, 0, 70, t, 8) == 1 && t[0] < TOL, "launched clip plays from the top");
    og_destroy(g);
}

static void test_quantized_launch(void) {
    og_t *g = fresh();
    for (int s = 0; s < 16; s++) put(g, 0, 0, s, 60);      /* clip 1: every step */
    put(g, 0, 1, 0, 72);                                   /* clip 2: step 1 only */
    og_set_edit_clip(g, 0, 0);
    og_play(g, 1);
    run(g, SL * 5.3);
    og_clip_launch(g, 0, 1);                              /* default quantize: 1 bar */
    CHECK(og_clip_state(g, 0, 1) == OG_CLIP_QUEUED, "queued (%d)", og_clip_state(g, 0, 1));
    CHECK(og_clip_state(g, 0, 0) == OG_CLIP_STOPPING, "old clip marked as ending");
    run(g, SL * 12);
    double t[64];
    int n = collect(g, 0, 72, t, 64);
    CHECK(n == 1 && fabs(t[0] - 16 * SL) <= TOL, "new clip starts on the bar: %.0f want %.0f", n ? t[0] : -1, 16 * SL);
    n = collect(g, 0, 60, t, 64);
    CHECK(n == 16, "old clip plays every step up to the bar, none after: %d", n);
    if (n) CHECK(fabs(t[n - 1] - 15 * SL) <= TOL, "last old step at 15: %.0f", t[n - 1]);
    CHECK(og_clip_state(g, 0, 1) == OG_CLIP_PLAYING, "now playing");

    /* each quantize setting */
    const double q[OG_LAUNCH_QS] = { 0, 1, 4, 16, 32, 64 };
    for (int qi = 1; qi < OG_LAUNCH_QS; qi++) {
        og_t *h = fresh();
        put(h, 0, 1, 0, 72);
        og_set_launch_quant(h, qi);
        og_play(h, 1);
        run(h, SL * 0.4);
        og_clip_launch(h, 0, 1);
        run(h, SL * (q[qi] + 2));
        n = collect(h, 0, 72, t, 64);
        CHECK(n >= 1 && fabs(t[0] - q[qi] * SL) <= TOL, "quantize %d: start %.0f want %.0f", qi, n ? t[0] : -1, q[qi] * SL);
        og_destroy(h);
    }
    og_destroy(g);
}

static void test_scenes_and_stop(void) {
    og_t *g = fresh();
    put(g, 0, 2, 0, 60);
    put(g, 1, 2, 0, 62);
    put(g, 2, 0, 0, 64);            /* track 3 plays clip 1; has nothing in scene 3 */
    og_set_launch_quant(g, 3);
    og_play(g, 1);
    run(g, SL * 3);
    og_scene_launch(g, 2);
    CHECK(og_clip_state(g, 2, 0) == OG_CLIP_STOPPING, "empty scene slot stops the track");
    run(g, SL * 14);
    CHECK(og_playing_clip(g, 0) == 2 && og_playing_clip(g, 1) == 2 && og_playing_clip(g, 2) == -1,
          "scene: %d %d %d", og_playing_clip(g, 0), og_playing_clip(g, 1), og_playing_clip(g, 2));
    double t[16];
    CHECK(collect(g, 2, 64, t, 16) == 1, "stopped track plays no more notes");
    og_stop_all(g);
    run(g, SL * 17);
    int any = 0;
    for (int tr = 0; tr < OG_TRACKS; tr++) any |= og_playing_clip(g, tr) >= 0;
    CHECK(!any, "stop all");
    run(g, SR * 3);
    CHECK(og_active_voices(g) == 0, "stopped tracks release their notes (%d voices)", og_active_voices(g));
    og_destroy(g);
}

static void test_copy_clear_and_first(void) {
    og_t *g = fresh();
    put(g, 3, 0, 5, 66);
    og_lock_set(g, 3, 5, P_FREQ, 22);
    og_clip_copy(g, 3, 0, 4, 6);
    og_set_edit_clip(g, 4, 6);
    CHECK(og_step_get(g, 4, 5, OG_F_NOTE0) == 66 && og_lock_get(g, 4, 5, P_FREQ) == 22, "copy brings steps and locks");
    CHECK(og_clip_state(g, 4, 6) == OG_CLIP_STOPPED, "copied slot shows content");
    og_clip_clear(g, 4, 6);
    CHECK(og_clip_state(g, 4, 6) == OG_CLIP_EMPTY && og_lock_get(g, 4, 5, P_FREQ) == -1, "clear empties slot");

    /* FIRST condition restarts when a clip is relaunched */
    og_t *h = fresh();
    put(h, 0, 0, 0, 60);
    og_step_set(h, 0, 0, OG_F_COND, OG_COND_FIRST);
    og_set_launch_quant(h, 3);
    og_play(h, 1);
    run(h, SL * 40);
    og_clip_launch(h, 0, 0);
    run(h, SL * 20);
    double t[16];
    int n = collect(h, 0, 60, t, 16);
    CHECK(n == 2, "1ST fires once per launch: %d", n);
    og_destroy(g); og_destroy(h);
}

/* Bar-to-bar switching between two clips with a note on every step must
 * produce a perfectly even grid: no gap, no double. */
static void test_gapless(void) {
    og_t *g = fresh();
    for (int s = 0; s < 16; s++) { put(g, 0, 0, s, 60); put(g, 0, 1, s, 61); }
    og_play(g, 1);
    for (int bar = 0; bar < 8; bar++) {
        run(g, SL * 8);
        og_clip_launch(g, 0, (bar + 1) & 1);
        run(g, SL * 8);
    }
    double t[256];
    int n = collect(g, 0, -1, t, 256);
    while (n > 0 && t[n - 1] > 128 * SL - 64) n--;   /* next bar's first note may land on the last tick */
    CHECK(n == 128, "128 notes over 8 bars, got %d", n);
    double worst = 0;
    for (int i = 1; i < n; i++) { double d = fabs((t[i] - t[i - 1]) - SL); if (d > worst) worst = d; }
    CHECK(worst <= TOL, "even grid across clip switches (worst error %.1f samples)", worst);
    og_destroy(g);
}

static void test_plugin(void) {
    extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);
    host_api_v1_t host; memset(&host, 0, sizeof(host));
    host.sample_rate = (int)SR; host.frames_per_block = 128;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *in = api->create_instance("/tmp", NULL);
    char buf[256];
    api->set_param(in, "edit", "2 5");
    api->set_param(in, "stog", "2 0");
    api->get_param(in, "edits", buf, sizeof(buf));
    CHECK(strlen(buf) == 16 && buf[2] == '5', "edits: %s", buf);
    api->get_param(in, "clips", buf, sizeof(buf));
    CHECK(strlen(buf) == 128 && buf[2 * 8 + 5] == '0' + OG_CLIP_STOPPED && buf[0] == '0' + OG_CLIP_EMPTY, "clips: %.24s", buf);
    api->set_param(in, "launch", "2 5");
    api->get_param(in, "clips", buf, sizeof(buf));
    CHECK(buf[2 * 8 + 5] == '0' + OG_CLIP_PLAYING, "launch while stopped");
    api->set_param(in, "lq", "4");
    api->get_param(in, "lq", buf, sizeof(buf));
    CHECK(!strcmp(buf, "4"), "lq");
    api->set_param(in, "ccopy", "2 5 9 1");
    api->set_param(in, "scene", "1");
    api->get_param(in, "clips", buf, sizeof(buf));
    CHECK(buf[9 * 8 + 1] == '0' + OG_CLIP_PLAYING && buf[0] == '0' + OG_CLIP_EMPTY, "scene 2 while stopped: %.16s / %.8s", buf, buf + 72);
    api->set_param(in, "cclr", "9 1");
    api->set_param(in, "stopall", "1");
    api->destroy_instance(in);
}

int main(void) {
    SL = SR * 60.0 / 120.0 / 4.0;
    test_states_and_edit();
    test_quantized_launch();
    test_scenes_and_stop();
    test_copy_clear_and_first();
    test_gapless();
    test_plugin();
    if (failures) { printf("%d clip test(s) FAILED\n", failures); return 1; }
    printf("all clip/scene tests passed\n");
    return 0;
}
