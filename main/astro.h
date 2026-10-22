#pragma once
/* the sun and the moon for a place and a day: rise and set, golden hours, the
 * moon's phase, the next full and new moon, the next solstice or equinox.
 * Paul Schlyter's low-precision formulas (an arcminute or so): times within
 * a minute or two. Plain C: no clock or time zone here, the caller passes
 * UTC instants (time_t) */
#include <stdbool.h>
#include <time.h>

typedef struct {
    /* the sun; 0: it doesn't happen that day (polar day or night) */
    time_t sunrise, sunset, noon;
    double noon_alt;                       /* degrees, at solar noon */
    time_t golden_am[2], golden_pm[2];     /* the sun between -4 and +6 degrees */
    bool up_all_day;                       /* no rise or set: above the horizon all day */
    /* the moon */
    time_t moonrise, moonset;              /* 0: not that day */
    double moon_lit;                       /* the lit fraction, 0..1 */
    double moon_elong;                     /* degrees east of the sun, 0..360: < 180 waxing */
    time_t next_full, next_new;
    /* the next solstice or equinox: the sun's longitude at season * 90 degrees
       (0: March equinox, 1: June solstice, 2: September equinox, 3: December solstice) */
    time_t next_season;
    int season;
} astro_day_t;

/* the day from day_start to day_end (local midnights, as UTC instants); now
 * for the moon's phase and the "next" ones. lat, lon in degrees, east positive */
void astro_day(double lat, double lon, time_t day_start, time_t day_end, time_t now, astro_day_t *out);
/* the sun's altitude, degrees (geometric: no refraction) */
double astro_sun_alt(double lat, double lon, time_t t);
/* where the sun and the moon are in the sky at t: altitude (degrees,
 * geometric) and azimuth (degrees from north, east 90) */
void astro_sun_pos(double lat, double lon, time_t t, double *alt, double *az);
void astro_moon_pos(double lat, double lon, time_t t, double *alt, double *az);
/* the moon's elongation east of the sun at t, degrees 0..360 (< 180:
 * waxing), and its lit fraction 0..1 */
double astro_moon_phase(time_t t, double *lit);
