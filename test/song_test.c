/* OpGrid milestone 7 tests: song playback (bars, repeats, end, loop),
 * per-row tempo, swing and mutes, jumps, start row, performance capture
 * and its playback, row editing, dump/load, plugin protocol. */
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

static double SL, BAR;
static void run(og_t *g, double samples) {
    float buf[256];
    long n = (long)samples;
    while (n > 0) { int k = n > 128 ? 128 : (int)n; og_render_float(g, buf, k); n -= k; }
}
/* note-ons of a track, as (bar, note) */
static int bars_of(og_t *g, int track, int *bar, int *note, int max) {
    const og_event_t *e; int n = og_debug_events(g, &e), k = 0;
    for (int i = 0; i < n && k < max; i++) if (e[i].on && e[i].track == track) {
        double b = (double)e[i].t / BAR;
        if (fabs(b - floor(b + 0.5)) * BAR > TOL) { bar[k] = -1000; note[k++] = e[i].note; continue; }
        bar[k] = (int)floor(b + 0.5); note[k++] = e[i].note;
    }
    return k;
}

/* track 0: clip c plays note 60+c on step 1; track 1: same with 70+c */
static og_t *setup(void) {
    og_t *g = og_create((float)SR);
    og_set_tempo(g, 120.0f);
    for (int t = 0; t < 2; t++)
        for (int c = 0; c < 3; c++) {
            og_set_edit_clip(g, t, c);
            og_step_set(g, t, 0, OG_F_FLAGS, OG_STEP_NOTE);
            og_step_set(g, t, 0, OG_F_NOTE0, (t ? 70 : 60) + c);
        }
    og_song_insert(g, 0, 0, 0); og_song_set(g, 0, 0, OG_R_BARS, 2);
    og_song_insert(g, 0, 1, 1); og_song_set(g, 0, 1, OG_R_BARS, 1); og_song_set(g, 0, 1, OG_R_REPS, 2);
    og_song_insert(g, 0, 2, 2); og_song_set(g, 0, 2, OG_R_BARS, 1);
    og_song_set_loop(g, 0, 0);
    og_debug_log(g, 1);
    return g;
}

static void test_playback(void) {
    og_t *g = setup();
    og_song_mode(g, 1, 0, 0);
    og_play(g, 1);
    run(g, BAR * 8 - 100);
    int bar[32], note[32];
    int n = bars_of(g, 0, bar, note, 32);
    const int want_bar[5] = {0, 1, 2, 3, 4}, want_note[5] = {60, 60, 61, 61, 62};
    CHECK(n == 5, "5 bars of music then the end (got %d)", n);
    for (int i = 0; i < n && i < 5; i++)
        CHECK(bar[i] == want_bar[i] && note[i] == want_note[i], "bar %d: note %d (want bar %d note %d)", bar[i], note[i], want_bar[i], want_note[i]);
    CHECK(!og_song_on(g), "song mode ends after the last row");
    int st[6]; og_song_status(g, st);
    CHECK(og_playing_clip(g, 0) == -1, "tracks stopped at the end");
    og_destroy(g);

    /* loop */
    g = setup();
    og_song_set_loop(g, 0, 1);
    og_song_mode(g, 1, 0, 0);
    og_play(g, 1);
    run(g, BAR * 7 - 100);
    n = bars_of(g, 0, bar, note, 32);
    CHECK(n == 7 && note[5] == 60 && note[6] == 60, "loops back to row 1 (n=%d)", n);
    og_destroy(g);

    /* start at row 2, and a jump */
    g = setup();
    og_song_mode(g, 1, 0, 1);
    og_play(g, 1);
    run(g, SL * 5);
    og_song_jump(g, 2);
    run(g, BAR * 2 - SL * 5 - 100);
    n = bars_of(g, 0, bar, note, 32);
    CHECK(n == 2 && note[0] == 61 && note[1] == 62 && bar[1] == 1, "start at row 2, jump lands on the next bar (%d: %d %d)", n, note[0], n > 1 ? note[1] : -1);
    og_destroy(g);
}

static void test_row_settings(void) {
    og_t *g = setup();
    og_song_set(g, 0, 1, OG_R_TEMPO, 150);
    og_song_set(g, 0, 1, OG_R_MUTES, 1 << 1);           /* mute track 2 in row 2 */
    og_song_mode(g, 1, 0, 0);
    og_play(g, 1);
    run(g, BAR * 2 + SL);
    CHECK(fabs(og_get_tempo(g) - 150.0f) < 0.01f, "row tempo applied (%.1f)", og_get_tempo(g));
    CHECK(og_get_mute(g, 1) && !og_get_mute(g, 0), "row mutes applied");
    og_destroy(g);

    /* swing override */
    og_t *h = og_create((float)SR);
    og_set_tempo(h, 120.0f);
    og_step_set(h, 0, 0, OG_F_FLAGS, OG_STEP_NOTE);
    og_step_set(h, 0, 1, OG_F_FLAGS, OG_STEP_NOTE);
    og_song_insert(h, 0, 0, 0);
    og_song_set(h, 0, 0, OG_R_SWING, 66);
    og_debug_log(h, 1);
    og_song_mode(h, 1, 0, 0);
    og_play(h, 1);
    run(h, SL * 2);
    const og_event_t *e; int n = og_debug_events(h, &e);
    double t1 = -1;
    for (int i = 0, k = 0; i < n; i++) if (e[i].on && k++ == 1) t1 = (double)e[i].t;
    CHECK(fabs(t1 - 1.32 * SL) <= TOL, "row swing applies to all tracks (%.0f want %.0f)", t1, 1.32 * SL);
    og_destroy(h);
}

static void test_capture(void) {
    og_t *g = setup();
    og_set_launch_quant(g, 3);
    og_clip_launch(g, 0, 0);
    og_clip_launch(g, 1, 0);
    og_track_stop(g, 1);
    og_capture(g, 1, 3);
    og_play(g, 1);
    run(g, BAR * 1.5);
    og_clip_launch(g, 0, 1);              /* lands on bar 2 */
    run(g, BAR * 1.0);
    og_track_stop(g, 0);                  /* lands on bar 3 */
    run(g, BAR * 2.0);
    og_capture(g, 0, 0);
    int len = og_song_length(g, 3);
    CHECK(len == 3, "captured 3 rows (%d)", len);
    if (len == 3) {
        CHECK(og_song_get(g, 3, 0, OG_R_CLIP0) == 0 && og_song_get(g, 3, 0, OG_R_BARS) == 2, "row 1: clip 1 for 2 bars (%d, %d)",
              og_song_get(g, 3, 0, OG_R_CLIP0), og_song_get(g, 3, 0, OG_R_BARS));
        CHECK(og_song_get(g, 3, 1, OG_R_CLIP0) == 1 && og_song_get(g, 3, 1, OG_R_BARS) == 1, "row 2: clip 2 for 1 bar");
        CHECK(og_song_get(g, 3, 2, OG_R_CLIP0) == -1 && og_song_get(g, 3, 2, OG_R_CLIP0 + 1) == -1, "row 3: stopped");
    }
    /* the captured song plays the same thing back */
    og_play(g, 0);
    og_song_set_loop(g, 3, 0);
    og_debug_log(g, 1);
    og_song_mode(g, 1, 3, 0);
    og_play(g, 1);
    run(g, BAR * 6 - 100);
    int bar[16], note[16];
    int n = bars_of(g, 0, bar, note, 16);
    CHECK(n == 3 && note[0] == 60 && note[1] == 60 && note[2] == 61, "captured song replays (%d notes)", n);
    og_destroy(g);
}

static void test_editing_and_dump(void) {
    og_t *g = setup();
    CHECK(og_song_scene_of(g, 0, 1) == 1, "row built from scene 2 reports scene 2");
    og_song_set(g, 0, 1, OG_R_CLIP0 + 1, 0);
    CHECK(og_song_scene_of(g, 0, 1) == -1, "edited row is mixed");
    int r = og_song_insert(g, 0, 3, -1);
    CHECK(r == 3 && og_song_length(g, 0) == 4 && og_song_get(g, 0, 3, OG_R_CLIP0) == 2, "insert duplicates the row before");
    og_song_delete(g, 0, 0);
    CHECK(og_song_length(g, 0) == 3 && og_song_get(g, 0, 0, OG_R_REPS) == 2, "delete shifts rows up");
    for (int i = 0; i < 80; i++) og_song_insert(g, 1, 0, 0);
    CHECK(og_song_length(g, 1) == OG_SONG_ROWS, "64 rows max");
    og_song_set(g, 0, 0, OG_R_TEMPO, 97); og_song_set(g, 0, 0, OG_R_SWING, 61); og_song_set(g, 0, 0, OG_R_MUTES, 0xA5A5);
    og_song_set_loop(g, 0, 0);
    static char a[OG_SONG_HEX_MAX + 1], b[OG_SONG_HEX_MAX + 1];
    CHECK(og_song_dump(g, 0, a, sizeof(a)) > 0, "dump");
    CHECK(og_song_load(g, 5, a), "load");
    og_song_dump(g, 5, b, sizeof(b));
    CHECK(!strcmp(a, b), "round trip");
    CHECK(og_song_dump(g, 1, a, sizeof(a)) == OG_SONG_HEX_MAX, "full song fits the max size");
    CHECK(!og_song_load(g, 5, "40"), "truncated song rejected");
    og_reset(g);
    CHECK(og_song_length(g, 0) == 0 && og_song_length(g, 5) == 0, "reset clears songs");
    og_destroy(g);
}

static void test_plugin(void) {
    extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);
    host_api_v1_t host; memset(&host, 0, sizeof(host));
    host.sample_rate = (int)SR; host.frames_per_block = 128;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *in = api->create_instance("/tmp", NULL);
    static char buf[OG_SONG_HEX_MAX + 64], cmd[OG_SONG_HEX_MAX + 128];
    api->set_param(in, "stog", "0 0");
    api->set_param(in, "sins", "2 0 0");
    api->set_param(in, "srow", "2 0 1 8");
    api->get_param(in, "srow:2:0", buf, sizeof(buf));
    CHECK(!strncmp(buf, "0,8,1,0,0,0,0,-1,-1", 19), "srow: %.40s", buf);
    api->get_param(in, "slens", buf, sizeof(buf));
    CHECK(!strcmp(buf, "0,0,1,0,0,0,0,0"), "slens: %s", buf);
    api->get_param(in, "sdump:2", buf, sizeof(buf));
    snprintf(cmd, sizeof(cmd), "6 %s", buf);
    api->set_param(in, "sload", cmd);
    api->get_param(in, "slens", buf, sizeof(buf));
    CHECK(!strcmp(buf, "0,0,1,0,0,0,1,0"), "sload: %s", buf);
    api->set_param(in, "song", "1 2 0");
    api->get_param(in, "sstat", buf, sizeof(buf));
    CHECK(!strcmp(buf, "1,2,0,0,0,0"), "sstat: %s", buf);
    api->set_param(in, "song", "0");
    api->set_param(in, "capture", "1 4");
    api->get_param(in, "sstat", buf, sizeof(buf));
    CHECK(buf[strlen(buf) - 1] == '1', "capturing: %s", buf);
    api->destroy_instance(in);
}

int main(void) {
    SL = SR * 60.0 / 120.0 / 4.0;
    BAR = SL * 16;
    test_playback();
    test_row_settings();
    test_capture();
    test_editing_and_dump();
    test_plugin();
    if (failures) { printf("%d song test(s) FAILED\n", failures); return 1; }
    printf("all song tests passed\n");
    return 0;
}
