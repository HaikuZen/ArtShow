/* thumb.c - box-filter down, then esp_new_jpeg's encoder (RGB565 in, quality
 * 75); its decoder back. Both in software: a thumbnail is a few ms. */
#include "thumb.h"
#include "esp_jpeg_enc.h"
#include "esp_jpeg_dec.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

uint8_t *thumb_encode(const uint16_t *src, int stride, int w, int h, size_t *len, int *tw, int *th) {
    if (w < 1 || h < 1) return NULL;
    /* the box's size in the picture's shape, in whole 16 x 16 blocks (4:2:0's):
       then the decoder's output is exactly that, no padding to allow for */
    int dw = THUMB_W, dh = (int)((int64_t)h * THUMB_W / w);
    if (dh > THUMB_H) {
        dh = THUMB_H;
        dw = (int)((int64_t)w * THUMB_H / h);
    }
    dw &= ~15;
    dh &= ~15;
    if (dw < 16 || dh < 16) return NULL;
    uint16_t *px = heap_caps_aligned_calloc(16, dw * dh, sizeof *px, MALLOC_CAP_SPIRAM);
    uint8_t *out = heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);
    if (!px || !out) goto fail;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * h / dh), y1 = (int)((int64_t)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * w / dw), x1 = (int)((int64_t)(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = (x1 - x0) * (y1 - y0);
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint16_t p = src[(size_t)yy * stride + xx];
                    r += p >> 11;
                    g += (p >> 5) & 0x3f;
                    b += p & 0x1f;
                }
            px[y * dw + x] = (uint16_t)((r / n) << 11 | (g / n) << 5 | b / n);
        }
    }
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = dw;
    cfg.height = dh;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.quality = 75;
    jpeg_enc_handle_t enc = NULL;
    int out_len = 0;
    if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) goto fail;
    jpeg_error_t e = jpeg_enc_process(enc, (const uint8_t *)px, dw * dh * 2, out, 32 * 1024, &out_len);
    jpeg_enc_close(enc);
    if (e != JPEG_ERR_OK || out_len <= 0) goto fail;
    free(px);
    *len = out_len;
    *tw = dw;
    *th = dh;
    return out;
fail:
    free(px);
    free(out);
    return NULL;
}

bool thumb_decode(const uint8_t *jpg, size_t len, uint16_t *dst, int *w, int *h) {
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return false;
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = len };
    jpeg_dec_header_info_t hi;
    int olen = 0;
    jpeg_error_t e = jpeg_dec_parse_header(dec, &io, &hi);
    if (e == JPEG_ERR_OK) e = jpeg_dec_get_outbuf_len(dec, &olen);
    bool ok = e == JPEG_ERR_OK && olen <= THUMB_W * THUMB_H * 2;
    if (ok) {
        io.outbuf = (uint8_t *)dst;
        ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
        *w = hi.width;
        *h = hi.height;
    }
    jpeg_dec_close(dec);
    return ok;
}
