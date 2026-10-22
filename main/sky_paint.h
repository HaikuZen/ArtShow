#pragma once
/* the Sun and Moon page's picture: the sky as it is now, painted like a
 * shin-hanga woodblock print. A gradient that follows the sun's altitude
 * (night, blue hour, sunrise, golden hour, day), paper grain, stars at
 * night, the day's path of the sun dotted, the sun and the moon where they
 * are (the moon's phase, its seas, earthshine), and three ridges of hills,
 * the place's own, lighter towards the horizon. Plain C: it paints on a
 * computer too */
#include <stdbool.h>
#include <stdint.h>

#define SKY_TRACK 97   /* the day's path: every 15 minutes */

typedef struct {
    float sun_alt, sun_az;            /* degrees, now; azimuth from north */
    float moon_alt, moon_az;
    float moon_lit, moon_elong;       /* 0..1; degrees east of the sun (< 180 waxing) */
    float lat;                        /* the hemisphere: which way we look, which side is lit */
    uint32_t place;                   /* the hills' seed */
    uint32_t day;                     /* the clouds' seed: other clouds each day */
    int ntrack;                       /* the sun's path today: */
    float track_alt[SKY_TRACK], track_az[SKY_TRACK];
} sky_t;

/* where an azimuth and altitude are on the picture (x may be off it) */
void sky_xy(const sky_t *s, float az, float alt, int w, int h, float *x, float *y);
/* the horizon's height on the picture */
int sky_horizon(int h);
/* the whole picture: w x h RGB565 */
void sky_paint(const sky_t *s, uint16_t *px, int w, int h);
/* a small moon (the caption's, there whether it's up or not) at cx, cy */
void sky_paint_moon(const sky_t *s, uint16_t *px, int w, int h, float cx, float cy, float r);
