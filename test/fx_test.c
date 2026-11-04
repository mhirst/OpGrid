/* OpGrid milestone 4 tests: delay, reverb, chorus, master bus, LFOs,
 * solo, MIDI out, stability at extreme settings, plugin protocol. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api_v1.h"
#include "og_engine.h"

#define SR 44100
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float *render(og_t *g, int frames) {
    float *out = calloc((size_t)frames * 2, sizeof(float));
    for (int i = 0; i < frames; i += 128) og_render_float(g, out + i * 2, frames - i < 128 ? frames - i : 128);
    return out;
}
static double energy(const float *b, int from, int to) {
    double e = 0;
    for (int i = from; i < to; i++) e += (double)b[2 * i] * b[2 * i] + (double)b[2 * i + 1] * b[2 * i + 1];
    return e;
}
static int finite_bounded(const float *b, int frames) {
    for (int i = 0; i < 2 * frames; i++) if (!(b[i] == b[i]) || fabsf(b[i]) > 1.0f) return 0;
    return 1;
}

/* a short click-like note on track 0 */
static og_t *clicker(void) {
    og_t *g = og_create(SR);
    og_set_tempo(g, 120.0f);
    og_set_param(g, 0, P_ADEC, 20);
    og_set_param(g, 0, P_ASUS, 0);
    og_set_param(g, 0, P_AREL, 10);
    og_note_on(g, 0, 60, 127);
    return g;
}

static void test_delay(void) {
    int delay = (int)(3 * SR * 60.0 / 120.0 / 4.0);          /* default 1/8 dotted = 3 steps */
    og_t *a = clicker();
    float *dry = render(a, delay * 3);
    og_t *b = clicker();
    og_set_param(b, 0, P_SDLY, 127);
    float *wet = render(b, delay * 3);
    double e_dry = energy(dry, delay - 100, delay + 3000);
    double e_wet = energy(wet, delay - 100, delay + 3000);
    CHECK(e_wet > 50 * (e_dry + 1e-9), "echo at the delay time (wet %.3g dry %.3g)", e_wet, e_dry);
    double before = energy(wet, delay / 2, delay - 400);
    CHECK(before < e_wet * 0.05, "no echo before the delay time (%.3g vs %.3g)", before, e_wet);
    CHECK(energy(wet, 2 * delay - 100, 2 * delay + 3000) > 0.01 * e_wet, "repeats via feedback");
    free(dry); free(wet); og_destroy(a); og_destroy(b);
}

static void test_reverb_and_chorus(void) {
    og_t *a = clicker();
    float *dry = render(a, SR * 2);
    og_t *b = clicker();
    og_set_param(b, 0, P_SREV, 127);
    float *rev = render(b, SR * 2);
    double tail_dry = energy(dry, SR / 2, SR), tail_rev = energy(rev, SR / 2, SR);
    CHECK(tail_rev > 100 * (tail_dry + 1e-12), "reverb tail (%.3g vs %.3g)", tail_rev, tail_dry);
    /* longer decay -> more tail */
    og_t *c = clicker();
    og_set_param(c, 0, P_SREV, 127);
    og_set_fx(c, FX_RDEC, 127);
    float *rev2 = render(c, SR * 2);
    CHECK(energy(rev2, SR, 2 * SR) > energy(rev, SR, 2 * SR) * 2, "decay control lengthens the tail");
    /* chorus changes the signal */
    og_t *d = og_create(SR);
    og_set_param(d, 0, P_SCHO, 127);
    og_note_on(d, 0, 60, 100);
    float *ch = render(d, SR / 2);
    og_t *e = og_create(SR);
    og_note_on(e, 0, 60, 100);
    float *nc = render(e, SR / 2);
    double diff = 0;
    for (int i = 0; i < SR; i++) diff += fabs(ch[i] - nc[i]);
    CHECK(diff > 10.0, "chorus audible (diff %.2f)", diff);
    free(dry); free(rev); free(rev2); free(ch); free(nc);
    og_destroy(a); og_destroy(b); og_destroy(c); og_destroy(d); og_destroy(e);
}

static void test_master(void) {
    og_t *g = og_create(SR);
    for (int n = 0; n < 6; n++) og_note_on(g, n, 48 + n * 4, 127);
    float *b = render(g, SR / 4);
    CHECK(og_debug_comp_gain(g) > 0.999f, "compressor off by default (gain %.3f)", og_debug_comp_gain(g));
    og_set_fx(g, FX_MTHR, 20);
    og_set_fx(g, FX_MRAT, 127);
    free(b); b = render(g, SR / 4);
    CHECK(og_debug_comp_gain(g) < 0.7f, "compressor reduces gain (%.3f)", og_debug_comp_gain(g));
    og_set_fx(g, FX_MVOL, 0);
    free(b); b = render(g, SR / 10);
    CHECK(energy(b, SR / 20, SR / 10) < 1e-9, "master volume 0 is silent");
    free(b); og_destroy(g);
}

static void test_lfo(void) {
    /* square LFO on track volume, 1 cycle per 2 steps, synced: loud/quiet halves */
    og_t *g = og_create(SR);
    og_set_tempo(g, 120.0f);
    og_set_param(g, 0, P_ASUS, 127);
    og_set_param(g, 0, P_L1DEST, OG_DEST_PARAM0 + P_VOL);
    og_set_param(g, 0, P_L1WAVE, OG_WAVE_SQR);
    og_set_param(g, 0, P_L1SYNC, 1);
    og_set_param(g, 0, P_L1RATE, 12 * 128 / OG_LFO_SYNC_RATES + 1);   /* index 12 = 2 steps */
    og_set_param(g, 0, P_L1DPTH, 0);                                    /* full negative depth */
    og_set_param(g, 0, P_L1TRIG, 1);
    og_note_on(g, 0, 60, 100);
    int step = (int)(SR * 60.0 / 120.0 / 4.0);
    float *b = render(g, step * 4);
    double e1 = energy(b, step / 8, step - step / 8);            /* first half-cycle */
    double e2 = energy(b, step + step / 8, 2 * step - step / 8); /* second half-cycle */
    CHECK(e1 < e2 * 0.2 || e2 < e1 * 0.2, "square LFO on volume alternates (%.3g / %.3g)", e1, e2);
    free(b); og_destroy(g);

    /* every wave, sync/free, every destination: finite and bounded */
    for (int w = 0; w < OG_WAVES; w++) {
        og_t *h = og_create(SR);
        for (int i = 0; i < OG_LFOS; i++) {
            og_set_param(h, 0, P_LFO(i, LFO_WAVE), w);
            og_set_param(h, 0, P_LFO(i, LFO_SYNC), i & 1);
            og_set_param(h, 0, P_LFO(i, LFO_RATE), 100);
            og_set_param(h, 0, P_LFO(i, LFO_DPTH), i == 1 ? 0 : 127);
            og_set_param(h, 0, P_LFO(i, LFO_FADE), i == 2 ? 60 : 0);
        }
        for (int d = 0; d < OG_DESTS; d++) {
            og_set_param(h, 0, P_L1DEST, d);
            og_set_param(h, 0, P_L2DEST, (d + 7) % OG_DESTS);
            og_set_param(h, 0, P_L3DEST, (d + 19) % OG_DESTS);
            og_note_on(h, 0, 50 + d % 20, 110);
            float *x = render(h, 2048);
            CHECK(finite_bounded(x, 2048), "wave %d dest %d produced NaN or clipping", w, d);
            free(x);
            og_note_off(h, 0, 50 + d % 20);
        }
        og_destroy(h);
    }
}

static void test_extremes(void) {
    og_t *g = og_create(SR);
    for (int i = 0; i < FX_COUNT; i++) og_set_fx(g, i, og_fx_max(i));
    og_set_fx(g, FX_MTHR, 127);
    for (int t = 0; t < 4; t++) {
        og_set_param(g, t, P_SDLY, 127); og_set_param(g, t, P_SREV, 127); og_set_param(g, t, P_SCHO, 127);
        og_set_param(g, t, P_FDBK, 127); og_set_param(g, t, P_RESO, 127); og_set_param(g, t, P_DRIV, 127);
        og_note_on(g, t, 40 + t * 7, 127);
    }
    float *b = render(g, SR * 20);
    CHECK(finite_bounded(b, SR * 20), "20 s at maximum feedback / decay stays finite and bounded");
    og_all_notes_off(g);
    free(b);
    b = render(g, SR * 2);
    CHECK(finite_bounded(b, SR * 2), "tail after panic finite");
    free(b);
    og_destroy(g);
}

static int msgs[64][3], nmsg = 0;
static void capture(void *ctx, uint8_t st, uint8_t d1, uint8_t d2) {
    (void)ctx;
    if (nmsg < 64) { msgs[nmsg][0] = st; msgs[nmsg][1] = d1; msgs[nmsg][2] = d2; nmsg++; }
}

static void test_midi_out_and_solo(void) {
    og_t *g = og_create(SR);
    og_set_midi_out(g, capture, NULL);
    og_set_param(g, 2, P_MCH, 3);
    og_set_param(g, 2, P_MOUT, OG_MOUT_EXT);
    nmsg = 0;
    og_note_on(g, 2, 64, 99);
    CHECK(nmsg == 1 && msgs[0][0] == 0x92 && msgs[0][1] == 64 && msgs[0][2] == 99, "note on out (n=%d st=%x)", nmsg, nmsg ? msgs[0][0] : 0);
    CHECK(og_active_voices(g) == 0, "EXT mode makes no internal sound");
    og_note_off(g, 2, 64);
    CHECK(nmsg == 2 && msgs[1][0] == 0x82 && msgs[1][1] == 64, "note off out");
    og_set_param(g, 2, P_MOUT, OG_MOUT_BOTH);
    og_note_on(g, 2, 65, 80);
    CHECK(nmsg == 3 && og_active_voices(g) == 1, "BOTH mode: MIDI + voice");
    og_set_param(g, 2, P_MOUT, OG_MOUT_INT);
    og_note_on(g, 2, 66, 80);
    CHECK(nmsg == 3, "INT mode sends nothing");
    og_set_param(g, 2, P_MOUT, OG_MOUT_BOTH);
    og_all_notes_off(g);
    CHECK(nmsg == 4 && msgs[3][0] == 0xB2 && msgs[3][1] == 123, "panic sends all-notes-off CC");

    /* sequencer drives MIDI out too */
    nmsg = 0;
    og_step_set(g, 2, 0, OG_F_FLAGS, OG_STEP_NOTE);
    og_play(g, 1);
    float *b = render(g, 2000);
    free(b);
    CHECK(nmsg >= 1 && msgs[0][0] == 0x92 && msgs[0][1] == 60, "sequenced note goes out over MIDI");
    og_play(g, 0);
    og_destroy(g);

    /* solo: only soloed tracks play */
    og_t *s = og_create(SR);
    og_debug_log(s, 1);
    og_step_set(s, 0, 0, OG_F_FLAGS, OG_STEP_NOTE);
    og_step_set(s, 1, 0, OG_F_FLAGS, OG_STEP_NOTE);
    og_set_solo(s, 1, 1);
    og_play(s, 1);
    b = render(s, 4000); free(b);
    const og_event_t *e; int n = og_debug_events(s, &e), t0 = 0, t1 = 0;
    for (int i = 0; i < n; i++) if (e[i].on) { t0 += e[i].track == 0; t1 += e[i].track == 1; }
    CHECK(t0 == 0 && t1 == 1, "solo: track0 %d track1 %d", t0, t1);
    og_destroy(s);
}

static void test_plugin(void) {
    extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);
    host_api_v1_t host; memset(&host, 0, sizeof(host));
    host.sample_rate = SR; host.frames_per_block = 128;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *in = api->create_instance("/tmp", NULL);
    char buf[256];
    api->set_param(in, "fx", "4 99");
    api->get_param(in, "fx", buf, sizeof(buf));
    int vals[32], n = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) vals[n++] = atoi(t);
    CHECK(n == FX_COUNT && vals[FX_RSIZ] == 99, "fx list (%d values)", n);
    api->set_param(in, "solo", "7 1");
    api->set_param(in, "mute", "3 1");
    api->get_param(in, "mix", buf, sizeof(buf));
    CHECK(strlen(buf) == 32 && buf[3] == '1' && buf[16 + 7] == '1' && buf[0] == '0', "mix: %s", buf);
    api->get_param(in, "tp:0", buf, sizeof(buf));
    n = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) n++;
    CHECK(n == P_COUNT_USED, "tp has all %d params (%d)", P_COUNT_USED, n);
    api->destroy_instance(in);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--dump-fx")) {
        for (int i = 0; i < FX_COUNT; i++) printf("%d %d\n", og_fx_default(i), og_fx_max(i));
        return 0;
    }
    test_delay();
    test_reverb_and_chorus();
    test_master();
    test_lfo();
    test_extremes();
    test_midi_out_and_solo();
    test_plugin();
    if (failures) { printf("%d fx test(s) FAILED\n", failures); return 1; }
    printf("all fx/lfo/mixer tests passed\n");
    return 0;
}
