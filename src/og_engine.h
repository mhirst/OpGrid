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
    /* Pages 4-6: LFO 1-3 (8 params each, same layout) */
    P_L1DEST, P_L1WAVE, P_L1SYNC, P_L1RATE, P_L1DPTH, P_L1TRIG, P_L1PHAS, P_L1FADE,
    P_L2DEST, P_L2WAVE, P_L2SYNC, P_L2RATE, P_L2DPTH, P_L2TRIG, P_L2PHAS, P_L2FADE,
    P_L3DEST, P_L3WAVE, P_L3SYNC, P_L3RATE, P_L3DPTH, P_L3TRIG, P_L3PHAS, P_L3FADE,
    /* Page 7: sends and MIDI out */
    P_SDLY, P_SREV, P_SCHO, P_MCH, P_MOUT, P_MACH, P_RSV2, P_RSV3,
    P_COUNT_USED
};

/* Machines. Param ids 0..11 mean different things per machine (ranges from
 * og_machine_max); everything from P_DTUN on is shared. */
enum { OG_MACH_POLY = 0, OG_MACH_PERC, OG_MACH_MORPH, OG_MACH_STACK, OG_MACHINES };
#define OG_MACH_PARAMS   12
int   og_machine_default(int machine, int pid);
int   og_machine_max(int machine, int pid);

#define OG_LFOS          3
#define OG_LFO_PARAMS    8
#define P_LFO(i, k)      (P_L1DEST + (i) * OG_LFO_PARAMS + (k))
enum { LFO_DEST = 0, LFO_WAVE, LFO_SYNC, LFO_RATE, LFO_DPTH, LFO_TRIG, LFO_PHAS, LFO_FADE };
enum { OG_WAVE_SINE = 0, OG_WAVE_TRI, OG_WAVE_SQR, OG_WAVE_SAW, OG_WAVE_RAMP, OG_WAVE_RND, OG_WAVES };
#define OG_LFO_SYNC_RATES 17
/* LFO destinations: 0 off, 1 pitch, 2..33 = params 0..31, 34..36 = sends */
#define OG_DEST_OFF      0
#define OG_DEST_PITCH    1
#define OG_DEST_PARAM0   2
#define OG_DEST_SEND0    34
#define OG_DESTS         37

/* MIDI out modes for P_MOUT */
enum { OG_MOUT_INT = 0, OG_MOUT_EXT, OG_MOUT_BOTH, OG_MOUTS };

/* Global FX parameters (not per track, not lockable). Values 0..max. */
enum {
    FX_DTIM = 0, FX_DFB, FX_DPP, FX_DFLT, FX_RSIZ, FX_RDEC, FX_RPRE, FX_RDMP,
    FX_CDEP, FX_CRAT, FX_CWID, FX_MTHR, FX_MRAT, FX_MGAIN, FX_MDRV, FX_MVOL,
    FX_COUNT
};
#define OG_DELAY_TIMES   13

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

/* Clips and scenes. Step/lock/clip-setting calls above act on the track's
 * EDIT clip, which is independent of the clip that is playing. */
enum { OG_CLIP_EMPTY = 0, OG_CLIP_STOPPED, OG_CLIP_QUEUED, OG_CLIP_PLAYING, OG_CLIP_STOPPING };
#define OG_LAUNCH_QS 6          /* off, 1 step, 1 beat, 1 bar, 2 bars, 4 bars */
void  og_clip_launch(og_t *g, int track, int clip);
void  og_track_stop(og_t *g, int track);
void  og_scene_launch(og_t *g, int scene);
void  og_stop_all(og_t *g);
void  og_set_launch_quant(og_t *g, int q);
int   og_get_launch_quant(const og_t *g);
int   og_clip_state(const og_t *g, int track, int clip);
int   og_playing_clip(const og_t *g, int track);
void  og_set_edit_clip(og_t *g, int track, int clip);
int   og_get_edit_clip(const og_t *g, int track);
void  og_clip_copy(og_t *g, int st, int sc, int dt, int dc);
void  og_clip_clear(og_t *g, int track, int clip);
void  og_clip_notes(const og_t *g, int track, uint64_t out[2]);   /* notes used in the edit clip */
int   og_clip_remove_note(og_t *g, int track, int note);          /* returns steps changed */
void  og_clip_clear_steps(og_t *g, int track);                    /* keeps length/speed/swing */

/* Recording. Live notes go through og_live_note_on/off so they can be
 * recorded into the playing clip of the record track. */
enum { OG_CLICK_OFF = 0, OG_CLICK_COUNTIN, OG_CLICK_ON };
void  og_rec_arm(og_t *g, int on);           /* snapshots the take for undo */
int   og_rec_armed(const og_t *g);
int   og_rec_quant(const og_t *g);
int   og_rec_mode(const og_t *g);
void  og_rec_set_track(og_t *g, int track);
void  og_rec_set_quant(og_t *g, int on);
void  og_rec_set_mode(og_t *g, int replace);
void  og_set_click(og_t *g, int mode);
int   og_get_click(const og_t *g);
void  og_play_countin(og_t *g);              /* 1 bar of clicks, then play */
int   og_counting_in(const og_t *g);
void  og_live_note_on(og_t *g, int track, int note, int vel);
void  og_live_note_off(og_t *g, int track, int note);
void  og_rec_param(og_t *g, int track, int pid, int value);  /* lock while recording, else base */
void  og_rec_new_clip(og_t *g, int track, int clip);   /* launch an empty clip and record into it */
void  og_rec_close_clip(og_t *g, int track);           /* fix its length to whole bars */
int   og_rec_growing(const og_t *g);                   /* track with a growing clip, or -1 */
int   og_undo(og_t *g);                                /* restore the last take; 0 = nothing */
void  og_reset(og_t *g);                               /* new project: defaults everywhere */

/* Song mode. A song is up to 64 rows; a row says which clip every track
 * plays (or stop), which tracks are muted, for how many bars, how many
 * times, and optionally a tempo and a swing for all tracks. */
#define OG_SONGS         8
#define OG_SONG_ROWS     64
enum { OG_R_SCENE = 0,       /* write-only: set every track from a scene (empty slot = stop) */
       OG_R_BARS, OG_R_REPS, OG_R_TEMPO, OG_R_SWING, OG_R_MUTES,
       OG_R_CLIP0 };         /* OG_R_CLIP0 + track: clip index or -1 = stop */
#define OG_R_FIELDS      (OG_R_CLIP0 + OG_TRACKS)
void  og_song_set(og_t *g, int song, int row, int field, int value);
int   og_song_get(const og_t *g, int song, int row, int field);
int   og_song_scene_of(const og_t *g, int song, int row);   /* scene it matches, or -1 (mixed) */
int   og_song_insert(og_t *g, int song, int at, int scene); /* returns the new row or -1 */
void  og_song_delete(og_t *g, int song, int row);
int   og_song_length(const og_t *g, int song);
void  og_song_set_loop(og_t *g, int song, int on);
int   og_song_loop(const og_t *g, int song);
void  og_song_clear(og_t *g, int song);
void  og_song_mode(og_t *g, int on, int song, int row);    /* on: start at row (now or next bar) */
int   og_song_on(const og_t *g);
void  og_song_jump(og_t *g, int row);                       /* at the next bar */
/* status: on, song, row, bar within the row (0-based), repeat (0-based), capturing */
void  og_song_status(const og_t *g, int out[6]);
void  og_capture(og_t *g, int on, int dest_song);           /* record launches as song rows */
#define OG_SONG_HEX_MAX  (2 * (2 + OG_SONG_ROWS * (OG_TRACKS + 7)))
int   og_song_dump(const og_t *g, int song, char *out, int out_len);
int   og_song_load(og_t *g, int song, const char *hex);

/* Clip serialization (for saving): hex text, at most OG_CLIP_HEX_MAX chars + NUL. */
#define OG_CLIP_HEX_MAX  (2 * (4 + OG_STEPS * 10 + 1 + OG_LOCKS * 3))
int   og_clip_dump(const og_t *g, int track, int clip, char *out, int out_len);
int   og_clip_load(og_t *g, int track, int clip, const char *hex);  /* 1 = ok */

/* Machine of a track: resets ids 0..11 to that machine's defaults. */
void  og_set_machine(og_t *g, int track, int machine);

/* Effects, solo, MIDI out. */
void  og_set_fx(og_t *g, int id, int value);
int   og_get_fx(const og_t *g, int id);
int   og_fx_default(int id);
int   og_fx_max(int id);
void  og_set_solo(og_t *g, int track, int on);
int   og_get_solo(const og_t *g, int track);
/* Called from the audio thread with a 3-byte MIDI message for tracks whose
 * MIDI channel (P_MCH) is set. Must not block. */
typedef void (*og_midi_out_fn)(void *ctx, uint8_t status, uint8_t d1, uint8_t d2);
void  og_set_midi_out(og_t *g, og_midi_out_fn fn, void *ctx);
uint64_t og_lock_steps(const og_t *g, int track);  /* bit s = step s has a lock */
float og_debug_comp_gain(const og_t *g);           /* master compressor gain */

/* Introspection for UI / tests. */
int   og_active_voices(const og_t *g);
int   og_param_default(int pid);
int   og_param_max(int pid);

#endif
