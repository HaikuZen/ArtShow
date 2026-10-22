/* sky_paint.c - the painter.
 *  - the view: looking south (north of the equator; north, south of it),
 *    120 degrees of azimuth either side across the width; the horizon at
 *    two thirds, the altitude up to ~75 degrees above it
 *  - the sky: three colours (top, middle, horizon) from a table of the
 *    sun's altitude (-18 night .. 40 day), blended between rows of the
 *    table; a little grain per pixel (paper, and no RGB565 banding)
 *  - the sun: a glow (alpha falling off with the square of the distance)
 *    and a disc, orange low, white high; painted before the hills, which
 *    hide it as it sets
 *  - the moon: a disc lit from the side the sun is on: a pixel is lit when
 *    it's past the terminator, x > (1 - 2 lit) sqrt(1 - y^2), with a soft
 *    edge; a few darker seas; the dark side faintly lit (earthshine) at
 *    night, sky by day
 *  - the hills: three ridges of summed sines (their phases from the place),
 *    each a flat colour between the horizon's and the ink's, lighter at its
 *    top (the printers' bokashi) */
#include "sky_paint.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265f

typedef struct { float r, g, b; } rgb_t;

static rgb_t hex(uint32_t c) { return (rgb_t){ (c >> 16) & 255, (c >> 8) & 255, c & 255 }; }
static rgb_t mix(rgb_t a, rgb_t b, float t) { return (rgb_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static inline uint16_t pack(float r, float g, float b) {
    int R = (int)clampf(r, 0, 255), G = (int)clampf(g, 0, 255), B = (int)clampf(b, 0, 255);
    return (uint16_t)((R >> 3) << 11 | (G >> 2) << 5 | (B >> 3));
}

static inline rgb_t unpack(uint16_t c) { return (rgb_t){ (c >> 11) * 255 / 31.0f, ((c >> 5) & 63) * 255 / 63.0f, (c & 31) * 255 / 31.0f }; }

static inline void blend(uint16_t *p, rgb_t c, float a) {
    if (a <= 0) return;
    if (a >= 1) {
        *p = pack(c.r, c.g, c.b);
        return;
    }
    rgb_t o = unpack(*p);
    *p = pack(o.r + (c.r - o.r) * a, o.g + (c.g - o.g) * a, o.b + (c.b - o.b) * a);
}

static uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352D;
    x ^= x >> 15;
    x *= 0x846CA68B;
    x ^= x >> 16;
    return x;
}

/* the sky by the sun's altitude: top, middle, horizon; the hills' ink */
typedef struct {
    float alt;
    uint32_t top, mid, hor, ink;
} key_t;

static const key_t KEYS[] = {
    { -18, 0x070B1C, 0x0E1430, 0x1B2148, 0x05070F },   /* night */
    { -10, 0x0B1230, 0x1C2350, 0x3A3468, 0x0A0B1C },   /* the last of the light */
    { -5, 0x1A2558, 0x4A4580, 0xB0708A, 0x161630 },    /* blue hour, a rose horizon */
    { 0, 0x2E4A8C, 0x8A6E9A, 0xF08A50, 0x241E36 },     /* sunrise, sunset */
    { 6, 0x3E6AB0, 0xC9A2A0, 0xF6C27A, 0x2E2E3E },     /* golden hour */
    { 15, 0x4A82C8, 0x9CC0DE, 0xE8DCC0, 0x2F4A4A },
    { 40, 0x3A78C8, 0x86B4E0, 0xD6E6EE, 0x2A4A44 },    /* day */
};
#define NKEYS (int)(sizeof KEYS / sizeof KEYS[0])

static void palette(float alt, rgb_t *top, rgb_t *mid, rgb_t *hor, rgb_t *ink) {
    int k = 0;
    while (k < NKEYS - 2 && alt > KEYS[k + 1].alt) k++;
    float t = clampf((alt - KEYS[k].alt) / (KEYS[k + 1].alt - KEYS[k].alt), 0, 1);
    *top = mix(hex(KEYS[k].top), hex(KEYS[k + 1].top), t);
    *mid = mix(hex(KEYS[k].mid), hex(KEYS[k + 1].mid), t);
    *hor = mix(hex(KEYS[k].hor), hex(KEYS[k + 1].hor), t);
    *ink = mix(hex(KEYS[k].ink), hex(KEYS[k + 1].ink), t);
}

int sky_horizon(int h) { return h * 2 / 3; }

void sky_xy(const sky_t *s, float az, float alt, int w, int h, float *x, float *y) {
    float centre = s->lat >= 0 ? 180 : 0;   /* the sun's side of the sky */
    float d = fmodf(az - centre + 540, 360) - 180;
    if (s->lat < 0) d = -d;   /* looking north, east is on the right */
    *x = w / 2.0f + d / 120 * (w / 2.0f);
    int hz = sky_horizon(h);
    *y = hz - alt / 75 * (hz - 40);
}

static void disc(uint16_t *px, int w, int h, float cx, float cy, float r, rgb_t c, float a) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) {
        if (y < 0 || y >= h) continue;
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            if (x < 0 || x >= w) continue;
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            blend(&px[y * w + x], c, a * clampf(r - d + 0.5f, 0, 1));   /* a 1-pixel soft edge */
        }
    }
}

static void glow(uint16_t *px, int w, int h, float cx, float cy, float r, rgb_t c, float a) {
    for (int y = (int)(cy - r); y <= (int)(cy + r); y++) {
        if (y < 0 || y >= h) continue;
        for (int x = (int)(cx - r); x <= (int)(cx + r); x++) {
            if (x < 0 || x >= w) continue;
            float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r;
            if (d < 1) blend(&px[y * w + x], c, a * (1 - d) * (1 - d));
        }
    }
}

/* the moon's seas, in the disc's own coordinates (x right, y down, radius 1) */
static const float SEAS[][3] = {
    { -0.28f, -0.30f, 0.42f }, { 0.08f, -0.42f, 0.30f }, { 0.30f, -0.12f, 0.34f },
    { 0.40f, 0.25f, 0.26f },   { -0.42f, 0.22f, 0.34f }, { 0.00f, 0.10f, 0.22f },
};

void sky_paint_moon(const sky_t *s, uint16_t *px, int w, int h, float cx, float cy, float r) {
    bool right = (s->moon_elong < 180) == (s->lat >= 0);   /* the lit side */
    float c = 1 - 2 * s->moon_lit;
    bool night = s->sun_alt < -4;
    rgb_t lit = hex(0xEFE8D0), sea = hex(0xC9C2AE), shine = hex(0x2A3354);
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) {
        if (y < 0 || y >= h) continue;
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            if (x < 0 || x >= w) continue;
            float nx = (x + 0.5f - cx) / r, ny = (y + 0.5f - cy) / r, d2 = nx * nx + ny * ny;
            float edge = clampf((1 - sqrtf(d2)) * r + 0.5f, 0, 1);   /* inside the disc */
            if (edge <= 0) continue;
            float sx = right ? nx : -nx, half = sqrtf(fmaxf(1 - ny * ny, 0));
            float l = clampf((sx - c * half) / 0.08f + 0.5f, 0, 1);   /* past the terminator, softly */
            rgb_t col = lit;
            for (int k = 0; k < (int)(sizeof SEAS / sizeof SEAS[0]); k++) {
                float dx = nx - SEAS[k][0], dy = ny - SEAS[k][1], q = (dx * dx + dy * dy) / (SEAS[k][2] * SEAS[k][2]);
                if (q < 1) col = mix(col, sea, (1 - q) * (1 - q) * 0.75f);   /* soft-edged seas */
            }
            col = mix(col, hex(0xD8D0B8), d2 * 0.5f);   /* darker to the limb */
            if (night) {
                blend(&px[y * w + x], mix(shine, col, l), edge);
            } else {   /* by day the dark side is the sky's */
                blend(&px[y * w + x], col, edge * l * 0.9f);
                blend(&px[y * w + x], hex(0xFFFFFF), edge * (1 - l) * 0.06f);
            }
        }
    }
}

/* the grain: 16 levels in a 64 x 64 tile; a colour's 16 grained versions
 * are packed once, then each pixel only picks one (no float per pixel) */
static uint8_t s_grain[64 * 64];

static void grained(rgb_t c, float amount, uint16_t out[16]) {
    for (int n = 0; n < 16; n++) {
        float d = (n - 7.5f) * amount;
        out[n] = pack(c.r + d, c.g + d, c.b + d);
    }
}

void sky_paint(const sky_t *s, uint16_t *px, int w, int h) {
    rgb_t top, mid, hor, ink;
    palette(s->sun_alt, &top, &mid, &hor, &ink);
    const int hz = sky_horizon(h);
    if (!s_grain[0] && !s_grain[1])
        for (int i = 0; i < 64 * 64; i++) s_grain[i] = hash(i * 2654435761u + 7) & 15;

    /* the sky, a little grain on it */
    for (int y = 0; y < h; y++) {
        float t = (float)y / hz;
        rgb_t c = t < 0.55f ? mix(top, mid, t / 0.55f) : mix(mid, hor, clampf((t - 0.55f) / 0.45f, 0, 1));
        uint16_t v[16], *row = px + y * w;
        const uint8_t *g = s_grain + (y & 63) * 64;
        grained(c, 0.6f, v);
        for (int x = 0; x < w; x++) row[x] = v[g[x & 63]];
    }

    /* stars, fading in as the sun goes down */
    float stars = clampf((-s->sun_alt - 6) / 8, 0, 1);
    if (stars > 0)
        for (int k = 0; k < 320; k++) {
            uint32_t r = hash(k * 7919 + 17);
            int x = r % w, y = (r >> 12) % (hz - 30);
            float b = (float)((r >> 24) & 255) / 255;
            float a = stars * (0.25f + 0.75f * b * b) * (1 - (float)y / hz * 0.6f);
            if (b > 0.93f) disc(px, w, h, x + 0.5f, y + 0.5f, 1.4f, hex(0xFFF6E0), a);
            else blend(&px[y * w + x], hex(0xF0F0FF), a);
        }

    /* clouds: long thin streaks, other ones each day, lit from below near sunrise and sunset */
    for (int k = 0; k < 6; k++) {
        uint32_t r = hash(s->day * 131 + k * 7 + 3);
        float cx = (float)(r % w), cy = 60 + (float)((r >> 10) % (hz - 160)), len = 140 + (float)((r >> 20) % 260);
        float th = 5 + (float)((r >> 4) % 9);
        rgb_t body = mix(mid, hex(0xFFFFFF), s->sun_alt > 10 ? 0.55f : 0.2f), under = mix(hor, hex(0xFFE0C0), 0.3f);
        float warm = clampf(1 - fabsf(s->sun_alt - 2) / 12, 0, 1);   /* sunrise and sunset light them from below */
        float a0 = s->sun_alt < -10 ? 0.25f : 0.6f;
        for (int y = (int)(cy - th); y <= (int)(cy + th); y++) {
            if (y < 0 || y >= hz) continue;
            float v = (y - cy) / th;   /* -1 top .. 1 underside */
            rgb_t c = mix(body, under, warm * clampf(v * 0.5f + 0.5f, 0, 1));
            for (int x = (int)(cx - len); x <= (int)(cx + len); x++) {
                if (x < 0 || x >= w) continue;
                float u = (x - cx) / len, e = 1 - u * u - v * v * 0.9f;   /* a long ellipse, ragged a little */
                e += ((int)(hash(x * 31 + k) & 15) - 7.5f) * 0.004f;
                if (e > 0) blend(&px[y * w + x], c, a0 * clampf(e * 3, 0, 1));
            }
        }
    }

    /* the sun's path today, dotted */
    for (int k = 0; k < s->ntrack; k++) {
        if (s->track_alt[k] < 0) continue;
        float x, y;
        sky_xy(s, s->track_az[k], s->track_alt[k], w, h, &x, &y);
        disc(px, w, h, x, y, 1.6f, hex(0xFFF4D8), 0.45f);
    }

    /* the moon, if it's up */
    if (s->moon_alt > -2) {
        float x, y;
        sky_xy(s, s->moon_az, s->moon_alt, w, h, &x, &y);
        if (s->sun_alt < -4) glow(px, w, h, x, y, 70, hex(0xC8D0F0), 0.18f);
        sky_paint_moon(s, px, w, h, x, y, 30);
    }

    /* the sun, if it's up: the hills will cut it as it sets */
    if (s->sun_alt > -2) {
        float x, y;
        sky_xy(s, s->sun_az, s->sun_alt, w, h, &x, &y);
        float hi = clampf(s->sun_alt / 25, 0, 1);
        rgb_t c = mix(hex(0xFF6A30), hex(0xFFF0C8), hi);
        glow(px, w, h, x, y, 110, mix(hex(0xFF9050), hex(0xFFF4D8), hi), 0.55f);
        disc(px, w, h, x, y, 22, c, 1);
    }

    /* the hills: far, middle, near. One pass a column: each pixel the
       nearest ridge's above it, from a table of its colour down the slope
       (32 steps of the bokashi, 16 of grain); the ridge's edge blended */
    enum { STEPS = 32 };
    static uint16_t shade[3][STEPS][16];
    float ph[3][3], fr[3][3], base[3], amp[3];
    rgb_t body[3];
    for (int layer = 0; layer < 3; layer++) {
        uint32_t seed = hash(s->place * 3 + layer + 1);
        for (int k = 0; k < 3; k++) ph[layer][k] = (hash(seed + k) & 0xFFFF) / 65536.0f * 2 * PI;
        fr[layer][0] = 1.3f + layer * 0.5f;
        fr[layer][1] = 3.1f + layer;
        fr[layer][2] = 7.0f + layer * 2;
        base[layer] = hz - 18 + layer * 30;
        amp[layer] = 18 + layer * 14;
        body[layer] = mix(hor, ink, 0.45f + layer * 0.28f);
        rgb_t crest = mix(body[layer], hor, 0.12f);   /* bokashi: a little lighter at the top */
        for (int k = 0; k < STEPS; k++) grained(mix(crest, body[layer], (float)k / (STEPS - 1)), 0.5f, shade[layer][k]);
    }
    for (int x = 0; x < w; x++) {
        float u = (float)x / w * 2 * PI, ridge[3];
        for (int l = 0; l < 3; l++)
            ridge[l] = base[l] - amp[l] * (0.55f * sinf(fr[l][0] * u + ph[l][0]) + 0.3f * sinf(fr[l][1] * u + ph[l][1]) +
                                          0.15f * sinf(fr[l][2] * u + ph[l][2]));
        int y0 = (int)fminf(ridge[0], fminf(ridge[1], ridge[2]));
        if (y0 < 0) y0 = 0;
        for (int y = y0; y < h; y++) {
            int l = y >= ridge[2] ? 2 : y >= ridge[1] ? 1 : y >= ridge[0] ? 0 : -1;
            int edge = -1;   /* a ridge's own pixel, if no nearer hill covers it: blended over what's behind */
            for (int k = 2; k >= 0; k--) {
                if (y == (int)ridge[k]) {
                    edge = k;
                    break;
                }
                if (y >= ridge[k]) break;   /* inside a nearer hill */
            }
            int g = s_grain[(y & 63) * 64 + (x & 63)];
            if (edge >= 0) {   /* the hill behind (if any) first, then the ridge's crest over it */
                for (int b = edge - 1; b >= 0; b--)
                    if (y > ridge[b]) {
                        int k = (int)((y - ridge[b]) * (STEPS - 1) / 140);
                        px[y * w + x] = shade[b][k > STEPS - 1 ? STEPS - 1 : k][g];
                        break;
                    }
                blend(&px[y * w + x], unpack(shade[edge][0][8]), 1 - (ridge[edge] - (int)ridge[edge]));
                continue;
            }
            if (l < 0) continue;
            int k = (int)((y - ridge[l]) * (STEPS - 1) / 140);
            if (k > STEPS - 1) k = STEPS - 1;
            px[y * w + x] = shade[l][k][g];
        }
    }
}
