/* astro.c - the sun and the moon by Paul Schlyter's "How to compute planetary
 * positions" (stjarnhimlen.se/comp/ppcomp.html): orbital elements plus the
 * moon's main perturbations. Rise, set and golden hours: the altitude
 * sampled every 10 minutes, each crossing refined by bisection. */
#include "astro.h"
#include <math.h>

#define RAD (M_PI / 180.0)
#define DEG (180.0 / M_PI)
#define STEP 600   /* s: the sampling of a day */

static double rev(double x) { return x - 360.0 * floor(x / 360.0); }      /* 0..360 */
static double wrap180(double x) { return rev(x + 180.0) - 180.0; }        /* -180..180 */

/* days since 2000 Jan 0.0 UT (Schlyter's d) */
static double days(time_t t) { return (t - 946598400.0) / 86400.0; }

typedef struct {
    double ra, dec;    /* degrees */
    double lon, lat;   /* ecliptic, degrees */
    double r;          /* the moon: Earth radii */
} pos_t;

static void to_equatorial(double lon, double lat, double ecl, pos_t *p) {
    double x = cos(lon * RAD) * cos(lat * RAD), y = sin(lon * RAD) * cos(lat * RAD), z = sin(lat * RAD);
    double ye = y * cos(ecl * RAD) - z * sin(ecl * RAD), ze = y * sin(ecl * RAD) + z * cos(ecl * RAD);
    p->ra = rev(atan2(ye, x) * DEG);
    p->dec = atan2(ze, sqrt(x * x + ye * ye)) * DEG;
}

static double obliquity(double d) { return 23.4393 - 3.563e-7 * d; }

/* the sun's mean anomaly and argument of perihelion */
static void sun_elements(double d, double *M, double *w) {
    *w = 282.9404 + 4.70935e-5 * d;
    *M = rev(356.0470 + 0.9856002585 * d);
}

static void sun(double d, pos_t *p) {
    double M, w, e = 0.016709 - 1.151e-9 * d;
    sun_elements(d, &M, &w);
    double E = M + e * DEG * sin(M * RAD) * (1.0 + e * cos(M * RAD));
    double xv = cos(E * RAD) - e, yv = sqrt(1.0 - e * e) * sin(E * RAD);
    p->lon = rev(atan2(yv, xv) * DEG + w);
    p->lat = 0;
    p->r = sqrt(xv * xv + yv * yv);
    to_equatorial(p->lon, 0, obliquity(d), p);
}

static void moon(double d, pos_t *p) {
    double N = rev(125.1228 - 0.0529538083 * d), i = 5.1454, w = rev(318.0634 + 0.1643573223 * d);
    double a = 60.2666, e = 0.054900, M = rev(115.3654 + 13.0649929509 * d);
    double E = M + e * DEG * sin(M * RAD) * (1.0 + e * cos(M * RAD));
    for (int k = 0; k < 5; k++) E -= (E - e * DEG * sin(E * RAD) - M) / (1.0 - e * cos(E * RAD));
    double xv = a * (cos(E * RAD) - e), yv = a * sqrt(1.0 - e * e) * sin(E * RAD);
    double v = atan2(yv, xv) * DEG, r = sqrt(xv * xv + yv * yv), vw = (v + w) * RAD;
    double xh = r * (cos(N * RAD) * cos(vw) - sin(N * RAD) * sin(vw) * cos(i * RAD));
    double yh = r * (sin(N * RAD) * cos(vw) + cos(N * RAD) * sin(vw) * cos(i * RAD));
    double zh = r * sin(vw) * sin(i * RAD);
    double lon = atan2(yh, xh) * DEG, lat = atan2(zh, sqrt(xh * xh + yh * yh)) * DEG;

    /* the perturbations: Schlyter's largest terms */
    double Ms, ws;
    sun_elements(d, &Ms, &ws);
    double Mm = M, Ls = Ms + ws, Lm = M + w + N, D = Lm - Ls, F = Lm - N;
#define S(x) sin((x) * RAD)
#define C(x) cos((x) * RAD)
    lon += -1.274 * S(Mm - 2 * D) + 0.658 * S(2 * D) - 0.186 * S(Ms) - 0.059 * S(2 * Mm - 2 * D)
           - 0.057 * S(Mm - 2 * D + Ms) + 0.053 * S(Mm + 2 * D) + 0.046 * S(2 * D - Ms) + 0.041 * S(Mm - Ms)
           - 0.035 * S(D) - 0.031 * S(Mm + Ms) - 0.015 * S(2 * F - 2 * D) + 0.011 * S(Mm - 4 * D);
    lat += -0.173 * S(F - 2 * D) - 0.055 * S(Mm - F - 2 * D) - 0.046 * S(Mm + F - 2 * D) + 0.033 * S(F + 2 * D)
           + 0.017 * S(2 * Mm + F);
    r += -0.58 * C(Mm - 2 * D) - 0.46 * C(2 * D);
#undef S
#undef C
    p->lon = rev(lon);
    p->lat = lat;
    p->r = r;
    to_equatorial(p->lon, lat, obliquity(d), p);
}

/* the altitude of ra/dec, degrees */
static double altitude(const pos_t *p, double lat, double lon, time_t t) {
    double jd = t / 86400.0 + 2440587.5;
    double lst = rev(280.46061837 + 360.98564736629 * (jd - 2451545.0) + lon);   /* local sidereal time */
    double ha = (lst - p->ra) * RAD;
    return asin(sin(lat * RAD) * sin(p->dec * RAD) + cos(lat * RAD) * cos(p->dec * RAD) * cos(ha)) * DEG;
}

double astro_sun_alt(double lat, double lon, time_t t) {
    pos_t p;
    sun(days(t), &p);
    return altitude(&p, lat, lon, t);
}

/* the moon's altitude less its rise/set altitude (parallax, refraction,
   semi-diameter: about +0.13 degrees) */
static double moon_above(double lat, double lon, time_t t) {
    pos_t p;
    moon(days(t), &p);
    double par = asin(1.0 / p.r) * DEG;
    return altitude(&p, lat, lon, t) - (0.7275 * par - 0.5667);
}

typedef double (*height_fn)(double lat, double lon, time_t t);

/* the first instant in [t0, t1] where f - h crosses zero, upwards (up) or
   downwards; 0 if none */
static time_t crossing(height_fn f, double lat, double lon, double h, bool up, time_t t0, time_t t1) {
    double prev = f(lat, lon, t0) - h;
    for (time_t a = t0; a < t1; a += STEP) {
        time_t b = a + STEP < t1 ? a + STEP : t1;
        double cur = f(lat, lon, b) - h;
        if (up ? (prev < 0 && cur >= 0) : (prev >= 0 && cur < 0)) {
            time_t lo = a, hi = b;   /* f - h is below at lo when up, above when down */
            while (hi - lo > 5) {
                time_t mid = lo + (hi - lo) / 2;
                double m = f(lat, lon, mid) - h;
                if ((m < 0) == up) lo = mid;
                else hi = mid;
            }
            return lo + (hi - lo) / 2;
        }
        prev = cur;
    }
    return 0;
}

/* the moon's elongation east of the sun, degrees 0..360, and its lit fraction */
static double elongation(time_t t, double *lit) {
    pos_t s, m;
    double d = days(t);
    sun(d, &s);
    moon(d, &m);
    double e = rev(m.lon - s.lon);
    if (lit) *lit = (1.0 - cos(e * RAD) * cos(m.lat * RAD)) / 2.0;
    return e;
}

/* the next instant after t when g(t) (degrees) passes target, searching in
   steps of step seconds for up to span; 0 if none */
static time_t next_angle(double (*g)(time_t), double target, time_t t, time_t step, time_t span) {
    double prev = wrap180(g(t) - target);
    for (time_t a = t; a < t + span; a += step) {
        double cur = wrap180(g(a + step) - target);
        if (prev < 0 && cur >= 0 && cur - prev < 90) {   /* a crossing, not the wrap at 180 */
            time_t lo = a, hi = a + step;
            while (hi - lo > 30) {
                time_t mid = lo + (hi - lo) / 2;
                if (wrap180(g(mid) - target) < 0) lo = mid;
                else hi = mid;
            }
            return lo + (hi - lo) / 2;
        }
        prev = cur;
    }
    return 0;
}

static double elong_of(time_t t) { return elongation(t, NULL); }

double astro_moon_phase(time_t t, double *lit) { return elongation(t, lit); }

static double sun_lon(time_t t) {
    pos_t s;
    sun(days(t), &s);
    return s.lon;
}

void astro_day(double lat, double lon, time_t day_start, time_t day_end, time_t now, astro_day_t *o) {
    *o = (astro_day_t){ 0 };
    /* the sun: rise and set at -0.833 degrees (refraction and its radius) */
    o->sunrise = crossing(astro_sun_alt, lat, lon, -0.833, true, day_start, day_end);
    o->sunset = crossing(astro_sun_alt, lat, lon, -0.833, false, day_start, day_end);
    o->golden_am[0] = crossing(astro_sun_alt, lat, lon, -4, true, day_start, day_end);
    o->golden_am[1] = crossing(astro_sun_alt, lat, lon, 6, true, day_start, day_end);
    o->golden_pm[0] = crossing(astro_sun_alt, lat, lon, 6, false, day_start, day_end);
    o->golden_pm[1] = crossing(astro_sun_alt, lat, lon, -4, false, day_start, day_end);
    /* solar noon: the highest, sampled then narrowed */
    time_t best = day_start;
    double top = -90;
    for (time_t t = day_start; t <= day_end; t += STEP) {
        double a = astro_sun_alt(lat, lon, t);
        if (a > top) top = a, best = t;
    }
    for (time_t step = STEP / 2; step >= 5; step /= 2) {
        double l = astro_sun_alt(lat, lon, best - step), r = astro_sun_alt(lat, lon, best + step);
        if (l > top) top = l, best -= step;
        else if (r > top) top = r, best += step;
    }
    o->noon = best;
    o->noon_alt = top;
    o->up_all_day = !o->sunrise && !o->sunset && astro_sun_alt(lat, lon, day_start) > -0.833;

    /* the moon */
    o->moonrise = crossing(moon_above, lat, lon, 0, true, day_start, day_end);
    o->moonset = crossing(moon_above, lat, lon, 0, false, day_start, day_end);
    o->moon_elong = elongation(now, &o->moon_lit);
    o->next_full = next_angle(elong_of, 180, now, 6 * 3600, 32 * 86400);
    o->next_new = next_angle(elong_of, 0, now, 6 * 3600, 32 * 86400);

    /* the next quarter of the sun's longitude */
    double l = sun_lon(now);
    o->season = ((int)floor(l / 90.0) + 1) % 4;
    o->next_season = next_angle(sun_lon, o->season * 90.0, now, 86400, 100 * 86400);
}
