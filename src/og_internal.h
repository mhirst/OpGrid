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
#define OG_WT_BITS   11
#define OG_WT_SIZE   (1 << OG_WT_BITS)
#define OG_WT_LEVELS 6               /* 32, 16, 8, 4, 2, 1 harmonics */
#define OG_WAVES_N   8
#define OG_STACK_MAX 8

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

    /* machine oscillator state */
    float    perc_swp, perc_fm, perc_nz, perc_click, perc_nlp;
    uint32_t nrng;
    float    drift[OG_STACK_MAX];
    float    stk[OG_STACK_MAX];

    /* LFOs */
    float    lfo_ph[OG_LFOS];       /* own phase (retrig mode) 0..1 */
    float    lfo_rnd[OG_LFOS];      /* current random value */
    float    lfo_age;               /* seconds since note start (fade-in) */
    int      ext_only;              /* MIDI-out only: no internal sound */

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
    uint8_t   used;             /* has been edited: shows as a clip */
} og_clip_t;

#define OG_Q_NONE  -2           /* og_trackseq_t.queued: nothing queued */
#define OG_Q_STOP  -1           /*                       stop the track */

typedef struct {
    double   pos;               /* position in steps since play started */
    int      cur_step;          /* last triggered step, -1 when none */
    int      pre;               /* result of the last conditional step */
    int      last_ok;           /* this step's condition (ratchets reuse it) */
    int      mute;
    int      clip;              /* playing clip, -1 = stopped */
    int      queued;            /* OG_Q_NONE, OG_Q_STOP or a clip index */
    double   queue_at;          /* global step position to switch at */
    int      edit;              /* clip the UI edits (steps, locks, settings) */
    unsigned hits;              /* note steps fired (UI flashes pads on change) */
    uint8_t  hit_notes[4];      /* notes of the last fired step */
} og_trackseq_t;

typedef struct {
    int      used;
    int      track, note;
    double   at;                /* track position (steps) to release at */
} og_noteoff_t;

#define OG_NOTEOFFS 128
/* positions accumulate float error: an event within OG_EPS steps of a tick's
 * end belongs to the next tick, the same rule everywhere */
#define OG_EPS 1e-6

typedef struct {
    int8_t   clips[OG_TRACKS];  /* -1 = stop */
    uint16_t mutes;
    uint8_t  bars, reps;        /* 1..64, 1..16 */
    uint16_t tempo;             /* 0 = keep, else 30..300 */
    uint8_t  swing;             /* 0 = clips' own, else 50..80 */
} og_row_t;

typedef struct {
    og_row_t rows[OG_SONG_ROWS];
    uint8_t  len, loop;
} og_song_t;
#define OG_DELAY_MAX_S 2.0f

/* per-tick mix buses */
typedef struct {
    float dry_l[OG_CTRL_RATE], dry_r[OG_CTRL_RATE];
    float dly_l[OG_CTRL_RATE], dly_r[OG_CTRL_RATE];
    float rev_l[OG_CTRL_RATE], rev_r[OG_CTRL_RATE];
    float cho_l[OG_CTRL_RATE], cho_r[OG_CTRL_RATE];
} og_bus_t;

typedef struct {
    int    p[FX_COUNT];
    float *dly[2]; int dly_len, dly_w; float dly_cur, dly_lp[2], dly_hp[2];
    float *pre; int pre_len, pre_w;
    float *rev[4]; int rev_cap[4], rev_w[4]; float rev_lp[4];
    float *ap[2]; int ap_len[2], ap_w[2];
    float *cho[2]; int cho_len, cho_w; float cho_ph;
    float  comp_env, comp_gain, last_gr;
} og_fx_t;

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
    double   gpos;              /* global position in steps (1x speed) */
    int      launch_q;          /* launch quantize index */

    /* song mode */
    og_song_t songs[OG_SONGS];
    int      song_on, song, song_row, song_bar, song_rep, song_jump, song_start_pending;
    int      swing_override;    /* 0 = none */
    int      capturing, cap_song;
    long     last_bar;          /* last bar boundary processed */

    /* recording */
    int      rec, rec_track, rec_quant, rec_mode, click;
    double   countin;           /* steps of count-in left (0 = none) */
    int      rec_grow;          /* track whose new clip is still growing, -1 none */
    long     rec_mark[OG_STEPS];/* absolute step index each step was last recorded at */
    struct { int on, track, step; double start; } rec_hold[128];
    og_clip_t undo_clip;
    int      undo_t, undo_c, undo_ok;
    uint32_t click_ph, click_inc;
    float    click_amp;
    int      solo[OG_TRACKS];
    float    lfo_free[OG_TRACKS][OG_LFOS];  /* free-running LFO phases */
    float    lfo_free_rnd[OG_TRACKS][OG_LFOS];
    og_midi_out_fn midi_out;
    void    *midi_ctx;
    og_fx_t  fx;
    float    bpm;
    uint32_t rng;

    /* test-only event log (off unless og_debug_log(g, 1)) */
    int      log_on, log_n;
    uint64_t sample_clock;
    og_event_t log[OG_LOG_MAX];

    float    sine[SINE_SIZE + 1];
    float    wt[OG_WAVES_N][OG_WT_LEVELS][OG_WT_SIZE + 1];
    float    att_inc[128];
    float    dec_coef[128];
    float    pan_l[128], pan_r[128];
};

/* engine-internal entry points used by the sequencer */
void og__note_on_locked(og_t *g, int track, int note, int vel,
                        const og_lock_t *locks, int step, int slide);
void og__apply_locks_to_track(og_t *g, int track, const og_lock_t *locks, int step);
void og__seq_init(og_t *g);
void og__wavetables_init(og_t *g);
int  og__pmax(const og_t *g, int track, int pid);
float og__ratio(int idx);
void og__osc_perc(og_t *g, voice_t *v, const int *p, float base, float *osc, int n);
void og__osc_morph(og_t *g, voice_t *v, const int *p, float base, float *osc, int n);
void og__osc_stack(og_t *g, voice_t *v, const int *p, float base, float *osc, int n);
void og__rec_init(og_t *g);
int  og__countin_advance(og_t *g, int frames, double step_samples); /* 1 = still counting */
void og__rec_cross(og_t *g, int track, double start, double end);   /* replace-mode clearing */
void og__click_beats(og_t *g, double gs, double ge);
void og__click_render(og_t *g, float *l, float *r, int n);
void og__clip_reset(og_clip_t *c);
void og__song_init(og_t *g);
void og__song_bar(og_t *g, double gs, double ge);   /* bar boundaries in this tick */
int  og__fx_init(og_t *g);
void og__fx_free(og_t *g);
void og__fx_process(og_t *g, const og_bus_t *b, float *out_l, float *out_r, int n);
int  og__track_audible(const og_t *g, int track);
void og__seq_advance(og_t *g, int frames);

#endif
