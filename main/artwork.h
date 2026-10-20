#pragma once
/* the artwork of the day: one of the Metropolitan Museum's highlighted
 * paintings (public domain, no API key), a new one each day, with Wikipedia's
 * summary of it. A task fetches it once Wi-Fi is up */
#include <stdbool.h>
#include <stdint.h>

#define ART_W 600   /* the picture fits in this box */
#define ART_H 460

typedef struct {
    bool random;                   /* "Another", not the day's */
    char title[160], artist[96], bio[128], date[64], medium[128], dims[96];
    char museum[200];              /* department and credit line */
    char about[1600];              /* Wikipedia's summary; "" if none */
    const char *about_of;          /* of "the painting", or (it has no page) of "the artist" */
    const uint16_t *pixels;        /* RGB565, w x h, rows of stride bytes */
    int w, h, stride;
} artwork_t;

/* a: a new artwork, or NULL; status: what's going on or wrong, or NULL. Runs
 * in the artwork task: take the LVGL lock to touch the UI. a->pixels stays
 * valid until the callback after next */
typedef void (*artwork_cb_t)(const artwork_t *a, const char *status);

void artwork_start(artwork_cb_t cb);
/* a random painting instead (the day's again tomorrow) */
void artwork_another(void);
/* the picture on show into "Artwork of the Day" on the card (and into the
 * slideshow's index and list); the result comes as a status */
void artwork_save(void);
