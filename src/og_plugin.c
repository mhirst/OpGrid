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
 *
 * Effects and mixer:
 *   set "fx"   "<id> <v>"      global effect parameter (FX_*)
 *   set "solo" "<t> <0|1>"
 *   get "fx"                   comma list of all FX values
 *   get "mix"                  16 chars per row: mute flags, then 16 solo flags
 *   get "gr"                   master compressor gain (0..1)
 *
 * Clips and scenes:
 *   set "launch" "<t> <c>"   "stop" "<t>"   "scene" "<r>"   "stopall"
 *   set "lq" "<0..5>"        launch quantize: off, step, beat, 1/2/4 bars
 *   set "edit" "<t> <c>"     clip the step/lock/clip calls act on
 *   set "ccopy" "<st> <sc> <dt> <dc>"   "cclr" "<t> <c>"
 *   get "clips"              128 chars, track-major: OG_CLIP_* state per slot
 *   get "edits"              16 chars: each track's edit clip
 *   get "lq"
 *
 * Recording and projects:
 *   set "rec" "0|1"  "rtrack" "<t>"  "rquant" "0|1"  "rmode" "0=dub|1=replace"
 *   set "click" "0=off|1=count-in|2=on"   "countin" "1" (play after 1 bar of clicks)
 *   set "lnon" "<t> <n> <v>"  "lnoff" "<t> <n>"   live notes (recorded when armed)
 *   set "rp" "<t> <pid> <v>"  knob move: a lock while recording, else the base value
 *   set "rnew" "<t> <c>"  "rclose" "<t>"   record a new clip / fix its length
 *   set "tpset" "<t> <v0>,<v1>,..."     all of a track's params at once (loading)
 *   set "undo"  "reset"
 *   set "cload" "<t> <c> <hex>"            restore a clip from og_clip_dump text
 *   get "rec"        "armed,counting_in,growing_track,quant,mode,click"
 *   get "cdump:<t>:<c>"
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

/* MIDI out to the USB-A port: USB-MIDI packet, cable 0. Lock-free ring in
 * the host; safe on the audio thread. */
static void midi_out(void *ctx, uint8_t st, uint8_t d1, uint8_t d2) {
    (void)ctx;
    if (!g_host || !g_host->midi_send_external) return;
    uint8_t pkt[4] = { (uint8_t)(st >> 4), st, d1, d2 };
    g_host->midi_send_external(pkt, 4);
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
    og_set_midi_out(in->g, midi_out, NULL);
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
    } else if (!strcmp(key, "launch")) {
        if (parse_ints(val, a, 2) == 2) og_clip_launch(g, a[0], a[1]);
    } else if (!strcmp(key, "stop")) {
        if (parse_ints(val, a, 1) == 1) og_track_stop(g, a[0]);
    } else if (!strcmp(key, "scene")) {
        if (parse_ints(val, a, 1) == 1) og_scene_launch(g, a[0]);
    } else if (!strcmp(key, "stopall")) {
        og_stop_all(g);
    } else if (!strcmp(key, "lq")) {
        if (parse_ints(val, a, 1) == 1) og_set_launch_quant(g, a[0]);
    } else if (!strcmp(key, "edit")) {
        if (parse_ints(val, a, 2) == 2) og_set_edit_clip(g, a[0], a[1]);
    } else if (!strcmp(key, "ccopy")) {
        if (parse_ints(val, a, 4) == 4) og_clip_copy(g, a[0], a[1], a[2], a[3]);
    } else if (!strcmp(key, "cclr")) {
        if (parse_ints(val, a, 2) == 2) og_clip_clear(g, a[0], a[1]);
    } else if (!strcmp(key, "rec")) {
        og_rec_arm(g, val && atoi(val));
    } else if (!strcmp(key, "rtrack")) {
        if (parse_ints(val, a, 1) == 1) og_rec_set_track(g, a[0]);
    } else if (!strcmp(key, "rquant")) {
        og_rec_set_quant(g, val && atoi(val));
    } else if (!strcmp(key, "rmode")) {
        og_rec_set_mode(g, val && atoi(val));
    } else if (!strcmp(key, "click")) {
        if (parse_ints(val, a, 1) == 1) og_set_click(g, a[0]);
    } else if (!strcmp(key, "countin")) {
        og_play_countin(g);
    } else if (!strcmp(key, "lnon")) {
        if (parse_ints(val, a, 3) == 3) og_live_note_on(g, a[0], a[1], a[2]);
    } else if (!strcmp(key, "lnoff")) {
        if (parse_ints(val, a, 2) == 2) og_live_note_off(g, a[0], a[1]);
    } else if (!strcmp(key, "rp")) {
        if (parse_ints(val, a, 3) == 3) og_rec_param(g, a[0], a[1], a[2]);
    } else if (!strcmp(key, "rnew")) {
        if (parse_ints(val, a, 2) == 2) og_rec_new_clip(g, a[0], a[1]);
    } else if (!strcmp(key, "rclose")) {
        if (parse_ints(val, a, 1) == 1) og_rec_close_clip(g, a[0]);
    } else if (!strcmp(key, "tpset")) {
        int v[1 + P_COUNT_USED];
        int n = parse_ints(val, v, 1 + P_COUNT_USED);
        for (int p = 0; p + 1 < n; p++) og_set_param(g, v[0], p, v[p + 1]);
    } else if (!strcmp(key, "undo")) {
        og_undo(g);
    } else if (!strcmp(key, "reset")) {
        og_reset(g);
        in->sync = 1;
    } else if (!strcmp(key, "cload")) {
        /* "<t> <c> <hex>": parse the two ints, the rest is the payload */
        if (val && parse_ints(val, a, 2) == 2) {
            const char *p = val;
            for (int skip = 0; skip < 2 && *p; skip++) {
                while (*p == ' ') p++;
                while (*p && *p != ' ') p++;
            }
            while (*p == ' ') p++;
            og_clip_load(g, a[0], a[1], p);
        }
    } else if (!strcmp(key, "fx")) {
        if (parse_ints(val, a, 2) == 2) og_set_fx(g, a[0], a[1]);
    } else if (!strcmp(key, "solo")) {
        if (parse_ints(val, a, 2) == 2) og_set_solo(g, a[0], a[1]);
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
    if (!strcmp(key, "clips")) {
        if (buf_len < OG_TRACKS * OG_CLIPS + 1) return -1;
        for (int t = 0; t < OG_TRACKS; t++)
            for (int c = 0; c < OG_CLIPS; c++) buf[t * OG_CLIPS + c] = (char)('0' + og_clip_state(g, t, c));
        buf[OG_TRACKS * OG_CLIPS] = 0;
        return OG_TRACKS * OG_CLIPS;
    }
    if (!strcmp(key, "edits")) {
        if (buf_len < OG_TRACKS + 1) return -1;
        for (int t = 0; t < OG_TRACKS; t++) buf[t] = (char)('0' + og_get_edit_clip(g, t));
        buf[OG_TRACKS] = 0;
        return OG_TRACKS;
    }
    if (!strcmp(key, "rec"))
        return snprintf(buf, (size_t)buf_len, "%d,%d,%d,%d,%d,%d", og_rec_armed(g), og_counting_in(g),
                        og_rec_growing(g), og_rec_quant(g), og_rec_mode(g), og_get_click(g));
    if (!strncmp(key, "cdump:", 6)) {
        int tc[2];
        if (parse_ints(key + 6, tc, 2) != 2) return -1;
        return og_clip_dump(g, tc[0], tc[1], buf, buf_len);
    }
    if (!strcmp(key, "lq")) return snprintf(buf, (size_t)buf_len, "%d", og_get_launch_quant(g));
    if (!strcmp(key, "fx")) {
        int w = 0;
        for (int i = 0; i < FX_COUNT && w < buf_len - 5; i++)
            w += snprintf(buf + w, (size_t)(buf_len - w), i ? ",%d" : "%d", og_get_fx(g, i));
        return w;
    }
    if (!strcmp(key, "mix")) {
        if (buf_len < 2 * OG_TRACKS + 1) return -1;
        for (int t = 0; t < OG_TRACKS; t++) {
            buf[t] = (char)('0' + og_get_mute(g, t));
            buf[OG_TRACKS + t] = (char)('0' + og_get_solo(g, t));
        }
        buf[2 * OG_TRACKS] = 0;
        return 2 * OG_TRACKS;
    }
    if (!strcmp(key, "gr")) return snprintf(buf, (size_t)buf_len, "%.3f", og_debug_comp_gain(g));
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
        uint64_t lm = og_lock_steps(g, t);
        for (int s2 = 0; s2 < OG_STEPS; s2++) {
            int f = og_step_get(g, t, s2, OG_F_FLAGS) & 7;
            buf[s2] = (char)('0' + (f | ((lm >> s2) & 1 ? 8 : 0)));
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
