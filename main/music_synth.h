#pragma once
/* generative music: a small synthesizer (FM voices, a pad, an echo) and a
 * sequencer that improvises in a scale. Plain C, no ESP-IDF: it renders on a
 * computer too. One instance; music.c plays it */
#include <stddef.h>
#include <stdint.h>

#define SYNTH_STYLES 6   /* one per generative piece: gen_algos' order */

/* a new piece of music: style (the picture's kind) sets the scale, the
 * tempo and the sounds; pal (5 x 0xRRGGBB, the picture's colours) the key;
 * seed the melody. desc: "D lydian, 72 bpm" */
void synth_set(uint32_t seed, int style, const uint32_t *pal, char *desc, size_t n);
/* desc only, the same as synth_set's: it touches nothing (another task may
 * be rendering) */
void synth_describe(uint32_t seed, int style, const uint32_t *pal, char *desc, size_t n);
/* n mono samples at rate (AUDIO_RATE); silence till synth_set */
void synth_render(int16_t *out, int n, int rate);
