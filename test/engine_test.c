/*
 * OpGrid engine tests + benchmark + demo render.
 *   make test   -> runs checks, prints timing, writes build/demo.wav
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "plugin_api_v1.h"
#include "og_engine.h"

#define BLOCK 128
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

typedef struct { float peak; int nan; double sumsq; long n; } stats_t;

static stats_t render(og_t *g, int blocks, float *capture) {
    stats_t st = {0};
    float buf[2 * BLOCK];
    for (int b = 0; b < blocks; b++) {
        og_render_float(g, buf, BLOCK);
        for (int i = 0; i < 2 * BLOCK; i++) {
            float x = buf[i];
            if (x != x || isinf(x)) st.nan++;
            if (fabsf(x) > st.peak) st.peak = fabsf(x);
            st.sumsq += (double)x * x;
            st.n++;
        }
        if (capture) memcpy(capture + (size_t)b * 2 * BLOCK, buf, sizeof(buf));
    }
    return st;
}

static double rms(stats_t s) { return s.n ? sqrt(s.sumsq / s.n) : 0; }

static void test_silence_and_note(void) {
    og_t *g = og_create(44100);
    stats_t s = render(g, 20, NULL);
    CHECK(s.peak == 0.0f, "not silent at start: %f", s.peak);

    og_note_on(g, 0, 60, 100);
    s = render(g, 40, NULL);
    CHECK(s.nan == 0, "NaN in note");
    CHECK(s.peak > 0.02f, "note too quiet: %f", s.peak);
    CHECK(s.peak <= 1.0f, "clipped: %f", s.peak);
    CHECK(og_active_voices(g) == 1, "voices %d", og_active_voices(g));

    og_note_off(g, 0, 60);
    render(g, 2000, NULL);   /* ~6 s: default release is well under that */
    s = render(g, 10, NULL);
    CHECK(s.peak < 1e-4f, "not silent after release: %f", s.peak);
    CHECK(og_active_voices(g) == 0, "voice still active");
    og_destroy(g);
}

static void test_all_algos_and_filters(void) {
    for (int a = 0; a < OG_ALGOS; a++) {
        for (int f = 0; f < OG_FILTER_TYPES; f++) {
            og_t *g = og_create(44100);
            og_set_param(g, 0, P_ALGO, a);
            og_set_param(g, 0, P_FTYP, f);
            og_set_param(g, 0, P_LVL4, 100);
            og_set_param(g, 0, P_FDBK, 127);
            og_set_param(g, 0, P_RESO, 127);
            og_set_param(g, 0, P_DRIV, 90);
            og_set_param(g, 0, P_FREQ, f == OG_FILTER_HP ? 40 : 90);
            og_note_on(g, 0, 48, 127);
            og_note_on(g, 0, 55, 127);
            stats_t s = render(g, 100, NULL);
            CHECK(s.nan == 0, "NaN algo %d filter %d", a, f);
            CHECK(rms(s) > 0.001, "silent algo %d filter %d (rms %f)", a, f, rms(s));
            CHECK(s.peak <= 1.0f, "clip algo %d filter %d", a, f);
            og_destroy(g);
        }
    }
}

static void test_voice_stealing(void) {
    og_t *g = og_create(44100);
    for (int i = 0; i < 40; i++) {
        og_note_on(g, i % OG_TRACKS, 36 + i, 100);
        render(g, 1, NULL);
    }
    CHECK(og_active_voices(g) <= OG_VOICES, "too many voices %d", og_active_voices(g));
    stats_t s = render(g, 50, NULL);
    CHECK(s.nan == 0, "NaN while stealing");
    CHECK(og_active_voices(g) == OG_VOICES, "pool should be full: %d", og_active_voices(g));
    og_all_notes_off(g);
    render(g, 4, NULL);
    CHECK(og_active_voices(g) == 0, "panic left voices: %d", og_active_voices(g));
    og_destroy(g);
}

static void test_retrigger_same_note(void) {
    og_t *g = og_create(44100);
    for (int i = 0; i < 30; i++) { og_note_on(g, 2, 60, 90); render(g, 2, NULL); }
    CHECK(og_active_voices(g) == 1, "same note should reuse one voice: %d", og_active_voices(g));
    og_destroy(g);
}

static void test_params_clamp(void) {
    og_t *g = og_create(44100);
    og_set_param(g, 0, P_ALGO, 99);
    CHECK(og_get_param(g, 0, P_ALGO) == OG_ALGOS - 1, "algo clamp");
    og_set_param(g, 0, P_FREQ, -5);
    CHECK(og_get_param(g, 0, P_FREQ) == 0, "freq clamp");
    og_set_param(g, 99, P_FREQ, 5);   /* out of range track: ignored */
    og_destroy(g);
}

static void test_plugin_protocol(void) {
    extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.sample_rate = 44100;
    host.frames_per_block = BLOCK;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    CHECK(api && api->api_version == 2, "api");
    void *inst = api->create_instance("/tmp", NULL);
    char buf[512];
    api->get_param(inst, "module_id", buf, sizeof(buf));
    CHECK(!strcmp(buf, "opgrid"), "module_id %s", buf);
    api->set_param(inst, "p", "3 16 42");
    api->get_param(inst, "tp:3", buf, sizeof(buf));
    int vals[64], n = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) vals[n++] = atoi(t);
    CHECK(n == P_COUNT_USED, "tp count %d", n);
    CHECK(vals[P_FREQ] == 42, "tp freq %d", vals[P_FREQ]);
    api->set_param(inst, "non", "3 64 100");
    int16_t out[2 * BLOCK];
    int nz = 0;
    for (int b = 0; b < 10; b++) {
        api->render_block(inst, out, BLOCK);
        for (int i = 0; i < 2 * BLOCK; i++) nz |= out[i] != 0;
    }
    CHECK(nz, "plugin produced silence");
    api->get_param(inst, "voices", buf, sizeof(buf));
    CHECK(atoi(buf) == 1, "voices %s", buf);
    api->set_param(inst, "noff", "3 64");
    api->set_param(inst, "panic", "1");
    api->destroy_instance(inst);
}

/* Worst case: every voice busy, heaviest settings. */
static void bench(void) {
    og_t *g = og_create(44100);
    for (int t = 0; t < OG_TRACKS; t++) {
        og_set_param(g, t, P_ALGO, 0);
        og_set_param(g, t, P_FTYP, OG_FILTER_LP24);
        og_set_param(g, t, P_DRIV, 64);
        og_set_param(g, t, P_FDBK, 64);
        og_set_param(g, t, P_GLID, 20);
        og_set_param(g, t, P_ASUS, 127);
    }
    for (int i = 0; i < OG_VOICES; i++) og_note_on(g, i, 40 + i * 3, 100);
    float buf[2 * BLOCK];
    for (int i = 0; i < 50; i++) og_render_float(g, buf, BLOCK);
    int blocks = 3000;
    double worst = 0, total = 0;
    for (int i = 0; i < blocks; i++) {
        double t0 = now_us();
        og_render_float(g, buf, BLOCK);
        double dt = now_us() - t0;
        total += dt;
        if (dt > worst) worst = dt;
    }
    printf("bench: %d voices, avg %.1f us/block, worst %.1f us/block (this machine)\n",
           og_active_voices(g), total / blocks, worst);
    og_destroy(g);
}

static void write_wav(const char *path, const float *lr, long frames) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return; }
    int sr = 44100, ch = 2, bits = 16;
    unsigned data = (unsigned)(frames * ch * 2);
    unsigned riff = 36 + data, br = (unsigned)(sr * ch * 2), fmt = 16;
    unsigned short pcm = 1, chs = (unsigned short)ch, ba = (unsigned short)(ch * 2), bps = (unsigned short)bits;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&chs, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (long i = 0; i < frames * ch; i++) {
        float x = lr[i] * 32767.0f;
        short s = (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

/* A few bars across three tracks so the FM engine can be heard. */
static void demo(const char *path) {
    og_t *g = og_create(44100);
    /* track 0: bass, algo 0, short */
    og_set_param(g, 0, P_ALGO, 0); og_set_param(g, 0, P_RAT2, 3); og_set_param(g, 0, P_LVL2, 80);
    og_set_param(g, 0, P_DEC2, 55); og_set_param(g, 0, P_FREQ, 80); og_set_param(g, 0, P_FENV, 90);
    og_set_param(g, 0, P_FDEC, 50); og_set_param(g, 0, P_ASUS, 60); og_set_param(g, 0, P_AREL, 30);
    og_set_param(g, 0, P_RESO, 60); og_set_param(g, 0, P_FTYP, OG_FILTER_LP24);
    /* track 1: bell pad, algo 4 */
    og_set_param(g, 1, P_ALGO, 4); og_set_param(g, 1, P_RAT2, 13); og_set_param(g, 1, P_LVL2, 50);
    og_set_param(g, 1, P_RAT3, 3); og_set_param(g, 1, P_LVL3, 90); og_set_param(g, 1, P_RAT4, 9);
    og_set_param(g, 1, P_LVL4, 40); og_set_param(g, 1, P_AATK, 50); og_set_param(g, 1, P_AREL, 85);
    og_set_param(g, 1, P_DTUN, 50); og_set_param(g, 1, P_VOL, 80); og_set_param(g, 1, P_PAN, 40);
    /* track 2: pluck lead with feedback */
    og_set_param(g, 2, P_ALGO, 2); og_set_param(g, 2, P_FDBK, 70); og_set_param(g, 2, P_LVL4, 60);
    og_set_param(g, 2, P_DEC4, 40); og_set_param(g, 2, P_ADEC, 55); og_set_param(g, 2, P_ASUS, 0);
    og_set_param(g, 2, P_PAN, 90); og_set_param(g, 2, P_VOL, 85);

    const int bass[16] = {36,0,36,48, 0,36,0,43, 34,0,34,46, 0,34,41,0};
    const int lead[16] = {72,0,75,0, 79,0,77,75, 0,74,0,72, 70,0,0,74};
    const int chords[2][3] = {{60,63,67},{58,62,65}};
    int step_frames = (int)(44100 * 60.0 / 118.0 / 4.0);
    int bars = 4;
    long total = (long)bars * 16 * step_frames + 44100;
    float *out = calloc((size_t)total * 2, sizeof(float));
    long pos = 0;
    for (int bar = 0; bar < bars; bar++) {
        const int *ch = chords[bar % 2];
        for (int i = 0; i < 3; i++) og_note_on(g, 1, ch[i], 80);
        for (int st = 0; st < 16; st++) {
            if (bass[st]) og_note_on(g, 0, bass[st], 110);
            if (bar >= 1 && lead[st]) og_note_on(g, 2, lead[st], 100);
            int half = step_frames / 2;
            og_render_float(g, out + pos * 2, half); pos += half;
            if (bass[st]) og_note_off(g, 0, bass[st]);
            if (bar >= 1 && lead[st]) og_note_off(g, 2, lead[st]);
            og_render_float(g, out + pos * 2, step_frames - half); pos += step_frames - half;
        }
        for (int i = 0; i < 3; i++) og_note_off(g, 1, ch[i]);
    }
    og_render_float(g, out + pos * 2, (int)(total - pos));
    write_wav(path, out, total);
    printf("demo: wrote %s (%.1f s)\n", path, total / 44100.0);
    free(out);
    og_destroy(g);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--dump-params")) {
        for (int p = 0; p < P_COUNT_USED; p++) printf("%d %d\n", og_param_default(p), og_param_max(p));
        return 0;
    }
    test_silence_and_note();
    test_all_algos_and_filters();
    test_voice_stealing();
    test_retrigger_same_note();
    test_params_clamp();
    test_plugin_protocol();
    bench();
    demo(argc > 1 ? argv[1] : "build/demo.wav");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all engine tests passed\n");
    return 0;
}
