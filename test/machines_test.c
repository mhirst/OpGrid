/* OpGrid milestone 8 tests: FM Perc, Morph and Stack machines — every
 * parameter at its extremes across the keyboard stays finite and bounded,
 * machine switching and per-machine ranges, percussive decay, loading
 * order, CPU per machine, and a demo render. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "plugin_api_v1.h"
#include "og_engine.h"

#define SR 44100
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *NAMES[OG_MACHINES] = {"FM Poly", "FM Perc", "Morph", "Stack"};

typedef struct { float peak; double rms; int bad; } st_t;
static st_t render(og_t *g, int frames) {
    st_t s = {0, 0, 0};
    float buf[256];
    double e = 0; long n = 0;
    while (frames > 0) {
        int k = frames > 128 ? 128 : frames;
        og_render_float(g, buf, k);
        for (int i = 0; i < 2 * k; i++) {
            float x = buf[i];
            if (!(x == x) || isinf(x)) s.bad++;
            if (fabsf(x) > s.peak) s.peak = fabsf(x);
            e += (double)x * x; n++;
        }
        frames -= k;
    }
    s.rms = n ? sqrt(e / n) : 0;
    return s;
}

static void test_switch_and_ranges(void) {
    og_t *g = og_create(SR);
    for (int m = 1; m < OG_MACHINES; m++) {
        og_set_machine(g, 0, m);
        CHECK(og_get_param(g, 0, P_MACH) == m, "%s selected", NAMES[m]);
        for (int pid = 0; pid < OG_MACH_PARAMS; pid++) {
            CHECK(og_get_param(g, 0, pid) == og_machine_default(m, pid), "%s pid %d default", NAMES[m], pid);
            og_set_param(g, 0, pid, 999);
            CHECK(og_get_param(g, 0, pid) == og_machine_max(m, pid), "%s pid %d clamps to its own max", NAMES[m], pid);
        }
    }
    og_set_machine(g, 1, OG_MACH_PERC);
    CHECK(og_get_param(g, 1, P_ASUS) == 0, "Perc gets a percussive amp");
    og_set_machine(g, 1, OG_MACH_POLY);
    CHECK(og_get_param(g, 1, P_ALGO) == og_param_default(P_ALGO), "back to FM Poly defaults");
    og_destroy(g);

    /* loading a Morph track: the machine must apply before its params */
    extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);
    host_api_v1_t host; memset(&host, 0, sizeof(host)); host.sample_rate = SR; host.frames_per_block = 128;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *in = api->create_instance("/tmp", NULL);
    char cmd[1024], buf[1024];
    int w = snprintf(cmd, sizeof(cmd), "3 ");
    for (int p = 0; p < P_COUNT_USED; p++)
        w += snprintf(cmd + w, sizeof(cmd) - (size_t)w, p ? ",%d" : "%d", p == 0 ? 100 : p == P_MACH ? OG_MACH_MORPH : og_param_default(p));
    api->set_param(in, "tpset", cmd);
    api->get_param(in, "tp:3", buf, sizeof(buf));
    CHECK(atoi(buf) == 100, "Morph WAVA 100 survives loading (got %d)", atoi(buf));
    api->set_param(in, "mach", "4 1");
    api->get_param(in, "tp:4", buf, sizeof(buf));
    int vals[64], k = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) vals[k++] = atoi(t);
    CHECK(vals[P_MACH] == OG_MACH_PERC && vals[P_ASUS] == 0, "mach command");
    api->destroy_instance(in);
}

/* every machine param at 0, mid and max, notes across the keyboard */
static void test_extremes(void) {
    const int notes[] = {12, 24, 48, 72, 96, 120};
    for (int m = 1; m < OG_MACHINES; m++) {
        int worst_bad = 0; float worst_peak = 0; double min_rms = 1e9;
        for (int pid = 0; pid < OG_MACH_PARAMS; pid++) {
            int max = og_machine_max(m, pid);
            if (max == 0) continue;
            const int vals[3] = {0, max / 2, max};
            for (int vi = 0; vi < 3; vi++) {
                og_t *g = og_create(SR);
                og_set_machine(g, 0, m);
                og_set_param(g, 0, P_ASUS, 100);
                og_set_param(g, 0, pid, vals[vi]);
                for (int i = 0; i < 6; i++) og_note_on(g, 0, notes[i], 127);
                st_t s = render(g, 4096);
                worst_bad += s.bad;
                if (s.peak > worst_peak) worst_peak = s.peak;
                if (s.rms < min_rms) min_rms = s.rms;
                og_destroy(g);
            }
        }
        CHECK(worst_bad == 0, "%s: NaN/inf at extreme settings", NAMES[m]);
        CHECK(worst_peak <= 1.0f, "%s: output over full scale (%.3f)", NAMES[m], worst_peak);
        CHECK(min_rms > 1e-4, "%s: some setting is silent (rms %.6f)", NAMES[m], min_rms);
    }
}

static void test_character(void) {
    /* Perc decays by itself (no note off) */
    og_t *g = og_create(SR);
    og_set_machine(g, 0, OG_MACH_PERC);
    og_note_on(g, 0, 36, 127);
    st_t a = render(g, SR / 20);
    render(g, SR);
    st_t b = render(g, SR / 20);
    CHECK(a.rms > 0.01 && b.rms < a.rms * 0.01, "Perc decays without a note off (%.4f -> %.6f)", a.rms, b.rms);
    og_destroy(g);

    /* Stack: more copies -> a wider (different) sound, still similar level */
    og_t *s1 = og_create(SR), *s8 = og_create(SR);
    og_set_machine(s1, 0, OG_MACH_STACK); og_set_param(s1, 0, 1, 0);
    og_set_machine(s8, 0, OG_MACH_STACK); og_set_param(s8, 0, 1, 7);
    og_note_on(s1, 0, 48, 100); og_note_on(s8, 0, 48, 100);
    st_t r1 = render(s1, SR / 4), r8 = render(s8, SR / 4);
    CHECK(r8.rms > r1.rms * 0.5 && r8.rms < r1.rms * 2.0, "Stack level stays similar with 8 copies (%.3f vs %.3f)", r8.rms, r1.rms);
    og_destroy(s1); og_destroy(s8);

    /* Morph: wave position changes the sound */
    og_t *m0 = og_create(SR), *m1 = og_create(SR);
    og_set_machine(m0, 0, OG_MACH_MORPH); og_set_machine(m1, 0, OG_MACH_MORPH);
    og_set_param(m1, 0, 0, 127);
    og_note_on(m0, 0, 60, 100); og_note_on(m1, 0, 60, 100);
    float b0[2048], b1[2048];
    og_render_float(m0, b0, 1024); og_render_float(m1, b1, 1024);
    double diff = 0;
    for (int i = 0; i < 2048; i++) diff += fabs(b0[i] - b1[i]);
    CHECK(diff > 20.0, "Morph WAVA changes the waveform (diff %.1f)", diff);
    og_destroy(m0); og_destroy(m1);
}

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; }

static void bench(void) {
    for (int m = 0; m < OG_MACHINES; m++) {
        og_t *g = og_create(SR);
        for (int t = 0; t < OG_TRACKS; t++) {
            og_set_machine(g, t, m);
            og_set_param(g, t, P_ASUS, 127);
            og_set_param(g, t, P_FTYP, OG_FILTER_LP24);
            if (m == OG_MACH_STACK) og_set_param(g, t, 1, 7);             /* 8 copies: worst case */
            if (m == OG_MACH_MORPH) { og_set_param(g, t, 3, 80); og_set_param(g, t, 5, 60); og_set_param(g, t, 8, 60); }
            if (m == OG_MACH_PERC) { og_set_param(g, t, 5, 100); og_set_param(g, t, P_ASUS, 127); }
        }
        for (int i = 0; i < OG_VOICES; i++) og_note_on(g, i, 40 + i * 3, 100);
        float buf[256];
        for (int i = 0; i < 50; i++) og_render_float(g, buf, 128);
        double total = 0;
        for (int i = 0; i < 2000; i++) { double t0 = now_us(); og_render_float(g, buf, 128); total += now_us() - t0; }
        printf("bench %-8s 12 voices: avg %.1f us/block (this machine)\n", NAMES[m], total / 2000);
        og_destroy(g);
    }
}

static void demo(const char *path) {
    og_t *g = og_create(SR);
    og_set_tempo(g, 124.0f);
    /* t0 kick (Perc) */
    og_set_machine(g, 0, OG_MACH_PERC);
    og_set_param(g, 0, 0, 70); og_set_param(g, 0, 1, 40); og_set_param(g, 0, 3, 20); og_set_param(g, 0, P_VOL, 120);
    for (int s = 0; s < 16; s += 4) { og_step_set(g, 0, s, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 0, s, OG_F_NOTE0, 36); }
    /* t1 snare (Perc: noise + body) */
    og_set_machine(g, 1, OG_MACH_PERC);
    og_set_param(g, 1, 0, 30); og_set_param(g, 1, 5, 110); og_set_param(g, 1, 6, 50); og_set_param(g, 1, 7, 90);
    og_set_param(g, 1, 9, 80); og_set_param(g, 1, P_VOL, 95); og_set_param(g, 1, P_SREV, 50);
    for (int s = 4; s < 16; s += 8) { og_step_set(g, 1, s, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 1, s, OG_F_NOTE0, 50); }
    /* t2 hats (Perc, high noise, closed) */
    og_set_machine(g, 2, OG_MACH_PERC);
    og_set_param(g, 2, 0, 0); og_set_param(g, 2, 5, 127); og_set_param(g, 2, 6, 15); og_set_param(g, 2, 7, 120);
    og_set_param(g, 2, 3, 0); og_set_param(g, 2, P_ADEC, 35); og_set_param(g, 2, P_VOL, 70); og_set_param(g, 2, P_PAN, 84);
    for (int s = 2; s < 16; s += 2) { og_step_set(g, 2, s, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 2, s, OG_F_NOTE0, 80); }
    og_clip_set_swing(g, 2, 58);
    /* t3 bass (Morph, pd + sub) */
    og_set_machine(g, 3, OG_MACH_MORPH);
    og_set_param(g, 3, 0, 36); og_set_param(g, 3, 3, 70); og_set_param(g, 3, 8, 80);
    og_set_param(g, 3, P_FTYP, OG_FILTER_LP24); og_set_param(g, 3, P_FREQ, 60); og_set_param(g, 3, P_FENV, 95);
    og_set_param(g, 3, P_FDEC, 45); og_set_param(g, 3, P_RESO, 60); og_set_param(g, 3, P_ASUS, 70);
    const int bass[16] = {36,0,0,36, 0,0,39,0, 36,0,0,43, 0,41,0,0};
    for (int s = 0; s < 16; s++) if (bass[s]) {
        og_step_set(g, 3, s, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 3, s, OG_F_NOTE0, bass[s]); og_step_set(g, 3, s, OG_F_LEN, 3);
    }
    og_lock_set(g, 3, 6, 0, 100);                               /* wave lock: brighter on step 7 */
    /* t4 pad (Stack, 7 copies, slow filter LFO) */
    og_set_machine(g, 4, OG_MACH_STACK);
    og_set_param(g, 4, 1, 6); og_set_param(g, 4, 2, 50); og_set_param(g, 4, P_AATK, 60); og_set_param(g, 4, P_AREL, 85);
    og_set_param(g, 4, P_ASUS, 110); og_set_param(g, 4, P_FREQ, 75); og_set_param(g, 4, P_VOL, 70);
    og_set_param(g, 4, P_SREV, 80); og_set_param(g, 4, P_SCHO, 60);
    og_set_param(g, 4, P_L1DEST, OG_DEST_PARAM0 + P_FREQ); og_set_param(g, 4, P_L1RATE, 2 * 128 / OG_LFO_SYNC_RATES + 1);
    og_set_param(g, 4, P_L1DPTH, 100);
    og_clip_set_speed(g, 4, 2);
    og_step_set(g, 4, 0, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 4, 0, OG_F_NOTE0, 60);
    og_step_set(g, 4, 0, OG_F_NOTE1, 63); og_step_set(g, 4, 0, OG_F_NOTE2, 67); og_step_set(g, 4, 0, OG_F_LEN, 10);
    og_step_set(g, 4, 8, OG_F_FLAGS, OG_STEP_NOTE); og_step_set(g, 4, 8, OG_F_NOTE0, 58);
    og_step_set(g, 4, 8, OG_F_NOTE1, 62); og_step_set(g, 4, 8, OG_F_NOTE2, 65); og_step_set(g, 4, 8, OG_F_LEN, 10);
    og_set_fx(g, FX_MTHR, 95); og_set_fx(g, FX_MRAT, 40); og_set_fx(g, FX_MGAIN, 20);

    og_play(g, 1);
    long frames = (long)(SR * 60.0 / 124.0 / 4.0 * 16 * 8);
    float *out = calloc((size_t)frames * 2, sizeof(float));
    for (long i = 0; i < frames; i += 128) og_render_float(g, out + i * 2, (int)(frames - i < 128 ? frames - i : 128));
    FILE *f = fopen(path, "wb");
    if (f) {
        int sr = SR; unsigned data = (unsigned)(frames * 4), riff = 36 + data, br = (unsigned)sr * 4, fmt = 16;
        unsigned short pcm = 1, ch = 2, ba = 4, bps = 16;
        fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
        fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
        fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
        for (long i = 0; i < frames * 2; i++) { float x = out[i] * 32767.0f; short v = (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); fwrite(&v, 2, 1, f); }
        fclose(f);
        printf("machines demo: wrote %s (%.1f s)\n", path, (double)frames / SR);
    }
    free(out);
    og_destroy(g);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--dump-machines")) {
        for (int m = 0; m < OG_MACHINES; m++)
            for (int p = 0; p < OG_MACH_PARAMS; p++) printf("%d %d %d %d\n", m, p, og_machine_default(m, p), og_machine_max(m, p));
        return 0;
    }
    test_switch_and_ranges();
    test_extremes();
    test_character();
    bench();
    if (argc > 1) demo(argv[1]);
    if (failures) { printf("%d machine test(s) FAILED\n", failures); return 1; }
    printf("all machine tests passed\n");
    return 0;
}
