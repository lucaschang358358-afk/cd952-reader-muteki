#ifndef INFLATE_TINY_H
#define INFLATE_TINY_H

#include <stddef.h>
#include <stdint.h>

int tiny_inflate_raw(const uint8_t *src, size_t src_len,
                     uint8_t *dst, size_t dst_cap);

int tiny_inflate_zlib(const uint8_t *src, size_t src_len,
                      uint8_t *dst, size_t dst_cap);

uint8_t *tiny_inflate_raw_alloc(const uint8_t *src, size_t src_len,
                                size_t hint_out_size, size_t max_out_size,
                                size_t *out_len);

uint8_t *tiny_inflate_zlib_alloc(const uint8_t *src, size_t src_len,
                                 size_t max_out_size, size_t *out_len);

#endif
