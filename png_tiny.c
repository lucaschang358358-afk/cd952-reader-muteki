#include "png_tiny.h"
#include "inflate_tiny.h"
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

uint8_t *tiny_png_decode(const uint8_t *data, size_t len, int *out_w, int *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    static const uint8_t SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!data || len < 33 || memcmp(data, SIG, 8) != 0) return NULL;

    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint8_t palette[256 * 3];
    int pal_entries = 0;
    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;

    size_t pos = 8;
    while (pos + 12 <= len) {
        uint32_t clen = be32(data + pos);
        const uint8_t *type = data + pos + 4;
        const uint8_t *payload = data + pos + 8;
        if (pos + 12u + clen > len) break;

        if (memcmp(type, "IHDR", 4) == 0 && clen >= 13) {
            w = (int)be32(payload);
            h = (int)be32(payload + 4);
            depth = payload[8];
            ctype = payload[9];
            interlace = payload[12];
        } else if (memcmp(type, "PLTE", 4) == 0 && clen >= 3) {
            pal_entries = (int)(clen / 3u);
            if (pal_entries > 256) pal_entries = 256;
            memcpy(palette, payload, (size_t)pal_entries * 3u);
        } else if (memcmp(type, "IDAT", 4) == 0 && clen > 0) {
            if (idat_len + clen > 8u * 1024u * 1024u) { free(idat); return NULL; }
            if (idat_len + clen > idat_cap) {
                size_t ncap = idat_cap ? idat_cap * 2 : 32768;
                while (ncap < idat_len + clen) ncap *= 2;
                uint8_t *nd = (uint8_t *)realloc(idat, ncap);
                if (!nd) { free(idat); return NULL; }
                idat = nd; idat_cap = ncap;
            }
            memcpy(idat + idat_len, payload, clen);
            idat_len += clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12u + clen;
    }

    if (!idat || idat_len == 0 || w <= 0 || h <= 0 || w > 4096 || h > 4096 || depth != 8 || interlace != 0) {
        free(idat);
        return NULL;
    }

    int bpp = 0;
    if (ctype == 0) bpp = 1;
    else if (ctype == 2) bpp = 3;
    else if (ctype == 3) bpp = 1;
    else if (ctype == 4) bpp = 2;
    else if (ctype == 6) bpp = 4;
    else { free(idat); return NULL; }

    size_t row_bytes = (size_t)w * (size_t)bpp;
    size_t expected = (size_t)h * (row_bytes + 1u);
    size_t raw_len = 0;
    uint8_t *raw = tiny_inflate_zlib_alloc(idat, idat_len, expected + 4096u, &raw_len);
    free(idat);
    if (!raw || raw_len < expected) { free(raw); return NULL; }

    uint8_t *unfilt = (uint8_t *)calloc((size_t)h, row_bytes);
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3u);
    if (!unfilt || !rgb) { free(raw); free(unfilt); free(rgb); return NULL; }

    for (int y = 0; y < h; y++) {
        const uint8_t *src = raw + (size_t)y * (row_bytes + 1u);
        uint8_t ftype = src[0];
        src++;
        uint8_t *dst = unfilt + (size_t)y * row_bytes;
        const uint8_t *prev = (y > 0) ? (unfilt + (size_t)(y - 1) * row_bytes) : NULL;

        for (size_t x = 0; x < row_bytes; x++) {
            int a = (x >= (size_t)bpp) ? dst[x - (size_t)bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= (size_t)bpp) ? prev[x - (size_t)bpp] : 0;
            int v = src[x];
            if (ftype == 1) v = (v + a) & 0xFF;
            else if (ftype == 2) v = (v + b) & 0xFF;
            else if (ftype == 3) v = (v + ((a + b) >> 1)) & 0xFF;
            else if (ftype == 4) v = (v + paeth(a, b, c)) & 0xFF;
            dst[x] = (uint8_t)v;
        }

        uint8_t *out_row = rgb + (size_t)y * (size_t)w * 3u;
        for (int x = 0; x < w; x++) {
            if (ctype == 2) {
                out_row[x * 3 + 0] = dst[x * 3 + 0];
                out_row[x * 3 + 1] = dst[x * 3 + 1];
                out_row[x * 3 + 2] = dst[x * 3 + 2];
            } else if (ctype == 6) {
                int a = dst[x * 4 + 3];
                out_row[x * 3 + 0] = (uint8_t)((dst[x * 4 + 0] * a + 255 * (255 - a)) / 255);
                out_row[x * 3 + 1] = (uint8_t)((dst[x * 4 + 1] * a + 255 * (255 - a)) / 255);
                out_row[x * 3 + 2] = (uint8_t)((dst[x * 4 + 2] * a + 255 * (255 - a)) / 255);
            } else if (ctype == 0) {
                out_row[x * 3 + 0] = out_row[x * 3 + 1] = out_row[x * 3 + 2] = dst[x];
            } else if (ctype == 4) {
                int g = dst[x * 2 + 0], a = dst[x * 2 + 1];
                uint8_t v = (uint8_t)((g * a + 255 * (255 - a)) / 255);
                out_row[x * 3 + 0] = out_row[x * 3 + 1] = out_row[x * 3 + 2] = v;
            } else if (ctype == 3) {
                int idx = dst[x];
                if (idx < pal_entries) {
                    out_row[x * 3 + 0] = palette[idx * 3 + 0];
                    out_row[x * 3 + 1] = palette[idx * 3 + 1];
                    out_row[x * 3 + 2] = palette[idx * 3 + 2];
                } else {
                    out_row[x * 3 + 0] = out_row[x * 3 + 1] = out_row[x * 3 + 2] = 255;
                }
            }
        }
    }

    free(raw);
    free(unfilt);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return rgb;
}
