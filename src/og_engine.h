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

/* Introspection for UI / tests. */
int   og_active_voices(const og_t *g);
int   og_param_default(int pid);
int   og_param_max(int pid);

#endif
