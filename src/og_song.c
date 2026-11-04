/*
 * OpGrid song mode: songs of rows (per-track clips, mutes, bars, repeats,
 * tempo, swing), played back on bar boundaries inside the audio callback,
 * plus performance capture that turns live launching into song rows.
 */
#include <math.h>
#include <string.h>

#include "og_internal.h"

static inline int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static int song_ok(int s) { return s >= 0 && s < OG_SONGS; }

static void row_default(og_row_t *r) {
    memset(r, 0, sizeof(*r));
    for (int t = 0; t < OG_TRACKS; t++) r->clips[t] = -1;
    r->bars = 4;
    r->reps = 1;
}

void og__song_init(og_t *g) {
    for (int s = 0; s < OG_SONGS; s++) {
        memset(&g->songs[s], 0, sizeof(og_song_t));
        g->songs[s].loop = 1;
        for (int r = 0; r < OG_SONG_ROWS; r++) row_default(&g->songs[s].rows[r]);
    }
    g->song_on = 0;
    g->song = 0;
    g->song_row = 0;
    g->song_jump = -1;
    g->song_start_pending = 0;
    g->swing_override = 0;
    g->capturing = 0;
}

static void row_from_scene(const og_t *g, og_row_t *r, int scene) {
    for (int t = 0; t < OG_TRACKS; t++)
        r->clips[t] = (int8_t)(g->clips[t][scene].used ? scene : -1);
}

/* ---- editing --------------------------------------------------------- */

void og_song_set(og_t *g, int song, int row, int field, int value) {
    if (!g || !song_ok(song) || row < 0 || row >= g->songs[song].len) return;
    og_row_t *r = &g->songs[song].rows[row];
    switch (field) {
    case OG_R_SCENE: if (value >= 0 && value < OG_CLIPS) row_from_scene(g, r, value); break;
    case OG_R_BARS:  r->bars = (uint8_t)clampi(value, 1, 64); break;
    case OG_R_REPS:  r->reps = (uint8_t)clampi(value, 1, 16); break;
    case OG_R_TEMPO: r->tempo = (uint16_t)(value <= 0 ? 0 : clampi(value, 30, 300)); break;
    case OG_R_SWING: r->swing = (uint8_t)(value <= 0 ? 0 : clampi(value, 50, 80)); break;
    case OG_R_MUTES: r->mutes = (uint16_t)(value & 0xFFFF); break;
    default:
        if (field >= OG_R_CLIP0 && field < OG_R_FIELDS)
            r->clips[field - OG_R_CLIP0] = (int8_t)clampi(value, -1, OG_CLIPS - 1);
        break;
    }
}

int og_song_get(const og_t *g, int song, int row, int field) {
    if (!g || !song_ok(song) || row < 0 || row >= g->songs[song].len) return 0;
    const og_row_t *r = &g->songs[song].rows[row];
    switch (field) {
    case OG_R_SCENE: return og_song_scene_of(g, song, row);
    case OG_R_BARS:  return r->bars;
    case OG_R_REPS:  return r->reps;
    case OG_R_TEMPO: return r->tempo;
    case OG_R_SWING: return r->swing;
    case OG_R_MUTES: return r->mutes;
    default:
        return (field >= OG_R_CLIP0 && field < OG_R_FIELDS) ? r->clips[field - OG_R_CLIP0] : 0;
    }
}

int og_song_scene_of(const og_t *g, int song, int row) {
    if (!g || !song_ok(song) || row < 0 || row >= g->songs[song].len) return -1;
    const og_row_t *r = &g->songs[song].rows[row];
    for (int sc = 0; sc < OG_CLIPS; sc++) {
        int match = 1, any = 0;
        for (int t = 0; t < OG_TRACKS && match; t++) {
            int want = g->clips[t][sc].used ? sc : -1;
            if (r->clips[t] != want) match = 0;
            any |= want >= 0;
        }
        if (match && any) return sc;
    }
    return -1;
}

int og_song_insert(og_t *g, int song, int at, int scene) {
    if (!g || !song_ok(song)) return -1;
    og_song_t *s = &g->songs[song];
    if (s->len >= OG_SONG_ROWS) return -1;
    at = clampi(at, 0, s->len);
    memmove(&s->rows[at + 1], &s->rows[at], sizeof(og_row_t) * (size_t)(s->len - at));
    og_row_t *r = &s->rows[at];
    if (scene >= 0 && scene < OG_CLIPS) { row_default(r); row_from_scene(g, r, scene); }
    else if (at > 0) *r = s->rows[at - 1];         /* duplicate the row before */
    else row_default(r);
    s->len++;
    return at;
}

void og_song_delete(og_t *g, int song, int row) {
    if (!g || !song_ok(song)) return;
    og_song_t *s = &g->songs[song];
    if (row < 0 || row >= s->len) return;
    memmove(&s->rows[row], &s->rows[row + 1], sizeof(og_row_t) * (size_t)(s->len - row - 1));
    s->len--;
    row_default(&s->rows[s->len]);
    if (g->song_on && g->song == song && g->song_row >= s->len) g->song_row = s->len ? s->len - 1 : 0;
}

int  og_song_length(const og_t *g, int song) { return (g && song_ok(song)) ? g->songs[song].len : 0; }
void og_song_set_loop(og_t *g, int song, int on) { if (g && song_ok(song)) g->songs[song].loop = on ? 1 : 0; }
int  og_song_loop(const og_t *g, int song) { return (g && song_ok(song)) ? g->songs[song].loop : 0; }
void og_song_clear(og_t *g, int song) {
    if (!g || !song_ok(song)) return;
    g->songs[song].len = 0;
    for (int r = 0; r < OG_SONG_ROWS; r++) row_default(&g->songs[song].rows[r]);
}

/* ---- playback -------------------------------------------------------- */

void og_song_mode(og_t *g, int on, int song, int row) {
    if (!g) return;
    if (!on) {
        g->song_on = 0;
        g->swing_override = 0;
        g->song_start_pending = 0;
        return;
    }
    if (!song_ok(song) || g->songs[song].len == 0) return;
    g->song = song;
    g->song_row = clampi(row, 0, g->songs[song].len - 1);
    g->song_bar = 0;
    g->song_rep = 0;
    g->song_jump = -1;
    g->song_start_pending = 1;
    g->song_on = 1;
}

int og_song_on(const og_t *g) { return g ? g->song_on : 0; }

void og_song_jump(og_t *g, int row) {
    if (!g || !g->song_on) return;
    if (row < 0 || row >= g->songs[g->song].len) return;
    g->song_jump = row;
}

void og_song_status(const og_t *g, int out[6]) {
    out[0] = g->song_on; out[1] = g->song; out[2] = g->song_row;
    out[3] = g->song_bar; out[4] = g->song_rep; out[5] = g->capturing;
}

static void queue_at(og_t *g, int t, int what, double b) {
    og_trackseq_t *ts = &g->ts[t];
    if (what < 0) {
        if (ts->clip < 0 && ts->queued == OG_Q_NONE) return;
        ts->queued = OG_Q_STOP;
    } else {
        if (ts->clip == what && ts->queued == OG_Q_NONE) return;   /* already playing: keep going */
        ts->queued = what;
    }
    ts->queue_at = b;
}

static void apply_row(og_t *g, double b) {
    const og_row_t *r = &g->songs[g->song].rows[g->song_row];
    for (int t = 0; t < OG_TRACKS; t++) {
        queue_at(g, t, r->clips[t], b);
        g->ts[t].mute = (r->mutes >> t) & 1;
    }
    if (r->tempo) og_set_tempo(g, (float)r->tempo);
    g->swing_override = r->swing;
}

/* what each track will play from bar b on (queued switches included) */
static void snapshot(const og_t *g, og_row_t *r, double b) {
    row_default(r);
    for (int t = 0; t < OG_TRACKS; t++) {
        const og_trackseq_t *ts = &g->ts[t];
        int c = ts->clip;
        if (ts->queued != OG_Q_NONE && ts->queue_at <= b + 1e-6) c = ts->queued == OG_Q_STOP ? -1 : ts->queued;
        r->clips[t] = (int8_t)c;
        if (ts->mute) r->mutes |= (uint16_t)(1u << t);
    }
    r->bars = 1;
}

static void capture_bar(og_t *g, double b) {
    og_song_t *s = &g->songs[g->cap_song];
    og_row_t now;
    snapshot(g, &now, b);
    if (s->len > 0) {
        og_row_t *last = &s->rows[s->len - 1];
        if (!memcmp(last->clips, now.clips, sizeof(now.clips)) && last->mutes == now.mutes && last->bars < 64) {
            last->bars++;
            return;
        }
    }
    if (s->len >= OG_SONG_ROWS) { g->capturing = 0; return; }   /* full: stop capturing */
    s->rows[s->len++] = now;
}

void og_capture(og_t *g, int on, int dest) {
    if (!g) return;
    if (!on) { g->capturing = 0; return; }
    if (!song_ok(dest)) return;
    og_song_clear(g, dest);
    g->cap_song = dest;
    g->capturing = 1;
}

void og__song_bar(og_t *g, double gs, double ge) {
    /* integer bar counter: a float ceil() on gpos can skip a boundary after drift */
    /* the last bar starting before ge — the same "at < end" rule the step scan uses */
    long k = (long)ceil((ge - OG_EPS) / 16.0) - 1;
    if (k <= g->last_bar) return;
    g->last_bar = k;
    if (!g->song_on && !g->capturing) return;
    double b = (double)k * 16.0;
    if (b < gs) b = gs;

    if (g->song_on) {
        og_song_t *s = &g->songs[g->song];
        if (s->len == 0) { og_song_mode(g, 0, 0, 0); }
        else if (g->song_start_pending) {
            g->song_start_pending = 0;
            g->song_bar = g->song_rep = 0;
            apply_row(g, b);
        } else if (g->song_jump >= 0) {
            g->song_row = g->song_jump;
            g->song_jump = -1;
            g->song_bar = g->song_rep = 0;
            apply_row(g, b);
        } else {
            const og_row_t *r = &s->rows[g->song_row];
            if (++g->song_bar >= r->bars) {
                g->song_bar = 0;
                if (++g->song_rep >= r->reps) {
                    g->song_rep = 0;
                    if (++g->song_row >= s->len) {
                        if (s->loop) g->song_row = 0;
                        else {
                            /* the end: stop every track on this bar */
                            g->song_row = s->len - 1;
                            for (int t = 0; t < OG_TRACKS; t++) queue_at(g, t, -1, b);
                            g->song_on = 0;
                            g->swing_override = 0;
                            goto capture;
                        }
                    }
                    apply_row(g, b);
                }
            }
        }
    }
capture:
    if (g->capturing) capture_bar(g, b);
}

/* ---- serialization --------------------------------------------------- */

static const char k_hex[] = "0123456789abcdef";
static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int og_song_dump(const og_t *g, int song, char *out, int out_len) {
    if (!g || !song_ok(song) || out_len < OG_SONG_HEX_MAX + 1) return -1;
    const og_song_t *s = &g->songs[song];
    uint8_t b[2 + OG_SONG_ROWS * (OG_TRACKS + 7)];
    int k = 0;
    b[k++] = s->len; b[k++] = s->loop;
    for (int i = 0; i < s->len; i++) {
        const og_row_t *r = &s->rows[i];
        for (int t = 0; t < OG_TRACKS; t++) b[k++] = (uint8_t)(r->clips[t] + 1);
        b[k++] = (uint8_t)(r->mutes & 0xFF); b[k++] = (uint8_t)(r->mutes >> 8);
        b[k++] = r->bars; b[k++] = r->reps;
        b[k++] = (uint8_t)(r->tempo & 0xFF); b[k++] = (uint8_t)(r->tempo >> 8);
        b[k++] = r->swing;
    }
    for (int i = 0; i < k; i++) { out[2 * i] = k_hex[b[i] >> 4]; out[2 * i + 1] = k_hex[b[i] & 15]; }
    out[2 * k] = 0;
    return 2 * k;
}

int og_song_load(og_t *g, int song, const char *hex) {
    if (!g || !song_ok(song) || !hex) return 0;
    uint8_t b[2 + OG_SONG_ROWS * (OG_TRACKS + 7)];
    int n = 0;
    for (const char *p = hex; p[0] && p[1] && n < (int)sizeof(b); p += 2) {
        int hi = hexval(p[0]), lo = hexval(p[1]);
        if (hi < 0 || lo < 0) return 0;
        b[n++] = (uint8_t)(hi << 4 | lo);
    }
    if (n < 2 || b[0] > OG_SONG_ROWS || n < 2 + b[0] * (OG_TRACKS + 7)) return 0;
    og_song_t s;
    memset(&s, 0, sizeof(s));
    for (int r = 0; r < OG_SONG_ROWS; r++) row_default(&s.rows[r]);
    s.len = b[0];
    s.loop = b[1] ? 1 : 0;
    int k = 2;
    for (int i = 0; i < s.len; i++) {
        og_row_t *r = &s.rows[i];
        for (int t = 0; t < OG_TRACKS; t++) r->clips[t] = (int8_t)clampi((int)b[k++] - 1, -1, OG_CLIPS - 1);
        r->mutes = (uint16_t)(b[k] | (b[k + 1] << 8)); k += 2;
        r->bars = (uint8_t)clampi(b[k++], 1, 64);
        r->reps = (uint8_t)clampi(b[k++], 1, 16);
        int tempo = b[k] | (b[k + 1] << 8); k += 2;
        r->tempo = (uint16_t)(tempo ? clampi(tempo, 30, 300) : 0);
        int sw = b[k++];
        r->swing = (uint8_t)(sw ? clampi(sw, 50, 80) : 0);
    }
    g->songs[song] = s;
    return 1;
}
