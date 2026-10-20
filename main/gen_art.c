/* gen_art.c - three generative pieces, each painting a little per step():
 *  - a flow field: particles drifting along a Perlin-noise current, each
 *    leaving a faint trail; ~2 minutes
 *  - a strange attractor (Clifford's or De Jong's): millions of points of a
 *    two-line map counted per pixel, shown through a log tone map; a minute
 *    of painting (or 40 M points)
 *  - Hitomezashi: sashiko stitching from random bits, its regions coloured in
 *    two (it's always two-colourable), revealed row by row in ~20 s
 * One piece at a time: their state is static here. */
#include "gen_art.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared ---- */

static uint32_t s_rng;
static uint32_t rnd(void) {   /* xorshift32 */
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float frand(float lo, float hi) { return lo + (hi - lo) * (rnd() & 0xFFFFFF) / (float)0x1000000; }

static uint16_t rgb565(uint32_t c) { return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F)); }

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    int r = ((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
    int g = ((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
    int bl = (a & 255) + (((int)(b & 255) - (int)(a & 255)) * t);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}

/* src over dst, alpha 0..32, RGB565 (green and red/blue apart in one word) */
static inline void blend(uint16_t *d, uint16_t s, uint32_t a) {
    uint32_t cd = (*d | (uint32_t)*d << 16) & 0x07E0F81F, cs = (s | (uint32_t)s << 16) & 0x07E0F81F;
    uint32_t c = ((cd * (32 - a) + cs * a) >> 5) & 0x07E0F81F;
    *d = (uint16_t)(c | c >> 16);
}

static void fill(gen_canvas_t *c, uint32_t color) {
    uint16_t v = rgb565(color);
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = v;
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ---- Perlin noise (Ken Perlin's improved noise, 2D) ---- */

static uint8_t s_perm[512];

static void noise_seed(void) {
    for (int i = 0; i < 256; i++) s_perm[i] = i;
    for (int i = 255; i > 0; i--) {
        int j = rnd() % (i + 1);
        uint8_t t = s_perm[i];
        s_perm[i] = s_perm[j];
        s_perm[j] = t;
    }
    memcpy(s_perm + 256, s_perm, 256);
}

static float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static float lerp(float t, float a, float b) { return a + t * (b - a); }
static float grad(int h, float x, float y) {
    switch (h & 7) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x;
    case 5: return -x;
    case 6: return y;
    default: return -y;
    }
}

static float noise2(float x, float y) {
    float fx = floorf(x), fy = floorf(y);
    int X = (int)fx & 255, Y = (int)fy & 255;
    x -= fx;
    y -= fy;
    float u = fade(x), v = fade(y);
    int a = s_perm[X] + Y, b = s_perm[X + 1] + Y;
    return lerp(v, lerp(u, grad(s_perm[a], x, y), grad(s_perm[b], x - 1, y)),
                lerp(u, grad(s_perm[a + 1], x, y - 1), grad(s_perm[b + 1], x - 1, y - 1)));
}

/* ---- the flow field ---- */

#define PARTICLES 2500
#define FLOW_ITER 2600           /* steps of each particle: ~2 minutes */

static struct {
    float x[PARTICLES], y[PARTICLES];
    uint16_t life[PARTICLES];
    uint8_t col[PARTICLES];
    float scale, turns, speed;
    int iter, next;              /* the round, and the next particle in it */
    uint16_t colors[GEN_COLORS];
    uint32_t alpha;
} *F;

static void flow_spawn(gen_canvas_t *c, int i) {
    F->x[i] = frand(0, c->w);
    F->y[i] = frand(0, c->h);
    F->life[i] = 60 + rnd() % 240;
    /* colour areas, not confetti: the colour from a slow noise of the place */
    float n = noise2(F->x[i] * 0.0018f + 31.7f, F->y[i] * 0.0018f + 12.3f) * 0.5f + 0.5f;
    int k = 1 + (int)(n * (GEN_COLORS - 1));
    F->col[i] = k < 1 ? 1 : k > GEN_COLORS - 1 ? GEN_COLORS - 1 : k;
}

static void flow_start(gen_canvas_t *c) {
    if (!F) F = heap_caps_calloc(1, sizeof *F, MALLOC_CAP_SPIRAM);
    if (!F) return;
    s_rng = c->seed | 1;
    noise_seed();
    F->scale = frand(0.0012f, 0.0035f);
    F->turns = frand(1.0f, 3.0f);
    F->speed = frand(0.9f, 1.6f);
    F->alpha = 3 + rnd() % 4;    /* of 32: a trail builds up where many pass */
    F->iter = F->next = 0;
    for (int k = 0; k < GEN_COLORS; k++) F->colors[k] = rgb565(c->pal[k]);
    fill(c, c->pal[0]);
    for (int i = 0; i < PARTICLES; i++) flow_spawn(c, i);
    snprintf(c->name, sizeof c->name, "Flow field %lu", (unsigned long)(c->seed % 10000));
    snprintf(c->caption, sizeof c->caption, "Flow field No. %lu   \xC2\xB7   scale %.4f, %.1f turns",
             (unsigned long)(c->seed % 10000), F->scale, F->turns);
}

static bool flow_step(gen_canvas_t *c, int budget_ms) {
    if (!F) return true;
    int64_t end = now_ms() + budget_ms;
    float k = F->turns * 2 * (float)M_PI;
    while (F->iter < FLOW_ITER) {
        for (int n = 0; n < 200 && F->next < PARTICLES; n++, F->next++) {
            int i = F->next;
            float x = F->x[i] * F->scale, y = F->y[i] * F->scale;
            float a = (noise2(x, y) + 0.5f * noise2(x * 2.3f + 5.2f, y * 2.3f + 1.3f)) * k;
            F->x[i] += cosf(a) * F->speed;
            F->y[i] += sinf(a) * F->speed;
            int px = (int)F->x[i], py = (int)F->y[i];
            if (px < 0 || py < 0 || px >= c->w || py >= c->h || !--F->life[i]) {
                flow_spawn(c, i);
                continue;
            }
            blend(&c->px[py * c->w + px], F->colors[F->col[i]], F->alpha);
        }
        if (F->next == PARTICLES) {
            F->next = 0;
            F->iter++;
        }
        if (now_ms() >= end) return false;
    }
    return true;
}

/* ---- a strange attractor ---- */

#define ATTR_ITER 40000000       /* points at most */
#define ATTR_MS   60000          /* or this long painting: the board does ~0.44 M points a second (26 M a minute) */
#define GRADIENT  256

static struct {
    uint16_t *count;             /* per pixel, saturating */
    uint16_t grad[GRADIENT];
    bool dejong;
    float a, b, cc, d, x, y;
    float sx, sy, ox, oy;        /* the attractor's box onto the screen */
    uint32_t done, max;
    int64_t shown;               /* the last tone map */
    int64_t busy_ms;             /* time spent painting */
} A;

static inline void attr_next(float *x, float *y) {
    float nx, ny;
    if (A.dejong) {
        nx = sinf(A.a * *y) - cosf(A.b * *x);
        ny = sinf(A.cc * *x) - cosf(A.d * *y);
    } else {
        nx = sinf(A.a * *y) + A.cc * cosf(A.a * *x);
        ny = sinf(A.b * *x) + A.d * cosf(A.b * *y);
    }
    *x = nx;
    *y = ny;
}

/* settings that make a filament, not a few dots: the points of a trial run
   cover enough of a coarse grid */
static bool attr_try(gen_canvas_t *c) {
    A.dejong = rnd() & 1;
    A.a = frand(-2.6f, 2.6f);
    A.b = frand(-2.6f, 2.6f);
    A.cc = A.dejong ? frand(-2.6f, 2.6f) : frand(-1.6f, 1.6f);
    A.d = A.dejong ? frand(-2.6f, 2.6f) : frand(-1.6f, 1.6f);
    float x = 0.1f, y = 0.1f, x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < 100; i++) attr_next(&x, &y);
    for (int i = 0; i < 20000; i++) {
        attr_next(&x, &y);
        x0 = fminf(x0, x);
        x1 = fmaxf(x1, x);
        y0 = fminf(y0, y);
        y1 = fmaxf(y1, y);
    }
    if (x1 - x0 < 0.3f || y1 - y0 < 0.3f) return false;
    static uint8_t grid[64 * 40];
    memset(grid, 0, sizeof grid);
    int hit = 0;
    for (int i = 0; i < 20000; i++) {
        attr_next(&x, &y);
        int gx = (int)((x - x0) / (x1 - x0) * 63.99f), gy = (int)((y - y0) / (y1 - y0) * 39.99f);
        if (gx >= 0 && gy >= 0 && gx < 64 && gy < 40 && !grid[gy * 64 + gx]++) hit++;
    }
    if (hit < 400) return false;   /* of 2560 cells */
    /* its box onto the screen, the same scale both ways, a margin around */
    float s = fminf((c->w * 0.9f) / (x1 - x0), (c->h * 0.9f) / (y1 - y0));
    A.sx = A.sy = s;
    A.ox = c->w / 2.0f - (x0 + x1) / 2 * s;
    A.oy = c->h / 2.0f - (y0 + y1) / 2 * s;
    A.x = x;
    A.y = y;
    return true;
}

static void attr_tone(gen_canvas_t *c) {
    if (!A.max) return;
    float k = (GRADIENT - 1) / logf(1.0f + A.max);
    for (int i = 0; i < c->w * c->h; i++) {
        uint16_t n = A.count[i];
        c->px[i] = A.grad[n ? (int)(logf(1.0f + n) * k) : 0];
    }
}

static void attr_start(gen_canvas_t *c) {
    if (!A.count) A.count = heap_caps_calloc(c->w * c->h, sizeof *A.count, MALLOC_CAP_SPIRAM);
    if (!A.count) return;
    s_rng = c->seed | 1;
    int tries = 0;
    while (!attr_try(c) && ++tries < 200) {}
    memset(A.count, 0, c->w * c->h * sizeof *A.count);
    A.done = A.max = 0;
    A.shown = 0;
    A.busy_ms = 0;
    /* the ground, then the palette's colours to the lightest: dense = light */
    for (int i = 0; i < GRADIENT; i++) {
        float t = i / (float)(GRADIENT - 1) * (GEN_COLORS - 1);
        int k = (int)t;
        A.grad[i] = rgb565(k >= GEN_COLORS - 1 ? c->pal[GEN_COLORS - 1] : mix(c->pal[k], c->pal[k + 1], t - k));
    }
    fill(c, c->pal[0]);
    const char *kind = A.dejong ? "De Jong" : "Clifford";
    snprintf(c->name, sizeof c->name, "%s attractor %lu", kind, (unsigned long)(c->seed % 10000));
    snprintf(c->caption, sizeof c->caption, "%s attractor No. %lu   \xC2\xB7   a %.2f  b %.2f  c %.2f  d %.2f", kind,
             (unsigned long)(c->seed % 10000), A.a, A.b, A.cc, A.d);
}

static bool attr_step(gen_canvas_t *c, int budget_ms) {
    if (!A.count) return true;
    int64_t t0 = now_ms(), end = t0 + budget_ms;
    while (A.done < ATTR_ITER) {
        for (int n = 0; n < 4096; n++) {
            attr_next(&A.x, &A.y);
            int px = (int)(A.x * A.sx + A.ox), py = (int)(A.y * A.sy + A.oy);
            if (px >= 0 && py >= 0 && px < c->w && py < c->h) {
                uint16_t *p = &A.count[py * c->w + px];
                if (*p < 65535) ++*p;
                if (*p > A.max) A.max = *p;
            }
        }
        A.done += 4096;
        if (now_ms() >= end) break;
    }
    A.busy_ms += now_ms() - t0;
    bool finished = A.done >= ATTR_ITER || A.busy_ms >= ATTR_MS;
    if (now_ms() - A.shown > 1000 || finished) {   /* the picture so far: about once a second */
        attr_tone(c);
        A.shown = now_ms();
    }
    return finished;
}

/* ---- Hitomezashi ---- */

static struct {
    int s, cols, rows, drawn;    /* cell size, cells, rows revealed */
    uint8_t colbit[160], rowbit[120];
    uint8_t *parity;             /* each cell's colour, 0 or 1 */
    int64_t t0;
    uint16_t fill[2], stitch;
} H;

/* is the stitch on vertical line i (x = i*s) present beside row j? on
   horizontal line j beside column i? */
static bool vseg(int i, int j) { return (j + H.colbit[i]) % 2 == 0; }
static bool hseg(int j, int i) { return (i + H.rowbit[j]) % 2 == 0; }

static void hito_start(gen_canvas_t *c) {
    s_rng = c->seed | 1;
    static const int sizes[] = { 16, 20, 24, 30, 40 };
    H.s = sizes[rnd() % 5];
    H.cols = (c->w + H.s - 1) / H.s;
    H.rows = (c->h + H.s - 1) / H.s;
    for (int i = 0; i <= H.cols; i++) H.colbit[i] = rnd() & 1;
    for (int j = 0; j <= H.rows; j++) H.rowbit[j] = rnd() & 1;
    free(H.parity);
    H.parity = heap_caps_malloc(H.cols * H.rows, MALLOC_CAP_SPIRAM);
    if (!H.parity) return;
    /* the two colours of the regions: crossing a stitch flips it */
    for (int j = 0; j < H.rows; j++)
        for (int i = 0; i < H.cols; i++) {
            uint8_t p = i ? H.parity[j * H.cols + i - 1] ^ vseg(i, j) : j ? H.parity[(j - 1) * H.cols] ^ hseg(j, 0) : 0;
            H.parity[j * H.cols + i] = p;
        }
    H.fill[0] = rgb565(c->pal[1]);
    H.fill[1] = rgb565(c->pal[2]);
    H.stitch = rgb565(c->pal[GEN_COLORS - 1]);
    H.drawn = 0;
    H.t0 = now_ms();
    fill(c, c->pal[0]);
    snprintf(c->name, sizeof c->name, "Hitomezashi %lu", (unsigned long)(c->seed % 10000));
    snprintf(c->caption, sizeof c->caption, "Hitomezashi No. %lu   \xC2\xB7   %d px stitches", (unsigned long)(c->seed % 10000), H.s);
}

static void hito_row(gen_canvas_t *c, int j) {
    int s = H.s, y0 = j * s;
    for (int i = 0; i < H.cols; i++) {
        uint16_t col = H.fill[H.parity[j * H.cols + i]];
        for (int y = y0; y < y0 + s && y < c->h; y++)
            for (int x = i * s; x < (i + 1) * s && x < c->w; x++) c->px[y * c->w + x] = col;
    }
    /* the stitches, 2 px: the vertical ones beside this row, the horizontal one above it */
    for (int i = 0; i <= H.cols; i++)
        if (vseg(i, j))
            for (int y = y0; y < y0 + s && y < c->h; y++)
                for (int x = i * s - 1; x <= i * s; x++)
                    if (x >= 0 && x < c->w) c->px[y * c->w + x] = H.stitch;
    for (int i = 0; i < H.cols; i++)
        if (hseg(j, i))
            for (int y = y0 - 1; y <= y0; y++)
                for (int x = i * s; x < (i + 1) * s && x < c->w; x++)
                    if (y >= 0 && y < c->h) c->px[y * c->w + x] = H.stitch;
}

static bool hito_step(gen_canvas_t *c, int budget_ms) {
    if (!H.parity) return true;
    int want = (int)((now_ms() - H.t0) * H.rows / 20000);   /* ~20 s whatever the stitch: a calm reveal */
    while (H.drawn < H.rows && H.drawn <= want) hito_row(c, H.drawn++);
    return H.drawn >= H.rows;
}

const gen_algo_t gen_algos[] = {
    { "Flow field", flow_start, flow_step },
    { "Attractor", attr_start, attr_step },
    { "Hitomezashi", hito_start, hito_step },
};
const int gen_nalgos = sizeof gen_algos / sizeof gen_algos[0];
