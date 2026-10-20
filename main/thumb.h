#pragma once
/* thumbnails for the Gallery: a picture fitted into THUMB_W x THUMB_H and
 * kept as a small JPEG (~6 KB) in the index */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define THUMB_W 176
#define THUMB_H 112

/* the w x h picture at src (rows of stride pixels, RGB565) fitted into the
 * thumbnail's box and encoded: a malloc'd JPEG (free it), its *len and size
 * in *tw x *th; NULL if it fails */
uint8_t *thumb_encode(const uint16_t *src, int stride, int w, int h, size_t *len, int *tw, int *th);
/* a thumbnail's JPEG into dst (THUMB_W * THUMB_H pixels at least, 16-byte
 * aligned): its size */
bool thumb_decode(const uint8_t *jpg, size_t len, uint16_t *dst, int *w, int *h);
