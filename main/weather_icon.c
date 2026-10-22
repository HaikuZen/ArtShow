/* weather_icon.c - weather icons drawn in the object's draw event, on a 100 x
 * 100 grid scaled to its size: a few circles, rounded bars and lines. */
#include "weather_icon.h"
#include <math.h>

/* the inks: bright (for a dark ground) and a print's (for paper) */
typedef struct { uint32_t sun, moon, cloud, cloud_dark, rain, snow, fog, bolt; } inks_t;
static const inks_t BRIGHT = { 0xFFC93C, 0xE8E4C9, 0xD8DEE6, 0x7D8A9C, 0x4FA3FF, 0xFFFFFF, 0x9AA8B8, 0xFFC93C };
static const inks_t PRINT = { 0xD9542B, 0xC99A2E, 0x8E9BB0, 0x4A5670, 0x2F5D8A, 0x8FA6C2, 0x9A9A96, 0xD9542B };
#define FLAG_NIGHT 0x100
#define FLAG_PRINT 0x200

typedef struct {
    const inks_t *ink;
    lv_layer_t *layer;
    int32_t x, y;   /* the icon's top left on the screen */
    float u;        /* pixels per grid unit */
    lv_color_t bg;  /* under the icon: the moon's bite */
} pen_t;

static int32_t px(const pen_t *p, float v) { return lroundf(v * p->u); }

static void bar(const pen_t *p, float x1, float y1, float x2, float y2, float r, lv_color_t c) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    d.radius = px(p, r);
    lv_area_t a = { p->x + px(p, x1), p->y + px(p, y1), p->x + px(p, x2), p->y + px(p, y2) };
    lv_draw_rect(p->layer, &d, &a);
}

static void disk(const pen_t *p, float cx, float cy, float r, lv_color_t c) {
    bar(p, cx - r, cy - r, cx + r, cy + r, r, c);
}

static void line(const pen_t *p, float x1, float y1, float x2, float y2, float w, uint32_t c) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = p->x + px(p, x1);
    d.p1.y = p->y + px(p, y1);
    d.p2.x = p->x + px(p, x2);
    d.p2.y = p->y + px(p, y2);
    d.width = px(p, w) > 1 ? px(p, w) : 1;
    d.color = lv_color_hex(c);
    d.opa = LV_OPA_COVER;
    d.round_start = d.round_end = 1;
    lv_draw_line(p->layer, &d);
}

static void sun(const pen_t *p, float cx, float cy, float r) {
    for (int k = 0; k < 8; k++) {
        float a = k * (float)M_PI / 4, c = cosf(a), s = sinf(a);
        line(p, cx + c * r * 1.4f, cy + s * r * 1.4f, cx + c * r * 1.85f, cy + s * r * 1.85f, r * 0.2f, p->ink->sun);
    }
    disk(p, cx, cy, r, lv_color_hex(p->ink->sun));
}

static void moon(const pen_t *p, float cx, float cy, float r) {
    disk(p, cx, cy, r, lv_color_hex(p->ink->moon));
    disk(p, cx + r * 0.5f, cy - r * 0.35f, r * 0.8f, p->bg);   /* the bite: a crescent */
}

/* a cloud w wide, its flat bottom at y1 */
static void cloud(const pen_t *p, float x0, float y1, float w, uint32_t c) {
    lv_color_t col = lv_color_hex(c);
    disk(p, x0 + 0.30f * w, y1 - 0.30f * w, 0.21f * w, col);
    disk(p, x0 + 0.58f * w, y1 - 0.38f * w, 0.29f * w, col);
    bar(p, x0, y1 - 0.34f * w, x0 + w, y1, 0.17f * w, col);
}

static void draw_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    uint32_t v = (uint32_t)(uintptr_t)lv_obj_get_user_data(o);
    wx_kind_t kind = v & 0xff;
    bool night = v & FLAG_NIGHT;
    const inks_t *ink = v & FLAG_PRINT ? &PRINT : &BRIGHT;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    pen_t p = { ink, lv_event_get_layer(e), a.x1, a.y1, lv_area_get_width(&a) / 100.0f, lv_color_black() };
    for (lv_obj_t *q = o; q; q = lv_obj_get_parent(q))   /* the first opaque background up the tree */
        if (lv_obj_get_style_bg_opa(q, LV_PART_MAIN) == LV_OPA_COVER) {
            p.bg = lv_obj_get_style_bg_color(q, LV_PART_MAIN);
            break;
        }

    switch (kind) {
    case WX_CLEAR:
        if (night) moon(&p, 50, 50, 30);
        else sun(&p, 50, 50, 20);
        break;
    case WX_PARTLY:
        if (night) moon(&p, 38, 36, 20);
        else sun(&p, 38, 36, 14);
        cloud(&p, 22, 84, 70, ink->cloud);
        break;
    case WX_CLOUDY:
        cloud(&p, 34, 62, 58, ink->cloud_dark);
        cloud(&p, 8, 84, 76, ink->cloud);
        break;
    case WX_FOG:
        cloud(&p, 18, 56, 64, ink->cloud);
        for (int k = 0; k < 3; k++) line(&p, 16 + k * 6, 68 + k * 10, 84 - k * 6, 68 + k * 10, 5, ink->fog);
        break;
    case WX_DRIZZLE:
        cloud(&p, 12, 66, 76, ink->cloud);
        for (int x = 32; x <= 68; x += 18) {
            line(&p, x, 76, x - 2, 80, 4, ink->rain);
            line(&p, x - 5, 88, x - 7, 92, 4, ink->rain);
        }
        break;
    case WX_RAIN:
        cloud(&p, 12, 66, 76, ink->cloud);
        for (int x = 32; x <= 68; x += 18) line(&p, x, 75, x - 7, 93, 5, ink->rain);
        break;
    case WX_SNOW:
        cloud(&p, 12, 66, 76, ink->cloud);
        for (int x = 30; x <= 70; x += 20) disk(&p, x, 78, 4, lv_color_hex(ink->snow));
        for (int x = 40; x <= 60; x += 20) disk(&p, x, 91, 4, lv_color_hex(ink->snow));
        break;
    case WX_STORM:
        cloud(&p, 12, 64, 76, ink->cloud_dark);
        line(&p, 56, 62, 44, 80, 6, ink->bolt);   /* the bolt */
        line(&p, 44, 80, 58, 80, 6, ink->bolt);
        line(&p, 58, 80, 46, 97, 6, ink->bolt);
        break;
    default:
        cloud(&p, 12, 74, 76, ink->cloud_dark);
        break;
    }
}

lv_obj_t *weather_icon_create(lv_obj_t *parent, int32_t size) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_clickable(o, false);
    lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

void weather_icon_set(lv_obj_t *icon, wx_kind_t kind, bool night) {
    uint32_t v = (uint32_t)(uintptr_t)lv_obj_get_user_data(icon) & FLAG_PRINT;
    lv_obj_set_user_data(icon, (void *)(uintptr_t)(kind | (night ? FLAG_NIGHT : 0) | v));
    lv_obj_invalidate(icon);
}

void weather_icon_print(lv_obj_t *icon, bool print) {
    uint32_t v = (uint32_t)(uintptr_t)lv_obj_get_user_data(icon) & ~FLAG_PRINT;
    lv_obj_set_user_data(icon, (void *)(uintptr_t)(v | (print ? FLAG_PRINT : 0)));
    lv_obj_invalidate(icon);
}
