#pragma once
/* a picture's own colours as a soft, dark ground (Now Showing's page) */
#include <stdint.h>

/* src: sw x sh RGB565; dst: dw x dh RGB565 (rows of dw). level: the
   brightness, Q8 (256: as bright as the picture; ~100: dark enough for
   text). Fixed point, at -O2: ~0.23 s for 1024 x 600 on the P4 */
void glow_make(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh, int level);
