/* glow.c - a picture's own colours as a soft ground: averaged down to a
 * coarse grid (GW x GH cells), stretched back up smoothly (bilinear with
 * a smoothstep between cells: no creases; fixed point), darkened, a
 * vignette, a 4 x 4 ordered dither against RGB565's banding. Plain C: it
 * renders on a computer too. */
#include "glow.h"
#include <string.h>

#define GW 16
#define GH 9

/* t (0..256) eased: 3t^2 - 2t^3 */
static inline int ease(int t) { return (int)((int64_t)t * t * (768 - 2 * t) >> 16); }

static const int8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

void glow_make(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh, int level) {
    static int32_t grid[GH][GW][3];   /* 0..255 << 8 */
    memset(grid, 0, sizeof grid);
    for (int gy = 0; gy < GH; gy++) {   /* each cell's average, a pixel in 16 */
        int y0 = gy * sh / GH, y1 = (gy + 1) * sh / GH;
        for (int gx = 0; gx < GW; gx++) {
            int x0 = gx * sw / GW, x1 = (gx + 1) * sw / GW;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y += 4)
                for (int x = x0; x < x1; x += 4, n++) {
                    uint16_t p = src[y * sw + x];
                    r += p >> 11;
                    g += (p >> 5) & 63;
                    b += p & 31;
                }
            if (!n) n = 1;
            grid[gy][gx][0] = (int32_t)(r * 255 * 256 / (31 * n));
            grid[gy][gx][1] = (int32_t)(g * 255 * 256 / (63 * n));
            grid[gy][gx][2] = (int32_t)(b * 255 * 256 / (31 * n));
        }
    }
    /* per column, once: its grid cell, the eased step into the next, the
       vignette across (1 - 0.45 d^2, Q8) */
    static int16_t cx[2048], ctx[2048], vx[2048];
    int w = dw < 2048 ? dw : 2048;
    for (int x = 0; x < w; x++) {
        int fx = ((2 * x + 1) * GW * 128 / dw) - 128;
        if (fx < 0) fx = 0;
        int gx = fx >> 8, tx = fx & 255;
        if (gx >= GW - 1) { gx = GW - 2; tx = 256; }
        cx[x] = (int16_t)gx;
        ctx[x] = (int16_t)ease(tx);
        int d = (2 * x - w) * 256 / w;   /* -256..256 */
        vx[x] = (int16_t)(256 - (115 * d * d >> 16));
    }
    static int32_t row[GW][3];
    for (int y = 0; y < dh; y++) {
        /* this row between two grid rows (the cells' middles) */
        int fy = ((2 * y + 1) * GH * 128 / dh) - 128;   /* Q8, from the first cell's middle */
        if (fy < 0) fy = 0;
        int gy = fy >> 8, ty = fy & 255;
        if (gy >= GH - 1) { gy = GH - 2; ty = 256; }
        ty = ease(ty);
        for (int gx = 0; gx < GW; gx++)
            for (int c = 0; c < 3; c++)
                row[gx][c] = grid[gy][gx][c] + (int32_t)(((int64_t)(grid[gy + 1][gx][c] - grid[gy][gx][c]) * ty) >> 8);
        int d = (2 * y - dh) * 256 / dh;
        int32_t vy = 256 - (int32_t)(140 * d * d >> 16);
        int32_t k = level * vy >> 8;   /* the level (Q8) and the vignette down */
        uint16_t *out = dst + y * dw;
        const int8_t *bay = BAYER[y & 3];
        for (int x = 0; x < w; x++) {
            int gx = cx[x], tx = ctx[x];
            int32_t kk = k * vx[x] >> 8;
            int dither = bay[x & 3] * 255;
            int32_t v[3];
            for (int c = 0; c < 3; c++) {
                int32_t a = row[gx][c] + (((row[gx + 1][c] - row[gx][c]) * tx) >> 8);   /* 0..255 << 8 */
                v[c] = (a >> 4) * kk >> 12;   /* 0..255 */
            }
            /* (v * levels * 16 + dither * 255) / (255 * 16): times 2^26 / 4080 */
            int r = (int)((uint32_t)(v[0] * 31 * 16 + dither) * 16449u >> 26);
            int g = (int)((uint32_t)(v[1] * 63 * 16 + dither) * 16449u >> 26);
            int b = (int)((uint32_t)(v[2] * 31 * 16 + dither) * 16449u >> 26);
            out[x] = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
        }
    }
}
