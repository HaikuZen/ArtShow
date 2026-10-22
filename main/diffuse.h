#pragma once
/* diffusion effects on a picture: error-diffusion dithering (Floyd-Steinberg,
 * Atkinson) to a few inks, anisotropic diffusion (Perona-Malik: flat areas
 * melt, edges stay: a painted look), and Gray-Scott reaction-diffusion (a
 * coral growing over the picture, dense where it's dark). Plain C, no
 * ESP-IDF: it renders on a computer too. Each runs a slice at a time, so the
 * page can show it happen */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint16_t *src;   /* w x h RGB565, rows of w: the picture (left as is) */
    uint16_t *dst;         /* w x h RGB565: the effect, painted as it goes */
    int w, h;
    void *mem;             /* the effect's scratch, mem_size(w, h) bytes, zeroed (PSRAM) */
    uint32_t seed;
    int done, total;       /* progress, for the screen */
    char what[48];         /* "rows", "passes", "steps" */
} diffuse_t;

typedef struct {
    const char *name;      /* for the button and the saved file */
    size_t (*mem_size)(int w, int h);
    void (*start)(diffuse_t *d);
    /* work until ms() passes deadline (milliseconds); true: finished */
    bool (*step)(diffuse_t *d, uint32_t (*ms)(void), uint32_t deadline);
} diffuse_fx_t;

extern const diffuse_fx_t diffuse_fx[];
extern const int diffuse_nfx;
