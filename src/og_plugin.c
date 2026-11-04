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
 *
 * Sequencer (steps/locks act on the track's active clip):
 *   set "play" "0|1"   "tempo" "<bpm>"   "sync" "0|1"   "fill" "0|1"
 *   set "mute"   "<t> <0|1>"
 *   set "stog"   "<t> <s>"                toggle a note step
 *   set "sf"     "<t> <s> <field> <v>"    step field (OG_F_*)
 *   set "sclr"   "<t> <s>"                clear step + its locks
 *   set "lock"   "<t> <s> <pid> <v>"      set a parameter lock
 *   set "unlock" "<t> <s> <pid>"
 *   set "clen" / "cspd" / "cswg"  "<t> <v>"  clip length, speed, swing
 *   get "play", "tempo", "sync"
 *   get "pos"            16 comma-separated playheads (-1 = stopped)
 *   get "steps:<t>"      64 chars; char - '0' = flags | 8 if the step has locks
 *   get "step:<t>:<s>"   comma list of the OG_FIELDS step fields
 *   get "locks:<t>:<s>"  "pid=val,..."
 *   get "clip:<t>"       "length,speed,swing,locks_used,muted"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api_v1.h"
#include "og_engine.h"

static const host_api_v1_t *g_host;

typedef struct { og_t *g; int sync; } inst_t;

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
    inst_t *in = (inst_t *)calloc(1, sizeof(inst_t));
    if (!in) return NULL;
    in->g = og_create(sr);
    if (!in->g) { free(in); return NULL; }
    in->sync = 1;
    return in;
}

static void plug_destroy(void *inst) {
    inst_t *in = (inst_t *)inst;
    if (!in) return;
    og_destroy(in->g);
    free(in);
}

static void plug_midi(void *inst, const uint8_t *msg, int len, int source) {
    (void)source;
    og_t *g = inst ? ((inst_t *)inst)->g : NULL;
    if (!g || len < 3) return;
    /* External MIDI: channel N plays track N. */
    int kind = msg[0] & 0xF0, ch = msg[0] & 0x0F;
    if (source != MOVE_MIDI_SOURCE_EXTERNAL) return;
    if (kind == 0x90 && msg[2] > 0) og_note_on(g, ch, msg[1], msg[2]);
    else if (kind == 0x80 || (kind == 0x90 && msg[2] == 0)) og_note_off(g, ch, msg[1]);
}

static void plug_set(void *inst, const char *key, const char *val) {
    inst_t *in = (inst_t *)inst;
    og_t *g = in ? in->g : NULL;
    if (!g || !key) return;
    int a[4] = {0};
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
    } else if (!strcmp(key, "play")) {
        og_play(g, val && atoi(val));
    } else if (!strcmp(key, "tempo")) {
        if (val) { og_set_tempo(g, (float)atof(val)); in->sync = 0; }
    } else if (!strcmp(key, "sync")) {
        in->sync = val && atoi(val);
    } else if (!strcmp(key, "fill")) {
        og_set_fill(g, val && atoi(val));
    } else if (!strcmp(key, "mute")) {
        if (parse_ints(val, a, 2) == 2) og_set_mute(g, a[0], a[1]);
    } else if (!strcmp(key, "stog")) {
        if (parse_ints(val, a, 2) == 2) {
            int f = og_step_get(g, a[0], a[1], OG_F_FLAGS);
            int on = f & (OG_STEP_NOTE | OG_STEP_LOCK);
            og_step_set(g, a[0], a[1], OG_F_FLAGS, on ? (f & ~(OG_STEP_NOTE | OG_STEP_LOCK))
                                                      : (f | OG_STEP_NOTE));
        }
    } else if (!strcmp(key, "sf")) {
        if (parse_ints(val, a, 4) == 4) og_step_set(g, a[0], a[1], a[2], a[3]);
    } else if (!strcmp(key, "sclr")) {
        if (parse_ints(val, a, 2) == 2) og_step_clear(g, a[0], a[1]);
    } else if (!strcmp(key, "lock")) {
        if (parse_ints(val, a, 4) == 4) og_lock_set(g, a[0], a[1], a[2], a[3]);
    } else if (!strcmp(key, "unlock")) {
        if (parse_ints(val, a, 3) == 3) og_lock_clear(g, a[0], a[1], a[2]);
    } else if (!strcmp(key, "clen")) {
        if (parse_ints(val, a, 2) == 2) og_clip_set_length(g, a[0], a[1]);
    } else if (!strcmp(key, "cspd")) {
        if (parse_ints(val, a, 2) == 2) og_clip_set_speed(g, a[0], a[1]);
    } else if (!strcmp(key, "cswg")) {
        if (parse_ints(val, a, 2) == 2) og_clip_set_swing(g, a[0], a[1]);
    }
}

static int plug_get(void *inst, const char *key, char *buf, int buf_len) {
    inst_t *in = (inst_t *)inst;
    og_t *g = in ? in->g : NULL;
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
    if (!strcmp(key, "play")) return snprintf(buf, (size_t)buf_len, "%d", og_is_playing(g));
    if (!strcmp(key, "tempo")) return snprintf(buf, (size_t)buf_len, "%.1f", og_get_tempo(g));
    if (!strcmp(key, "sync")) return snprintf(buf, (size_t)buf_len, "%d", in->sync);
    if (!strcmp(key, "pos")) {
        int w = 0;
        for (int t = 0; t < OG_TRACKS && w < buf_len - 5; t++)
            w += snprintf(buf + w, (size_t)(buf_len - w), t ? ",%d" : "%d", og_track_step(g, t));
        return w;
    }
    int a[2];
    if (!strncmp(key, "steps:", 6)) {
        int t = atoi(key + 6);
        if (buf_len < OG_STEPS + 1) return -1;
        for (int s2 = 0; s2 < OG_STEPS; s2++) {
            int f = og_step_get(g, t, s2, OG_F_FLAGS) & 7, has = 0;
            for (int p = 0; p < P_COUNT_USED && !has; p++) has = og_lock_get(g, t, s2, p) >= 0;
            buf[s2] = (char)('0' + (f | (has ? 8 : 0)));
        }
        buf[OG_STEPS] = 0;
        return OG_STEPS;
    }
    if (!strncmp(key, "step:", 5) && parse_ints(key + 5, a, 2) == 2) {
        int w = 0;
        for (int f = 0; f < OG_FIELDS && w < buf_len - 6; f++)
            w += snprintf(buf + w, (size_t)(buf_len - w), f ? ",%d" : "%d", og_step_get(g, a[0], a[1], f));
        return w;
    }
    if (!strncmp(key, "locks:", 6) && parse_ints(key + 6, a, 2) == 2) {
        int w = 0;
        buf[0] = 0;
        for (int p = 0; p < P_COUNT_USED && w < buf_len - 10; p++) {
            int v = og_lock_get(g, a[0], a[1], p);
            if (v >= 0) w += snprintf(buf + w, (size_t)(buf_len - w), w ? ",%d=%d" : "%d=%d", p, v);
        }
        return w;
    }
    if (!strncmp(key, "clip:", 5)) {
        int t = atoi(key + 5);
        return snprintf(buf, (size_t)buf_len, "%d,%d,%d,%d,%d", og_clip_length(g, t),
                        og_clip_speed(g, t), og_clip_swing(g, t), og_lock_count(g, t), og_get_mute(g, t));
    }
    return -1;
}

static int plug_error(void *inst, char *buf, int buf_len) {
    (void)inst; (void)buf; (void)buf_len;
    return 0;
}

static void plug_render(void *inst, int16_t *out_lr, int frames) {
    inst_t *in = (inst_t *)inst;
    if (!in || !in->g) { memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t)); return; }
    /* follow Move's tempo unless the user set one here */
    if (in->sync && g_host && g_host->get_bpm) {
        float bpm = g_host->get_bpm();
        if (bpm >= 30.0f && bpm <= 300.0f) og_set_tempo(in->g, bpm);
    }
    og_render(in->g, out_lr, frames);
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
