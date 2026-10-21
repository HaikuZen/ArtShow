/* gen_art.c - three generative pieces, each painting a little per step():
 *  - a flow field: particles drifting along a Perlin-noise current, each
 *    leaving a faint trail; ~2 minutes
 *  - a strange attractor (Clifford's or De Jong's): millions of points of a
 *    two-line map counted per pixel, shown through a log tone map; 30 M
 *    points (or 50 s of painting)
 *  - Hitomezashi: sashiko stitching from random bits, its regions coloured in
 *    two (it's always two-colourable), revealed row by row in ~20 s
 *  - stained glass: Voronoi cells round jittered seeds, lead between them,
 *    each pane lighter at its middle
 *  - Mondrian: a rectangle split and split again, black bars, a few panes in
 *    colour
 *  - Truchet: quarter-circle arcs, two per tile, turned at random: long
 *    winding paths
 * One piece at a time: their state is static here. sinf/cosf cost ~6 us on
 * this chip (measured): a table instead, 25x faster. */
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
    gen_dirty(c, 0, c->h - 1);
}

static void rect(gen_canvas_t *c, int x0, int y0, int x1, int y1, uint16_t col) {   /* x1, y1 excluded */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) c->px[y * c->w + x] = col;
    if (y1 > y0) gen_dirty(c, y0, y1 - 1);
}

/* a colour a little lighter (f > 1) or darker */
static uint32_t shade(uint32_t a, float f) {
    int r = ((a >> 16) & 255) * f, g = ((a >> 8) & 255) * f, b = (a & 255) * f;
    r = r > 255 ? 255 : r;
    g = g > 255 ? 255 : g;
    b = b > 255 ? 255 : b;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* sine and cosine from a table: 4096 steps a turn, interpolated */
#define TAU 6.2831853f
static float s_sin[4097];

static void trig_init(void) {
    if (s_sin[1024] > 0.5f) return;
    for (int i = 0; i <= 4096; i++) s_sin[i] = sinf(i * TAU / 4096);
}

static inline float fsin(float x) {
    float t = x * (4096 / TAU), f = floorf(t);
    int i = (int)f & 4095;
    return s_sin[i] + (s_sin[i + 1] - s_sin[i]) * (t - f);
}

static inline float fcos(float x) { return fsin(x + TAU / 4); }

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
    trig_init();
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
            F->x[i] += fcos(a) * F->speed;
            F->y[i] += fsin(a) * F->speed;
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
            gen_dirty(c, 0, c->h - 1);   /* everywhere a little */
        }
        if (now_ms() >= end) return false;
    }
    return true;
}

/* ---- a strange attractor ---- */

#define ATTR_ITER 30000000       /* points at most */
#define ATTR_MS   50000          /* or this long painting: ~0.65 M points a second here (the count's PSRAM writes, the trig a table) */
#define GRADIENT  256

static struct {
    uint16_t *count;             /* per pixel, saturating */
    uint16_t *tone;              /* count -> colour, 0..65535 */
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
        nx = fsin(A.a * *y) - fcos(A.b * *x);
        ny = fsin(A.cc * *x) - fcos(A.d * *y);
    } else {
        nx = fsin(A.a * *y) + A.cc * fcos(A.a * *x);
        ny = fsin(A.b * *x) + A.d * fcos(A.b * *y);
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

/* count -> colour: log(1 + n) over log(1 + max), a table for 0..max made
   each time (max is in the thousands; a logf per pixel was 614 k of them) */
static void attr_tone(gen_canvas_t *c) {
    if (!A.max || !A.tone) return;
    float k = (GRADIENT - 1) / logf(1.0f + A.max);
    for (uint32_t n = 0; n <= A.max; n++) A.tone[n] = A.grad[n ? (int)(logf(1.0f + n) * k) : 0];
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = A.tone[A.count[i]];
    gen_dirty(c, 0, c->h - 1);
}

static void attr_start(gen_canvas_t *c) {
    if (!A.count) A.count = heap_caps_calloc(c->w * c->h, sizeof *A.count, MALLOC_CAP_SPIRAM);
    if (!A.tone) A.tone = heap_caps_calloc(65536, sizeof *A.tone, MALLOC_CAP_SPIRAM);
    if (!A.count || !A.tone) return;
    trig_init();
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
    while (H.drawn < H.rows && H.drawn <= want) {
        hito_row(c, H.drawn);
        gen_dirty(c, H.drawn * H.s - 1, (H.drawn + 1) * H.s);
        H.drawn++;
    }
    return H.drawn >= H.rows;
}

/* ---- stained glass ---- */

#define GLASS_MAX 30 * 20         /* seeds: one per grid cell, jittered */

static struct {
    int cell, gx, gy;            /* the grid: cell size, columns, rows */
    float sx[GLASS_MAX], sy[GLASS_MAX];
    uint32_t col[GLASS_MAX];
    uint16_t lead;
    int drawn;
    int64_t t0;
} G;

static void glass_start(gen_canvas_t *c) {
    s_rng = c->seed | 1;
    static const int cells[] = { 48, 64, 80, 100 };
    G.cell = cells[rnd() % 4];
    G.gx = (c->w + G.cell - 1) / G.cell;
    G.gy = (c->h + G.cell - 1) / G.cell;
    for (int j = 0; j < G.gy; j++)
        for (int i = 0; i < G.gx; i++) {
            int k = j * G.gx + i;
            G.sx[k] = (i + frand(0.15f, 0.85f)) * G.cell;
            G.sy[k] = (j + frand(0.15f, 0.85f)) * G.cell;
            G.col[k] = shade(c->pal[1 + rnd() % (GEN_COLORS - 1)], frand(0.75f, 1.1f));
        }
    G.lead = rgb565(shade(c->pal[0], 0.6f));
    G.drawn = 0;
    G.t0 = now_ms();
    fill(c, c->pal[0]);
    snprintf(c->name, sizeof c->name, "Stained glass %lu", (unsigned long)(c->seed % 10000));
    snprintf(c->caption, sizeof c->caption, "Stained glass No. %lu   \xC2\xB7   %d panes", (unsigned long)(c->seed % 10000),
             G.gx * G.gy);
}

/* a row: each pixel's nearest and second nearest seed (only the 3 x 3 grid
   cells around it can hold them); lead where the two are about as near */
static void glass_row(gen_canvas_t *c, int y) {
    int cy = y / G.cell;
    for (int x = 0; x < c->w; x++) {
        int cx = x / G.cell, best = 0;
        float d1 = 1e9f, d2 = 1e9f;
        for (int j = cy - 1; j <= cy + 1; j++)
            for (int i = cx - 1; i <= cx + 1; i++) {
                if (i < 0 || j < 0 || i >= G.gx || j >= G.gy) continue;
                int k = j * G.gx + i;
                float dx = x - G.sx[k], dy = y - G.sy[k], d = dx * dx + dy * dy;
                if (d < d1) {
                    d2 = d1;
                    d1 = d;
                    best = k;
                } else if (d < d2) {
                    d2 = d;
                }
            }
        float a = sqrtf(d1), b = sqrtf(d2);
        if (b - a < 3.0f) {
            c->px[y * c->w + x] = G.lead;
        } else {   /* lighter at the pane's middle: light through glass */
            float glow = 1.25f - 0.5f * a / G.cell;
            c->px[y * c->w + x] = rgb565(shade(G.col[best], glow < 0.7f ? 0.7f : glow));
        }
    }
}

static bool glass_step(gen_canvas_t *c, int budget_ms) {
    int want = (int)((now_ms() - G.t0) * c->h / 15000);   /* ~15 s top to bottom */
    int y0 = G.drawn;
    while (G.drawn < c->h && G.drawn <= want) glass_row(c, G.drawn++);
    if (G.drawn > y0) gen_dirty(c, y0, G.drawn - 1);
    return G.drawn >= c->h;
}

/* ---- Mondrian ---- */

#define PANES 64

static struct {
    int n, drawn;
    short x0[PANES], y0[PANES], x1[PANES], y1[PANES];
    uint32_t col[PANES];
    int bar;
    int64_t t0;
} M;

static void split(gen_canvas_t *c, int x0, int y0, int x1, int y1, int depth) {
    int w = x1 - x0, h = y1 - y0;
    bool can_v = w > 220, can_h = h > 180;
    if (M.n < PANES - 1 && (can_v || can_h) && (depth < 3 || (int)(rnd() % 100) < 80 - depth * 10)) {
        bool vert = can_v && (!can_h || (w > h ? rnd() % 100 < 70 : rnd() % 100 < 30));
        if (vert) {
            int x = x0 + w * frand(0.3f, 0.7f);
            split(c, x0, y0, x, y1, depth + 1);
            split(c, x, y0, x1, y1, depth + 1);
        } else {
            int y = y0 + h * frand(0.3f, 0.7f);
            split(c, x0, y0, x1, y, depth + 1);
            split(c, x0, y, x1, y1, depth + 1);
        }
        return;
    }
    int k = M.n++;
    M.x0[k] = x0;
    M.y0[k] = y0;
    M.x1[k] = x1;
    M.y1[k] = y1;
    /* mostly the lightest, some in the palette's colours */
    M.col[k] = rnd() % 100 < 70 ? c->pal[GEN_COLORS - 1] : c->pal[1 + rnd() % (GEN_COLORS - 2)];
}

static void mondrian_start(gen_canvas_t *c) {
    s_rng = c->seed | 1;
    M.n = M.drawn = 0;
    M.bar = 6 + rnd() % 6;
    split(c, 0, 0, c->w, c->h, 0);
    for (int i = M.n - 1; i > 0; i--) {   /* painted in a random order */
        int j = rnd() % (i + 1);
#define SWAP(a) do { __typeof__(a[0]) t = a[i]; a[i] = a[j]; a[j] = t; } while (0)
        SWAP(M.x0); SWAP(M.y0); SWAP(M.x1); SWAP(M.y1); SWAP(M.col);
#undef SWAP
    }
    M.t0 = now_ms();
    fill(c, shade(c->pal[0], 0.5f));   /* the bars: what's left between the panes */
    snprintf(c->name, sizeof c->name, "Mondrian %lu", (unsigned long)(c->seed % 10000));
    snprintf(c->caption, sizeof c->caption, "Composition No. %lu   \xC2\xB7   %d panes, %d px bars", (unsigned long)(c->seed % 10000),
             M.n, M.bar);
}

static bool mondrian_step(gen_canvas_t *c, int budget_ms) {
    int want = (int)((now_ms() - M.t0) * M.n / 12000);   /* ~12 s for all */
    while (M.drawn < M.n && M.drawn <= want) {
        int k = M.drawn++, h = M.bar / 2;
        rect(c, M.x0[k] + h, M.y0[k] + h, M.x1[k] - h, M.y1[k] - h, rgb565(M.col[k]));
    }
    return M.drawn >= M.n;
}

/* ---- Truchet ---- */

static struct {
    int s, cols, rows, drawn;
    uint8_t turn[40 * 25];
    float line;                  /* the arcs' width */
    uint32_t bg, fg[2];
    int64_t t0;
} T;

static void truchet_start(gen_canvas_t *c) {
    s_rng = c->seed | 1;
    static const int sizes[] = { 40, 48, 64, 80 };
    T.s = sizes[rnd() % 4];
    T.cols = (c->w + T.s - 1) / T.s;
    T.rows = (c->h + T.s - 1) / T.s;
    for (int i = 0; i < T.cols * T.rows; i++) T.turn[i] = rnd() & 1;
    T.line = T.s * frand(0.12f, 0.22f);
    T.bg = c->pal[1];
    T.fg[0] = c->pal[GEN_COLORS - 1];
    T.fg[1] = c->pal[GEN_COLORS - 2];
    T.drawn = 0;
    T.t0 = now_ms();
    fill(c, c->pal[0]);
    snprintf(c->name, sizeof c->name, "Truchet %lu", (unsigned long)(c->seed % 10000));
    snprintf(c->caption, sizeof c->caption, "Truchet No. %lu   \xC2\xB7   %d px tiles", (unsigned long)(c->seed % 10000), T.s);
}

/* a tile: two quarter circles round opposite corners (which two: its turn),
   their edges softened over a pixel */
static void truchet_tile(gen_canvas_t *c, int ti, int tj) {
    int s = T.s, x0 = ti * s, y0 = tj * s;
    bool turn = T.turn[tj * T.cols + ti];
    float r = s / 2.0f, half = T.line / 2;
    uint16_t bg = rgb565(T.bg);
    uint16_t fg = rgb565(T.fg[(ti + tj) & 1]);   /* the paths alternate colour tile by tile: a woven look */
    for (int y = 0; y < s && y0 + y < c->h; y++)
        for (int x = 0; x < s && x0 + x < c->w; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            /* the corners: (0,0) and (s,s), or (s,0) and (0,s) */
            float ax = turn ? s : 0, bx = turn ? 0 : s;
            float d1 = fabsf(sqrtf((px - ax) * (px - ax) + py * py) - r);
            float d2 = fabsf(sqrtf((px - bx) * (px - bx) + (py - s) * (py - s)) - r);
            float d = fminf(d1, d2) - half;
            uint16_t *p = &c->px[(y0 + y) * c->w + x0 + x];
            *p = bg;
            if (d < 1) blend(p, fg, d <= 0 ? 32 : (uint32_t)((1 - d) * 32));
        }
}

static bool truchet_step(gen_canvas_t *c, int budget_ms) {
    int total = T.cols * T.rows, want = (int)((now_ms() - T.t0) * total / 15000);   /* ~15 s */
    while (T.drawn < total && T.drawn <= want) {
        /* row by row from the top */
        int k = T.drawn++, ti = k % T.cols, tj = k / T.cols;
        truchet_tile(c, ti, tj);
        gen_dirty(c, tj * T.s, (tj + 1) * T.s - 1);
    }
    return T.drawn >= total;
}

const gen_algo_t gen_algos[] = {
    { "Flow field", flow_start, flow_step },
    { "Attractor", attr_start, attr_step },
    { "Hitomezashi", hito_start, hito_step },
    { "Stained glass", glass_start, glass_step },
    { "Mondrian", mondrian_start, mondrian_step },
    { "Truchet", truchet_start, truchet_step },
};
const int gen_nalgos = sizeof gen_algos / sizeof gen_algos[0];
