#pragma once
/* generative art: algorithms that paint into a screen-sized RGB565 canvas a
 * little at a time (the Generative page calls step() in slices). Each is
 * deterministic from its seed: a caption's number makes the same picture */
#include <stdbool.h>
#include <stdint.h>

#define GEN_COLORS 5

typedef struct {
    uint16_t *px;                 /* w x h RGB565, rows of w */
    int w, h;
    uint32_t pal[GEN_COLORS];     /* 0xRRGGBB, darkest (the ground) first */
    uint32_t seed;
    char name[48];                /* "Clifford attractor 9273": the saved file's name */
    char caption[160];            /* the settings, for the screen */
} gen_canvas_t;

typedef struct {
    const char *title;
    void (*start)(gen_canvas_t *c);              /* the ground, the settings, name and caption */
    bool (*step)(gen_canvas_t *c, int budget_ms);   /* paint for about budget_ms; true: finished */
} gen_algo_t;

extern const gen_algo_t gen_algos[];
extern const int gen_nalgos;
