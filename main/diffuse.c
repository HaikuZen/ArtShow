/* diffuse.c - the effects.
 *  - dithering: each pixel takes the nearest ink and passes its error on to
 *    the pixels not done yet (Floyd-Steinberg 7/3/5/1 sixteenths, rows run
 *    back and forth; Atkinson 1/8 to six of them, three quarters in all:
 *    the old Macintosh's crisp look). The inks: newsprint (black on paper,
 *    from the grey), six e-paper inks, the Game Boy's four greens, or the
 *    picture's own eight (k-means on a sample)
 *  - Perona-Malik: each pass moves every pixel towards its four neighbours
 *    by lambda * g(difference) * difference, g = exp(-(d/kappa)^2): a big
 *    difference (an edge) barely moves. g from the three channels' summed
 *    difference (one edge for all: no colour fringes), a table. 12-bit
 *    fixed point, the channels interleaved; each edge's flux worked out
 *    once, from the values before the pass
 *  - Gray-Scott: u + 2v -> 3v on a grid a third the size, u fed at f, v
 *    killed at k; k follows the picture (dark: low, the coral grows; light:
 *    high, it dies back). Shown at full size: v interpolated, the ink the
 *    picture's own colour, the paper a warm white */
#include "diffuse.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void unpack(uint16_t c, int *r, int *g, int *b) {
    *r = (c >> 11) * 255 / 31;
    *g = ((c >> 5) & 63) * 255 / 63;
    *b = (c & 31) * 255 / 31;
}

static inline uint16_t pack(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

static uint32_t rnd(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* ------------------------------------------------------------- dithering */

#define INKS 8

typedef struct {
    int n;
    int16_t ink[INKS][3];
    bool grey;              /* the picture as grey first (newsprint) */
    bool atkinson;
    int y;
    int16_t *err;           /* 3 rows x (w + 4) x 3 channels */
} dither_t;

static size_t dither_mem(int w, int h) {
    (void)h;
    return sizeof(dither_t) + 3 * (w + 4) * 3 * sizeof(int16_t); }

static void inks(dither_t *t, const uint32_t *rgb, int n) {
    t->n = n;
    for (int i = 0; i < n; i++) {
        t->ink[i][0] = rgb[i] >> 16 & 255;
        t->ink[i][1] = rgb[i] >> 8 & 255;
        t->ink[i][2] = rgb[i] & 255;
    }
}

static dither_t *dither_init(diffuse_t *d) {
    dither_t *t = d->mem;
    t->err = (int16_t *)(t + 1);
    t->y = 0;
    d->done = 0;
    d->total = d->h;
    snprintf(d->what, sizeof d->what, "rows");
    return t;
}

static void start_news(diffuse_t *d) {
    dither_t *t = dither_init(d);
    static const uint32_t I[] = { 0x1C1B1A, 0xF1ECDF };
    inks(t, I, 2);
    t->grey = t->atkinson = true;
}

static void start_epaper(diffuse_t *d) {
    dither_t *t = dither_init(d);
    static const uint32_t I[] = { 0x191E21, 0xE8E8E0, 0xB2252A, 0xE7C53C, 0x2F5DA8, 0x3A7A3C };
    inks(t, I, 6);
}

static void start_gameboy(diffuse_t *d) {
    dither_t *t = dither_init(d);
    static const uint32_t I[] = { 0x0F380F, 0x306230, 0x8BAC0F, 0x9BBC0F };
    inks(t, I, 4);
    t->grey = true;
}

/* the picture's eight: k-means on 4096 pixels, from its brightest, darkest
 * and spread start */
static void start_own(diffuse_t *d) {
    dither_t *t = dither_init(d);
    enum { N = 4096 };
    static int16_t px[N][3];
    uint32_t s = d->seed | 1;
    for (int i = 0; i < N; i++) {
        int r, g, b;
        unpack(d->src[rnd(&s) % (uint32_t)(d->w * d->h)], &r, &g, &b);
        px[i][0] = r;
        px[i][1] = g;
        px[i][2] = b;
    }
    int k = INKS;
    float c[INKS][3];
    for (int j = 0; j < k; j++)
        for (int ch = 0; ch < 3; ch++) c[j][ch] = px[(j * N) / k + (int)(rnd(&s) % (N / k))][ch];
    for (int it = 0; it < 8; it++) {
        float sum[INKS][3] = { { 0 } };
        int cnt[INKS] = { 0 };
        for (int i = 0; i < N; i++) {
            int best = 0;
            float bd = 1e30f;
            for (int j = 0; j < k; j++) {
                float dr = px[i][0] - c[j][0], dg = px[i][1] - c[j][1], db = px[i][2] - c[j][2];
                float dd = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                if (dd < bd) {
                    bd = dd;
                    best = j;
                }
            }
            for (int ch = 0; ch < 3; ch++) sum[best][ch] += px[i][ch];
            cnt[best]++;
        }
        for (int j = 0; j < k; j++)
            if (cnt[j])
                for (int ch = 0; ch < 3; ch++) c[j][ch] = sum[j][ch] / cnt[j];
            else   /* an empty one: a random pixel */
                for (int ch = 0; ch < 3; ch++) c[j][ch] = px[rnd(&s) % N][ch];
    }
    t->n = k;
    for (int j = 0; j < k; j++)
        for (int ch = 0; ch < 3; ch++) t->ink[j][ch] = (int16_t)(c[j][ch] + 0.5f);
}

static bool dither_step(diffuse_t *d, uint32_t (*ms)(void), uint32_t deadline) {
    dither_t *t = d->mem;
    const int W = d->w + 4;
    while (t->y < d->h) {
        int y = t->y;
        /* the error rows rotate: this one, the next, the one after */
        int16_t *e0 = t->err + (y % 3) * W * 3, *e1 = t->err + ((y + 1) % 3) * W * 3, *e2 = t->err + ((y + 2) % 3) * W * 3;
        bool back = !t->atkinson && (y & 1);   /* Floyd-Steinberg: back and forth */
        for (int i = 0; i < d->w; i++) {
            int x = back ? d->w - 1 - i : i, dir = back ? -1 : 1;
            int r, g, b;
            unpack(d->src[y * d->w + x], &r, &g, &b);
            if (t->grey) r = g = b = (r * 77 + g * 150 + b * 29) >> 8;
            int16_t *e = e0 + (x + 2) * 3;
            r += e[0];
            g += e[1];
            b += e[2];
            e[0] = e[1] = e[2] = 0;
            int best = 0, bd = 1 << 30;
            for (int j = 0; j < t->n; j++) {
                int dr = r - t->ink[j][0], dg = g - t->ink[j][1], db = b - t->ink[j][2];
                int dd = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                if (dd < bd) {
                    bd = dd;
                    best = j;
                }
            }
            const int16_t *k = t->ink[best];
            d->dst[y * d->w + x] = pack(k[0], k[1], k[2]);
            int er[3] = { r - k[0], g - k[1], b - k[2] };
            for (int ch = 0; ch < 3; ch++) {
                int v = er[ch];
                if (v > 255) v = 255;   /* runaway error from inks far apart: clamped */
                if (v < -255) v = -255;
                if (t->atkinson) {
                    int a = v / 8;
                    e0[(x + 3) * 3 + ch] += a;
                    e0[(x + 4) * 3 + ch] += a;
                    e1[(x + 1) * 3 + ch] += a;
                    e1[(x + 2) * 3 + ch] += a;
                    e1[(x + 3) * 3 + ch] += a;
                    e2[(x + 2) * 3 + ch] += a;
                } else {
                    e0[(x + 2 + dir) * 3 + ch] += v * 7 / 16;
                    e1[(x + 2 - dir) * 3 + ch] += v * 3 / 16;
                    e1[(x + 2) * 3 + ch] += v * 5 / 16;
                    e1[(x + 2 + dir) * 3 + ch] += v / 16;
                }
            }
        }
        memset(e0, 0, W * 3 * sizeof *e0);   /* it becomes the row after next */
        t->y++;
        d->done = t->y;
        if ((t->y & 15) == 0 && ms() >= deadline) return false;
    }
    return true;
}

/* ----------------------------------------------------------- Perona-Malik */

#define PM_PASSES 48
#define PM_KAPPA  30.0f        /* in 8-bit levels, the three channels summed */
#define PM_LAMBDA 0.24f

/* the pixels interleaved (r, g, b in 12 bits: 8-bit << 4): one stream per
 * row, not three. Each edge between two pixels is worked out once: east
 * as the next one's west (carried along the row), south as the next row's
 * north (kept in a row of fluxes) */
typedef struct {
    int pass, y;
    int16_t *px;               /* w x h x 3 */
    int32_t *north;            /* the flux into each pixel from above, w x 3 */
    uint16_t g[1024];          /* g(summed |difference| >> 4), lambda in 1/4096 */
} pm_t;

static size_t pm_mem(int w, int h) { return sizeof(pm_t) + (size_t)w * h * 6 + (size_t)w * 12 + 64; }

static inline int pm_g(const pm_t *t, int a, int b, int c) {
    int s = (abs(a) + abs(b) + abs(c)) >> 4;
    return t->g[s > 1023 ? 1023 : s];
}

static void pm_start(diffuse_t *d) {
    pm_t *t = d->mem;
    size_t n = (size_t)d->w * d->h;
    t->px = (int16_t *)(t + 1);
    t->north = (int32_t *)(((uintptr_t)(t->px + 3 * n) + 3) & ~(uintptr_t)3);
    for (size_t i = 0; i < n; i++) {
        int r, g, b;
        unpack(d->src[i], &r, &g, &b);
        t->px[3 * i] = r << 4;
        t->px[3 * i + 1] = g << 4;
        t->px[3 * i + 2] = b << 4;
        d->dst[i] = d->src[i];
    }
    for (int i = 0; i < 1024; i++) {
        float x = i / PM_KAPPA;
        t->g[i] = (uint16_t)(PM_LAMBDA * expf(-x * x) * 4096 + 0.5f);
    }
    t->pass = t->y = 0;
    d->done = 0;
    d->total = PM_PASSES;
    snprintf(d->what, sizeof d->what, "passes");
}

static bool pm_step(diffuse_t *d, uint32_t (*ms)(void), uint32_t deadline) {
    pm_t *t = d->mem;
    const int w = d->w, h = d->h;
    while (t->pass < PM_PASSES) {
        int y = t->y;
        int16_t *row = t->px + (size_t)y * w * 3, *down = y < h - 1 ? row + w * 3 : NULL;
        if (y == 0) memset(t->north, 0, (size_t)w * 12);   /* nothing above the top */
        int32_t *north = t->north;
        int west[3] = { 0, 0, 0 };                        /* nothing left of the left edge */
        for (int x = 0; x < w; x++) {
            int16_t *p = row + 3 * x;
            int v0 = p[0], v1 = p[1], v2 = p[2];
            int f0 = west[0] + north[3 * x], f1 = west[1] + north[3 * x + 1], f2 = west[2] + north[3 * x + 2];
            if (x < w - 1) {   /* east: into this pixel, and its opposite into the next one */
                int e0 = p[3] - v0, e1 = p[4] - v1, e2 = p[5] - v2, g = pm_g(t, e0, e1, e2);
                e0 *= g;
                e1 *= g;
                e2 *= g;
                f0 += e0;
                f1 += e1;
                f2 += e2;
                west[0] = -e0;
                west[1] = -e1;
                west[2] = -e2;
            }
            if (down) {   /* south: into this pixel, and its opposite the next row's north */
                int16_t *q = down + 3 * x;
                int s0 = q[0] - v0, s1 = q[1] - v1, s2 = q[2] - v2, g = pm_g(t, s0, s1, s2);
                s0 *= g;
                s1 *= g;
                s2 *= g;
                f0 += s0;
                f1 += s1;
                f2 += s2;
                north[3 * x] = -s0;
                north[3 * x + 1] = -s1;
                north[3 * x + 2] = -s2;
            }
            /* the fluxes were all taken from values before this pixel changed:
               the row's own east from p[3..5] not yet updated, south from the
               row below not yet updated, north and west from before (kept) */
            p[0] = (int16_t)(v0 + (f0 >> 12));
            p[1] = (int16_t)(v1 + (f1 >> 12));
            p[2] = (int16_t)(v2 + (f2 >> 12));
            d->dst[(size_t)y * w + x] = pack(p[0] >> 4, p[1] >> 4, p[2] >> 4);
        }
        if (++t->y == h) {
            t->y = 0;
            d->done = ++t->pass;
        }
        if ((t->y & 7) == 0 && ms() >= deadline) return t->pass >= PM_PASSES;
    }
    return true;
}

/* ------------------------------------------------------------ Gray-Scott */

#define RD_SCALE 3
#define RD_STEPS 1000     /* settled by then (a sweep on the computer); ~50 s on the board */
#define RD_FQ    1786     /* the feed, 0.0545, in Q15 */

typedef struct {
    int gw, gh, n;
    int16_t *u, *v, *u2, *v2, *k;   /* Q15: a step is PSRAM-bound (~1.4 MB moved in 32 bits), so 16 */
    uint8_t *a;              /* the coral's cover per cell, 0..32 */
    uint16_t *ink, *paper;   /* per pixel, worked out once */
    uint32_t shown, cost;    /* ms() of the last render, and what it took */
} rd_t;

static size_t rd_mem(int w, int h) {
    size_t n = (size_t)(w / RD_SCALE + 2) * (h / RD_SCALE + 2);
    return sizeof(rd_t) + 5 * n * sizeof(int16_t) + n + (size_t)w * h * 4 + 128;
}

/* c565 x a/32 + d565 x (32-a)/32, the three channels at once */
static inline uint16_t mix565(uint16_t c, uint16_t d, int a) {
    uint32_t x = (c | (uint32_t)c << 16) & 0x07E0F81F, y = (d | (uint32_t)d << 16) & 0x07E0F81F;
    uint32_t m = (y + (((x - y) * a) >> 5)) & 0x07E0F81F;
    return (uint16_t)(m | m >> 16);
}

static void rd_render(diffuse_t *d) {
    rd_t *t = d->mem;
    const int gw = t->gw;
    for (int i = 0; i < t->n; i++) {   /* the cover: v ~0 bare, ~0.3 in the coral; smoothstepped */
        int s = (t->v[i] - 3932) * 16;   /* 0.12 in Q15; to 16.16 and * 8 */
        s = s < 0 ? 0 : s > 65536 ? 65536 : s;
        s = (int)(((int64_t)s * s >> 16) * (3 * 65536 - 2 * s) >> 16);
        t->a[i] = (uint8_t)((s * 32 + 32768) >> 16);
    }
    /* bilinear, in 1/256ths: grid cell centres at (i + 0.5) * RD_SCALE */
    const int step = 256 / RD_SCALE;
    for (int y = 0; y < d->h; y++) {
        int gy = (y * 256 + 128) / RD_SCALE - 128;
        if (gy < 0) gy = 0;
        int y0 = gy >> 8, fy = gy & 255;
        if (y0 >= t->gh - 1) {
            y0 = t->gh - 2;
            fy = 255;
        }
        const uint8_t *r0 = t->a + y0 * gw, *r1 = r0 + gw;
        const uint16_t *ink = t->ink + (size_t)y * d->w, *paper = t->paper + (size_t)y * d->w;
        uint16_t *o = d->dst + (size_t)y * d->w;
        int gx = 128 / RD_SCALE - 128;
        for (int x = 0; x < d->w; x++, gx += step) {
            int g = gx < 0 ? 0 : gx, x0 = g >> 8, fx = g & 255;
            if (x0 >= gw - 1) {
                x0 = gw - 2;
                fx = 255;
            }
            int top = r0[x0] * 256 + (r0[x0 + 1] - r0[x0]) * fx, bot = r1[x0] * 256 + (r1[x0 + 1] - r1[x0]) * fx;
            int a = (top * 256 + (bot - top) * fy) >> 16;
            o[x] = mix565(ink[x], paper[x], a);
        }
    }
}

static void rd_start(diffuse_t *d) {
    rd_t *t = d->mem;
    t->gw = d->w / RD_SCALE;
    t->gh = d->h / RD_SCALE;
    t->n = t->gw * t->gh;
    int16_t *p = (int16_t *)(t + 1);
    t->u = p;
    t->v = p + t->n;
    t->u2 = p + 2 * t->n;
    t->v2 = p + 3 * t->n;
    t->k = p + 4 * t->n;
    t->a = (uint8_t *)(p + 5 * t->n);
    t->ink = (uint16_t *)(((uintptr_t)(t->a + t->n) + 3) & ~(uintptr_t)3);
    t->paper = t->ink + (size_t)d->w * d->h;
    for (size_t i = 0; i < (size_t)d->w * d->h; i++) {
        int r, g, b;
        unpack(d->src[i], &r, &g, &b);
        /* the ink: the picture, deeper; the paper: warm white with a hint of it */
        t->ink[i] = pack(r * 85 / 100, g * 85 / 100, b * 85 / 100);
        t->paper[i] = pack(236 + (r - 236) * 12 / 100, 230 + (g - 230) * 12 / 100, 218 + (b - 218) * 12 / 100);
    }
    uint32_t s = d->seed | 1;
    for (int y = 0; y < t->gh; y++)
        for (int x = 0; x < t->gw; x++) {
            /* the cell's brightness, a 3 x 3 average */
            int sum = 0;
            for (int j = 0; j < RD_SCALE; j++)
                for (int i = 0; i < RD_SCALE; i++) {
                    int r, g, b;
                    unpack(d->src[(y * RD_SCALE + j) * d->w + x * RD_SCALE + i], &r, &g, &b);
                    sum += r * 77 + g * 150 + b * 29;
                }
            float L = sum / (256.0f * 255 * RD_SCALE * RD_SCALE);
            int i = y * t->gw + x;
            /* k: 0.060 covers it all, 0.062 half, 0.068 a few %: dark grows dense, light sparse */
            t->k[i] = (int16_t)((0.0600f + 0.0085f * L) * 32768);
            t->u[i] = 32767;
            t->v[i] = 0;
        }
    for (int n = t->n / 250; n > 0; n--) {   /* seeds: 3 x 3 blobs of v (weaker ones just die) */
        int x = rnd(&s) % (t->gw - 3), y = rnd(&s) % (t->gh - 3);
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++) t->v[(y + j) * t->gw + x + i] = 32767;
    }
    rd_render(d);
    d->done = 0;
    d->total = RD_STEPS;
    snprintf(d->what, sizeof d->what, "steps");
}

/* one cell: xm, x, xp its column and its neighbours' (wrapped at the edges).
 * Q15 (1.0 = 32768). The Laplacian: 0.2 the sides, 0.05 the corners, less the cell: (4 sides +
 * corners - 20 cell) / 20; u diffuses at 1, v at 0.5 */
static inline void rd_cell(rd_t *t, const int16_t *um, const int16_t *u0, const int16_t *up, const int16_t *vm,
                           const int16_t *v0, const int16_t *vp, int xm, int x, int xp, int i) {
    int32_t u = u0[x], v = v0[x];
    int32_t lu = (4 * (um[x] + up[x] + u0[xm] + u0[xp]) + (um[xm] + um[xp] + up[xm] + up[xp]) - 20 * u) / 20;
    int32_t lv = (4 * (vm[x] + vp[x] + v0[xm] + v0[xp]) + (vm[xm] + vm[xp] + vp[xm] + vp[xp]) - 20 * v) / 20;
    int32_t uvv = ((u * v) >> 15) * v >> 15;
    int32_t nu = u + lu - uvv + ((RD_FQ * (32768 - u)) >> 15);
    int32_t nv = v + (lv >> 1) + uvv - (((RD_FQ + t->k[i]) * v) >> 15);
    t->u2[i] = (int16_t)(nu < 0 ? 0 : nu > 32767 ? 32767 : nu);
    t->v2[i] = (int16_t)(nv < 0 ? 0 : nv > 32767 ? 32767 : nv);
}

static bool rd_step(diffuse_t *d, uint32_t (*ms)(void), uint32_t deadline) {
    rd_t *t = d->mem;
    const int gw = t->gw, gh = t->gh;
    while (d->done < RD_STEPS) {
        for (int y = 0; y < gh; y++) {
            int ym = y ? y - 1 : gh - 1, yp = y < gh - 1 ? y + 1 : 0;   /* wrapped edges */
            const int16_t *um = t->u + ym * gw, *u0 = t->u + y * gw, *up = t->u + yp * gw;
            const int16_t *vm = t->v + ym * gw, *v0 = t->v + y * gw, *vp = t->v + yp * gw;
            int i = y * gw;
            rd_cell(t, um, u0, up, vm, v0, vp, gw - 1, 0, 1, i);
            for (int x = 1; x < gw - 1; x++) rd_cell(t, um, u0, up, vm, v0, vp, x - 1, x, x + 1, i + x);
            rd_cell(t, um, u0, up, vm, v0, vp, gw - 2, gw - 1, 0, i + gw - 1);
        }
        int16_t *s = t->u;
        t->u = t->u2;
        t->u2 = s;
        s = t->v;
        t->v = t->v2;
        t->v2 = s;
        d->done++;
        if ((d->done & 3) == 0 && ms() >= deadline) break;
    }
    /* the full-size picture: often, but never more than a fifth of the time */
    uint32_t now = ms();
    if (d->done >= RD_STEPS || now - t->shown >= 250 + 4 * t->cost) {
        rd_render(d);
        t->cost = ms() - now;
        t->shown = ms();
    }
    return d->done >= RD_STEPS;
}

const diffuse_fx_t diffuse_fx[] = {
    { "Newsprint", dither_mem, start_news, dither_step },
    { "E-paper", dither_mem, start_epaper, dither_step },
    { "Game Boy", dither_mem, start_gameboy, dither_step },
    { "Its 8 colours", dither_mem, start_own, dither_step },
    { "Melt", pm_mem, pm_start, pm_step },
    { "Coral", rd_mem, rd_start, rd_step },
};
const int diffuse_nfx = sizeof diffuse_fx / sizeof diffuse_fx[0];
