/*
 * OpGrid engine internals shared by og_engine.c (voices) and og_seq.c
 * (sequencer). Not part of the public API.
 */
#ifndef OG_INTERNAL_H
#define OG_INTERNAL_H

#include <stdint.h>
#include "og_engine.h"

#define SINE_BITS   12
#define SINE_SIZE   (1 << SINE_BITS)

enum { ENV_IDLE = 0, ENV_ATT, ENV_DEC, ENV_SUS, ENV_REL };

typedef struct {
    int   stage;
    float level;
} env_t;

typedef struct {
    int      active;
    int      track, note, vel;
    int      gate;
    uint32_t age;

    int      p[P_COUNT_USED];   /* this voice's params: base + step locks */
    uint64_t lockmask;          /* params held by a lock (base edits skip them) */
    int      slide;             /* glide into this note regardless of GLID */

    uint32_t ph[5];
    float    fb1, fb2;
    float    pitch, target;

    env_t    amp, flt;
    float    mod_env[5];
    float    amp_out;
    float    ic1[2], ic2[2];

    int      fade;
    int      pending;
    int      pend_track, pend_note, pend_vel, pend_gate, pend_slide;
    int      pend_p[P_COUNT_USED];
    uint64_t pend_lockmask;
} voice_t;

/* ---- sequencer data ---- */

typedef struct {
    uint8_t flags;              /* OG_STEP_* */
    uint8_t notes[OG_CHORD];    /* 0 = unused slot (note 0 is not playable) */
    uint8_t vel;
    uint8_t len;                /* index into length table */
    uint8_t rat;                /* 1..8 */
    uint8_t cond;               /* OG_COND_* id */
    int8_t  micro;              /* -23..23 (48ths of a step) */
} og_step_t;

typedef struct {
    uint8_t used, step, pid, val;
} og_lock_t;

typedef struct {
    og_step_t steps[OG_STEPS];
    og_lock_t locks[OG_LOCKS];
    uint8_t   length;           /* 1..64 */
    uint8_t   speed;            /* index into speed table */
    uint8_t   swing;            /* 50..80 % */
} og_clip_t;

typedef struct {
    double   pos;               /* position in steps since play started */
    int      cur_step;          /* last triggered step, -1 when none */
    int      pre;               /* result of the last conditional step */
    int      last_ok;           /* this step's condition (ratchets reuse it) */
    int      mute;
    int      clip;              /* active clip index */
} og_trackseq_t;

typedef struct {
    int      used;
    int      track, note;
    double   at;                /* track position (steps) to release at */
} og_noteoff_t;

#define OG_NOTEOFFS 128

struct og {
    float    sr, inv_sr;
    int      params[OG_TRACKS][P_COUNT_USED];
    float    last_pitch[OG_TRACKS];
    float    master;
    uint32_t clock;
    voice_t  v[OG_VOICES];

    /* sequencer */
    og_clip_t     clips[OG_TRACKS][OG_CLIPS];
    og_trackseq_t ts[OG_TRACKS];
    og_noteoff_t  offs[OG_NOTEOFFS];
    int      playing;
    int      fill;
    float    bpm;
    uint32_t rng;

    /* test-only event log (off unless og_debug_log(g, 1)) */
    int      log_on, log_n;
    uint64_t sample_clock;
    og_event_t log[OG_LOG_MAX];

    float    sine[SINE_SIZE + 1];
    float    att_inc[128];
    float    dec_coef[128];
    float    pan_l[128], pan_r[128];
};

/* engine-internal entry points used by the sequencer */
void og__note_on_locked(og_t *g, int track, int note, int vel,
                        const og_lock_t *locks, int step, int slide);
void og__apply_locks_to_track(og_t *g, int track, const og_lock_t *locks, int step);
void og__seq_init(og_t *g);
void og__seq_advance(og_t *g, int frames);

#endif
