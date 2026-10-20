/* transition.c - from one picture to the next, a different way each time.
 *
 * Every effect is a map: for each 4x4 block of the screen, when it starts to
 * turn (0..255). A timer in the LVGL task moves a front through those times;
 * a block it passes blends from the old picture to the new over `feather`
 * map units. Only blocks whose blend changed are redrawn. So an iris is just
 * the distance from a point, a brushstroke the distance along a wavy line,
 * mosaic a random time per tile, and the dissolve the same time everywhere. */
#include "transition.h"
#include "board_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "settings.h"
#include <math.h>
#include <string.h>

static const char *TAG = "transition";

#define BS        4                    /* the map's block: BS x BS pixels */
#define BW        (LCD_H_RES / BS)
#define BH        (LCD_V_RES / BS)
#define FRAME_MS  20
#define BAND      5                    /* block rows per redrawn area: 30 areas, LVGL keeps up to 32 */
#define SCREEN_PX (LCD_H_RES * LCD_V_RES)

typedef struct {
    const char *name;
    int feather;                       /* how long a block takes to turn, in map units (of 255) */
    void (*map)(float *f);             /* each block's start, any scale: normalized after */
} effect_t;

static lv_obj_t *s_view;
static uint16_t *s_out, *s_old;        /* on show; the old picture, kept for the blend */
static const uint16_t *s_new;
static float *s_f;                     /* the effect's raw map */
static uint8_t *s_t, *s_a;             /* per block: its start (0..255), its blend now (0..32) */
static int s_feather;
static int s_k;                        /* 32/feather, << 16: a multiply, not a divide, per block */
static uint32_t s_t0;
static lv_timer_t *s_timer;
static bool s_running;
static const char *s_name;
static int s_frames;

/* ---- the effects ---- */

static float frand(void) { return (float)esp_random() / 4294967295.0f; }

/* a block's centre on screen */
#define BX(i) ((float)((i) % BW * BS + BS / 2))
#define BY(i) ((float)((i) / BW * BS + BS / 2))

/* the same time everywhere: the classic crossfade */
static void fx_dissolve(float *f) {
    for (int i = 0; i < BW * BH; i++) f[i] = 0;
}

/* a circle opening from a point somewhere in the middle */
static void fx_iris(float *f) {
    float px = LCD_H_RES * (0.25f + 0.5f * frand()), py = LCD_V_RES * (0.25f + 0.5f * frand());
    for (int i = 0; i < BW * BH; i++) f[i] = hypotf(BX(i) - px, BY(i) - py);
}

/* a drop of ink spreading: a circle with a lobed, wavering edge */
static void fx_ink(float *f) {
    float px = LCD_H_RES * (0.2f + 0.6f * frand()), py = LCD_V_RES * (0.2f + 0.6f * frand());
    float lobes = 3 + (int)(frand() * 5), ph = frand() * 6.283f;
    for (int i = 0; i < BW * BH; i++) {
        float dx = BX(i) - px, dy = BY(i) - py, d = hypotf(dx, dy), th = atan2f(dy, dx);
        f[i] = d * (1 + 0.18f * sinf(lobes * th + ph) + 0.06f * sinf(2.7f * lobes * th - ph));
    }
}

/* a broad brushstroke sweeping across at an angle, its edge uneven like bristles */
static void fx_brush(float *f) {
    float ang = frand() * 6.283f, ux = cosf(ang), uy = sinf(ang), ph = frand() * 6.283f;
    for (int i = 0; i < BW * BH; i++) {
        float along = BX(i) * ux + BY(i) * uy, across = -BX(i) * uy + BY(i) * ux;
        f[i] = along + 45 * sinf(across / 61 + ph) + 14 * sinf(across / 13 - ph) + 8 * frand();
    }
}

/* square tiles turning in a random order, a little earlier near one corner */
static void fx_mosaic(float *f) {
    enum { TX = 8, TY = 8, NTX = BW / TX + 1, NTY = BH / TY + 1 };   /* 32 x 32 px tiles */
    static float tile[NTX * NTY];
    for (int k = 0; k < NTX * NTY; k++) tile[k] = frand();
    bool flip = esp_random() & 1;
    for (int i = 0; i < BW * BH; i++) {
        int bx = i % BW, by = i / BW, k = by / TY * NTX + bx / TX;
        float sweep = (float)(flip ? BW - bx : bx) / BW;
        f[i] = 0.75f * tile[k] + 0.25f * sweep;
    }
}

/* a wipe that falls apart into grains, like sand blown off the old picture */
static void fx_sand(float *f) {
    float ang = frand() * 6.283f, ux = cosf(ang), uy = sinf(ang);
    for (int i = 0; i < BW * BH; i++) f[i] = (BX(i) * ux + BY(i) * uy) / LCD_H_RES + 0.45f * frand();
}

/* venetian blinds: strips turning one after the other, left to right or top down */
static void fx_blinds(float *f) {
    bool vertical = esp_random() & 1;
    for (int i = 0; i < BW * BH; i++) {
        float p = vertical ? BX(i) : BY(i), len = vertical ? LCD_H_RES : LCD_V_RES;
        f[i] = fmodf(p, 64) / 64 * 0.7f + p / len * 0.3f;
    }
}

/* a clock hand sweeping round from a random hour */
static void fx_clock(float *f) {
    float th0 = frand() * 6.283f, cx = LCD_H_RES / 2.0f, cy = LCD_V_RES / 2.0f;
    for (int i = 0; i < BW * BH; i++) {
        float dx = BX(i) - cx, dy = BY(i) - cy;
        float a = fmodf(atan2f(dy, dx) - th0 + 4 * 3.14159265f, 2 * 3.14159265f) / (2 * 3.14159265f);
        f[i] = a * 0.9f + hypotf(dx, dy) / LCD_H_RES * 0.1f;
    }
}

static const effect_t s_fx[] = {
    { "dissolve", 255, fx_dissolve },
    { "iris",      40, fx_iris },
    { "ink",       40, fx_ink },
    { "brush",     55, fx_brush },
    { "mosaic",    35, fx_mosaic },
    { "sand",      25, fx_sand },
    { "blinds",    45, fx_blinds },
    { "clock",     25, fx_clock },
};
#define N_FX (int)(sizeof s_fx / sizeof s_fx[0])

/* ---- the blend ---- */

/* RGB565 o to n by a/32: the channels spread over 32 bits, one multiply each */
static inline uint16_t mix(uint16_t o, uint16_t n, int a) {
    uint32_t co = (o | (uint32_t)o << 16) & 0x07E0F81F, cn = (n | (uint32_t)n << 16) & 0x07E0F81F;
    uint32_t c = ((co * (32 - a) + cn * a) >> 5) & 0x07E0F81F;
    return (uint16_t)(c | c >> 16);
}

static uint32_t s_len;   /* ms: this change's length */

static void frame_cb(lv_timer_t *t) {
    uint32_t el = lv_tick_elaps(s_t0), len = s_len;
    bool done = el >= len;
    int front = done ? 255 + s_feather : (int)(el * (255 + s_feather) / len);
    s_frames++;
    for (int band = 0; band < BH / BAND; band++) {
        int x0 = BW, x1 = -1;   /* the band's blocks redrawn */
        for (int by = band * BAND; by < (band + 1) * BAND; by++) {
            /* the block row's new blends first (-1: unchanged), then the
               pixels row by row: each buffer streamed, not 4 rows a block */
            static int8_t row_a[BW];
            int rx0 = BW, rx1 = -1;
            for (int bx = 0; bx < BW; bx++) {
                int i = by * BW + bx;
                row_a[bx] = -1;
                if (s_a[i] == 32 || s_t[i] >= front) continue;   /* done, or not reached yet */
                int a = ((front - s_t[i]) * s_k) >> 16;
                a = a >= 32 ? 32 : a;
                if (a == s_a[i]) continue;
                s_a[i] = row_a[bx] = a;
                if (bx < rx0) rx0 = bx;
                rx1 = bx;
            }
            if (rx1 < 0) continue;
            for (int r = 0; r < BS; r++) {
                size_t row = (size_t)(by * BS + r) * LCD_H_RES;
                for (int bx = rx0; bx <= rx1; bx++) {
                    int a = row_a[bx];
                    if (a < 0) continue;
                    size_t p = row + bx * BS;
                    if (a == 32) memcpy(&s_out[p], &s_new[p], BS * sizeof(uint16_t));
                    else for (int c = 0; c < BS; c++) s_out[p + c] = mix(s_old[p + c], s_new[p + c], a);
                }
            }
            if (rx0 < x0) x0 = rx0;
            if (rx1 > x1) x1 = rx1;
        }
        /* one area per band, not one box round everything: a thin front
           (iris, brush, clock) then redraws little more than itself */
        if (x1 >= 0) {
            lv_area_t ar = { x0 * BS, band * BAND * BS, (x1 + 1) * BS - 1, (band + 1) * BAND * BS - 1 };
            lv_obj_invalidate_area(s_view, &ar);
        }
    }
    if (done) {   /* the end exact, whatever the rounding on the way, and redrawn whole */
        memcpy(s_out, s_new, SCREEN_PX * sizeof(uint16_t));
        lv_obj_invalidate(s_view);
        ESP_LOGI(TAG, "%s: %d frames in %d ms", s_name, s_frames, (int)lv_tick_elaps(s_t0));
        s_running = false;
        lv_timer_pause(t);
    }
}

/* ---- the API ---- */

bool transition_init(lv_obj_t *view, uint16_t *out) {
    s_view = view;
    s_out = out;
    s_old = heap_caps_malloc(SCREEN_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_f = heap_caps_malloc(BW * BH * sizeof(float), MALLOC_CAP_SPIRAM);
    /* every frame reads both whole: internal RAM if there's room */
    s_t = heap_caps_malloc_prefer(BW * BH, 2, MALLOC_CAP_INTERNAL, MALLOC_CAP_SPIRAM);
    s_a = heap_caps_malloc_prefer(BW * BH, 2, MALLOC_CAP_INTERNAL, MALLOC_CAP_SPIRAM);
    if (!s_old || !s_f || !s_t || !s_a) {
        ESP_LOGE(TAG, "no memory");
        return false;
    }
    s_timer = lv_timer_create(frame_cb, FRAME_MS, NULL);
    lv_timer_pause(s_timer);
    return true;
}

void transition_finish(void) {
    if (!s_running) return;
    memcpy(s_out, s_new, SCREEN_PX * sizeof(uint16_t));
    lv_obj_invalidate(s_view);
    lv_timer_pause(s_timer);
    s_running = false;
}

void transition_start(const uint16_t *next) {
    static int last = -1;
    transition_finish();
    memcpy(s_old, s_out, SCREEN_PX * sizeof(uint16_t));
    s_new = next;

    int k = 0;   /* never the same twice in a row */
    s_len = g_settings.fade_ms;   /* the Settings page's, as it is now: the same for the whole change */
    if (g_settings.varied)
        do k = esp_random() % N_FX; while (k == last);
    last = k;
    const effect_t *fx = &s_fx[k];

    /* the effect's map, stretched over 0..255 */
    fx->map(s_f);
    float lo = s_f[0], hi = s_f[0];
    for (int i = 1; i < BW * BH; i++) {
        if (s_f[i] < lo) lo = s_f[i];
        if (s_f[i] > hi) hi = s_f[i];
    }
    float k255 = hi > lo ? 255 / (hi - lo) : 0;
    for (int i = 0; i < BW * BH; i++) s_t[i] = (uint8_t)((s_f[i] - lo) * k255);
    memset(s_a, 0, BW * BH);
    s_feather = fx->feather;
    s_k = ((32 << 16) + s_feather - 1) / s_feather;   /* rounded up: a block a feather past its start is at 32 */
    s_name = fx->name;
    s_frames = 0;

    s_running = true;
    s_t0 = lv_tick_get();
    lv_timer_reset(s_timer);
    lv_timer_resume(s_timer);
}
