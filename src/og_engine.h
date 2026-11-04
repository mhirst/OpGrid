/*
 * OpGrid engine — FM groovebox core.
 *
 * Pure C, no host dependency, no allocation after og_create(). Every
 * og_* call below is safe to make from the audio thread.
 */
#ifndef OG_ENGINE_H
#define OG_ENGINE_H

#include <stdint.h>

#define OG_TRACKS        16
#define OG_VOICES        12
#define OG_PARAMS        64      /* per-track parameter slots (0..127 each) */
#define OG_PAGE_PARAMS   8
#define OG_SAMPLE_RATE   44100
#define OG_CTRL_RATE     16      /* samples per control tick */

/* Sequencer sizes */
#define OG_STEPS         64
#define OG_LOCKS         64      /* parameter locks per clip */
#define OG_CHORD         4       /* notes per step */
#define OG_CLIPS         8       /* clip slots per track */

/* Step flags */
#define OG_STEP_NOTE     0x01    /* plays its notes */
#define OG_STEP_LOCK     0x02    /* lock-only: changes sound, no new note */
#define OG_STEP_SLIDE    0x04    /* glide into this step's note */

/* Step conditions: 0 none, 1-6 fill/pre/first (and their negations),
 * 7-41 A:B loop conditions (B = 2..8, A = 1..B), 42-60 probability 5..95%. */
#define OG_COND_NONE     0
#define OG_COND_FILL     1
#define OG_COND_NFILL    2
#define OG_COND_PRE      3
#define OG_COND_NPRE     4
#define OG_COND_FIRST    5
#define OG_COND_NFIRST   6
#define OG_COND_AB0      7
#define OG_COND_PROB0    42
#define OG_CONDS         61

#define OG_LENS          13      /* note length table size */
#define OG_SPEEDS        7       /* clip speed table size */
#define OG_SPEED_1X      4
#define OG_LEN_1STEP     4

/* Parameter ids. Pages of 8, in UI order. Values are 0..127. */
enum {
    /* Page 0: FM (machine page 1) */
    P_ALGO = 0, P_RAT1, P_RAT2, P_RAT3, P_RAT4, P_LVL2, P_LVL3, P_LVL4,
    /* Page 1: FM2 (machine page 2) */
    P_FDBK, P_DEC2, P_DEC3, P_DEC4, P_DTUN, P_VELM, P_TUNE, P_GLID,
    /* Page 2: Filter */
    P_FREQ, P_RESO, P_FTYP, P_FENV, P_FATK, P_FDEC, P_FSUS, P_FREL,
    /* Page 3: Amp */
    P_AATK, P_ADEC, P_ASUS, P_AREL, P_DRIV, P_PAN, P_VOL, P_VELA,
    P_COUNT_USED
};

#define OG_PAGES (P_COUNT_USED / OG_PAGE_PARAMS)

enum { OG_FILTER_LP12 = 0, OG_FILTER_LP24, OG_FILTER_HP, OG_FILTER_BP,
       OG_FILTER_NOTCH, OG_FILTER_TYPES };

#define OG_ALGOS 8

typedef struct og og_t;

/* Lifecycle (allocation happens only here, off the audio path). */
og_t *og_create(float sample_rate);
void  og_destroy(og_t *g);

/* Audio: render interleaved stereo int16. */
void  og_render(og_t *g, int16_t *out_lr, int frames);
/* Same, float output in [-1,1] (used by tests). */
void  og_render_float(og_t *g, float *out_lr, int frames);

/* Notes. velocity 1..127; note_off with velocity ignored. */
void  og_note_on(og_t *g, int track, int note, int velocity);
void  og_note_off(og_t *g, int track, int note);
void  og_all_notes_off(og_t *g);

/* Sound parameters. */
void  og_set_param(og_t *g, int track, int pid, int value);
int   og_get_param(const og_t *g, int track, int pid);
void  og_set_master(og_t *g, int value);   /* 0..127 */

/* Sequencer. Step fields: see og_step_set(). All step/clip calls act on
 * the track's active clip. */
enum { OG_F_FLAGS = 0, OG_F_NOTE0, OG_F_NOTE1, OG_F_NOTE2, OG_F_NOTE3,
       OG_F_VEL, OG_F_LEN, OG_F_RAT, OG_F_COND, OG_F_MICRO, OG_FIELDS };

void  og_play(og_t *g, int on);
int   og_is_playing(const og_t *g);
void  og_set_tempo(og_t *g, float bpm);
float og_get_tempo(const og_t *g);
void  og_set_fill(og_t *g, int on);
void  og_set_mute(og_t *g, int track, int on);
int   og_get_mute(const og_t *g, int track);

void  og_step_set(og_t *g, int track, int step, int field, int value);
int   og_step_get(const og_t *g, int track, int step, int field);
void  og_step_clear(og_t *g, int track, int step);
int   og_lock_set(og_t *g, int track, int step, int pid, int value); /* 0 = pool full */
void  og_lock_clear(og_t *g, int track, int step, int pid);
int   og_lock_get(const og_t *g, int track, int step, int pid);       /* -1 = none */
int   og_lock_count(const og_t *g, int track);

void  og_clip_set_length(og_t *g, int track, int len);
void  og_clip_set_speed(og_t *g, int track, int speed);
void  og_clip_set_swing(og_t *g, int track, int swing);
int   og_clip_length(const og_t *g, int track);
int   og_clip_speed(const og_t *g, int track);
int   og_clip_swing(const og_t *g, int track);
int   og_track_step(const og_t *g, int track);   /* playing step or -1 */

float og_len_steps(int idx);
float og_speed_mul(int idx);

/* Testing: sequencer event log (sample time quantized to the 16-sample tick). */
#define OG_LOG_MAX 4096
typedef struct { uint64_t t; int track, note, vel, on; } og_event_t;
void  og_debug_log(og_t *g, int enable);           /* also clears the log */
int   og_debug_events(const og_t *g, const og_event_t **out);
int   og_debug_voice_param(const og_t *g, int track, int pid); /* newest voice, -1 none */

/* Introspection for UI / tests. */
int   og_active_voices(const og_t *g);
int   og_param_default(int pid);
int   og_param_max(int pid);

#endif
