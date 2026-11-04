/*
 * OpGrid — Schwung plugin glue (overtake DSP, plugin API v2).
 *
 * Param protocol (all values are ASCII):
 *   set "non"    "<track> <note> <vel>"   note on
 *   set "noff"   "<track> <note>"         note off
 *   set "panic"  "1"                      all notes off
 *   set "p"      "<track> <pid> <value>"  sound parameter
 *   set "master" "<0..127>"
 *   get "module_id"                       "opgrid"
 *   get "tp:<track>"                      comma list of every param value
 *   get "voices"                          active voice count
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api_v1.h"
#include "og_engine.h"

static const host_api_v1_t *g_host;

static int parse_ints(const char *s, int *out, int max) {
    int n = 0;
    while (s && *s && n < max) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s) break;
        out[n++] = (int)v;
        s = end;
        while (*s == ' ' || *s == ',' || *s == ':') s++;
    }
    return n;
}

static void *plug_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    (void)json_defaults;
    float sr = (g_host && g_host->sample_rate > 0) ? (float)g_host->sample_rate : 44100.0f;
    return og_create(sr);
}

static void plug_destroy(void *inst) { og_destroy((og_t *)inst); }

static void plug_midi(void *inst, const uint8_t *msg, int len, int source) {
    (void)source;
    og_t *g = (og_t *)inst;
    if (!g || len < 3) return;
    /* External MIDI: channel N plays track N. */
    int kind = msg[0] & 0xF0, ch = msg[0] & 0x0F;
    if (source != MOVE_MIDI_SOURCE_EXTERNAL) return;
    if (kind == 0x90 && msg[2] > 0) og_note_on(g, ch, msg[1], msg[2]);
    else if (kind == 0x80 || (kind == 0x90 && msg[2] == 0)) og_note_off(g, ch, msg[1]);
}

static void plug_set(void *inst, const char *key, const char *val) {
    og_t *g = (og_t *)inst;
    if (!g || !key) return;
    int a[3] = {0};
    if (!strcmp(key, "non")) {
        if (parse_ints(val, a, 3) == 3) og_note_on(g, a[0], a[1], a[2]);
    } else if (!strcmp(key, "noff")) {
        if (parse_ints(val, a, 2) == 2) og_note_off(g, a[0], a[1]);
    } else if (!strcmp(key, "p")) {
        if (parse_ints(val, a, 3) == 3) og_set_param(g, a[0], a[1], a[2]);
    } else if (!strcmp(key, "panic")) {
        og_all_notes_off(g);
    } else if (!strcmp(key, "master")) {
        if (parse_ints(val, a, 1) == 1) og_set_master(g, a[0]);
    }
}

static int plug_get(void *inst, const char *key, char *buf, int buf_len) {
    og_t *g = (og_t *)inst;
    if (!key || !buf || buf_len <= 0) return -1;
    if (!strcmp(key, "module_id")) return snprintf(buf, (size_t)buf_len, "opgrid");
    if (!g) return -1;
    if (!strcmp(key, "voices")) return snprintf(buf, (size_t)buf_len, "%d", og_active_voices(g));
    if (!strncmp(key, "tp:", 3)) {
        int t = atoi(key + 3), w = 0;
        for (int p = 0; p < P_COUNT_USED && w < buf_len - 5; p++)
            w += snprintf(buf + w, (size_t)(buf_len - w), p ? ",%d" : "%d", og_get_param(g, t, p));
        return w;
    }
    return -1;
}

static int plug_error(void *inst, char *buf, int buf_len) {
    (void)inst; (void)buf; (void)buf_len;
    return 0;
}

static void plug_render(void *inst, int16_t *out_lr, int frames) {
    og_t *g = (og_t *)inst;
    if (!g) { memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t)); return; }
    og_render(g, out_lr, frames);
}

static plugin_api_v2_t api = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = plug_create,
    .destroy_instance = plug_destroy,
    .on_midi = plug_midi,
    .set_param = plug_set,
    .get_param = plug_get,
    .get_error = plug_error,
    .render_block = plug_render,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
