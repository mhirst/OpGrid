/* Render every starter sound (build/sounds.txt from tools/make_sounds.mjs):
 * it must load cleanly, sound, stay finite and under full scale, and fall
 * silent after the note ends. Prints a loudness table for balancing. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "og_engine.h"

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv) {
    FILE *f = fopen(argc > 1 ? argv[1] : "build/sounds.txt", "r");
    if (!f) { printf("no sounds file\n"); return 1; }
    char line[4096];
    int count = 0;
    double lo = 1e9, hi = 0;
    while (fgets(line, sizeof(line), f)) {
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        int p[P_COUNT_USED], n = 0;
        for (char *t = strtok(bar + 1, ",\n"); t && n < P_COUNT_USED; t = strtok(NULL, ",\n")) p[n++] = atoi(t);
        CHECK(n == P_COUNT_USED, "%s: %d params", line, n);
        og_t *g = og_create(44100);
        og_set_param(g, 0, P_MACH, p[P_MACH]);
        for (int i = 0; i < P_COUNT_USED; i++) og_set_param(g, 0, i, p[i]);
        for (int i = 0; i < P_COUNT_USED; i++)
            CHECK(og_get_param(g, 0, i) == p[i], "%s: param %d stored as %d, not %d", line, i, og_get_param(g, 0, i), p[i]);
        int notes[2] = {36, 60};
        double e = 0; long cnt = 0; float peak = 0; int bad = 0;
        float buf[256];
        for (int k = 0; k < 2; k++) {
            og_note_on(g, 0, notes[k], 110);
            for (int b = 0; b < 344; b++) {        /* 1 s held; loudness from the first 150 ms (fair to drums) */
                og_render_float(g, buf, 128);
                for (int i = 0; i < 256; i++) {
                    if (!(buf[i] == buf[i])) bad++;
                    if (fabsf(buf[i]) > peak) peak = fabsf(buf[i]);
                    if (b < 52) { e += (double)buf[i] * buf[i]; cnt++; }
                }
            }
            og_note_off(g, 0, notes[k]);
        }
        for (int b = 0; b < 344 * 6; b++) og_render_float(g, buf, 128);   /* 6 s of release/tails */
        double tail = 0;
        for (int b = 0; b < 40; b++) { og_render_float(g, buf, 128); for (int i = 0; i < 256; i++) tail += (double)buf[i] * buf[i]; }
        double rms = sqrt(e / (double)cnt);
        printf("  %-14s machine %d  rms %.3f  peak %.3f\n", line, p[P_MACH], rms, peak);
        CHECK(!bad, "%s: NaN", line);
        CHECK(peak <= 1.0f, "%s: clips", line);
        CHECK(rms > 0.01 || peak > 0.08, "%s: too quiet (rms %.4f, peak %.3f)", line, rms, peak);
        CHECK(sqrt(tail / (40 * 256)) < 0.002, "%s: still sounding 6 s after release", line);
        if (rms < lo) lo = rms;
        if (rms > hi) hi = rms;
        og_destroy(g);
        count++;
    }
    fclose(f);
    CHECK(count >= 30, "only %d sounds", count);
    printf("%d sounds, loudness range %.1f dB\n", count, 20 * log10(hi / lo));
    if (failures) { printf("%d sound test(s) FAILED\n", failures); return 1; }
    printf("all starter sounds passed\n");
    return 0;
}
