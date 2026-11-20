#ifndef PNG_TINY_H
#define PNG_TINY_H

#include <stddef.h>
#include <stdint.h>

uint8_t *tiny_png_decode(const uint8_t *data, size_t len, int *out_w, int *out_h);

#endif
