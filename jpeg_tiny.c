#include "jpeg_tiny.h"
#include <stdlib.h>
#include <string.h>

/* 整數 Loeffler/AAN-style 分離式 1D IDCT (12-bit 定點，免浮點數，ARM9 超高速) */
#define W1 2841 /* 2048*sqrt(2)*cos(1*pi/16) */
#define W2 2676 /* 2048*sqrt(2)*cos(2*pi/16) */
#define W3 2408 /* 2048*sqrt(2)*cos(3*pi/16) */
#define W5 1609 /* 2048*sqrt(2)*cos(5*pi/16) */
#define W6 1108 /* 2048*sqrt(2)*cos(6*pi/16) */
#define W7  565 /* 2048*sqrt(2)*cos(7*pi/16) */

static const uint8_t ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void idct_row(int *blk) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = blk[4] << 11) | (x2 = blk[6]) | (x3 = blk[2]) |
          (x4 = blk[1]) | (x5 = blk[7]) | (x6 = blk[5]) | (x7 = blk[3]))) {
        blk[0] = blk[1] = blk[2] = blk[3] = blk[4] = blk[5] = blk[6] = blk[7] = blk[0] << 3;
        return;
    }
    x0 = (blk[0] << 11) + 128;
    x8 = W7 * (x4 + x5);
    x4 = x8 + (W1 - W7) * x4;
    x5 = x8 - (W1 + W7) * x5;
    x8 = W3 * (x6 + x7);
    x6 = x8 - (W3 - W5) * x6;
    x7 = x8 - (W3 + W5) * x7;

    x8 = x0 + x1; x0 -= x1;
    x1 = W6 * (x3 + x2);
    x2 = x1 - (W2 + W6) * x2;
    x3 = x1 + (W2 - W6) * x3;
    x1 = x4 + x6; x4 -= x6;
    x6 = x5 + x7; x5 -= x7;

    x7 = x8 + x3; x8 -= x3;
    x3 = x0 + x2; x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;

    blk[0] = (x7 + x1) >> 8;
    blk[1] = (x3 + x2) >> 8;
    blk[2] = (x0 + x4) >> 8;
    blk[3] = (x8 + x6) >> 8;
    blk[4] = (x8 - x6) >> 8;
    blk[5] = (x0 - x4) >> 8;
    blk[6] = (x3 - x2) >> 8;
    blk[7] = (x7 - x1) >> 8;
}

static void idct_col(int *blk, uint8_t *out, int stride) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = blk[8*4] << 8) | (x2 = blk[8*6]) | (x3 = blk[8*2]) |
          (x4 = blk[8*1]) | (x5 = blk[8*7]) | (x6 = blk[8*5]) | (x7 = blk[8*3]))) {
        uint8_t v = clamp8(((blk[0] + 32) >> 6) + 128);
        for (int i = 0; i < 8; i++) out[i * stride] = v;
        return;
    }
    x0 = (blk[0] << 8) + 8192;
    x8 = W7 * (x4 + x5) + 4;
    x4 = (x8 + (W1 - W7) * x4) >> 3;
    x5 = (x8 - (W1 + W7) * x5) >> 3;
    x8 = W3 * (x6 + x7) + 4;
    x6 = (x8 - (W3 - W5) * x6) >> 3;
    x7 = (x8 - (W3 + W5) * x7) >> 3;

    x8 = x0 + x1; x0 -= x1;
    x1 = W6 * (x3 + x2) + 4;
    x2 = (x1 - (W2 + W6) * x2) >> 3;
    x3 = (x1 + (W2 - W6) * x3) >> 3;
    x1 = x4 + x6; x4 -= x6;
    x6 = x5 + x7; x5 -= x7;

    x7 = x8 + x3; x8 -= x3;
    x3 = x0 + x2; x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;

    out[0 * stride] = clamp8(((x7 + x1) >> 14) + 128);
    out[1 * stride] = clamp8(((x3 + x2) >> 14) + 128);
    out[2 * stride] = clamp8(((x0 + x4) >> 14) + 128);
    out[3 * stride] = clamp8(((x8 + x6) >> 14) + 128);
    out[4 * stride] = clamp8(((x8 - x6) >> 14) + 128);
    out[5 * stride] = clamp8(((x0 - x4) >> 14) + 128);
    out[6 * stride] = clamp8(((x3 - x2) >> 14) + 128);
    out[7 * stride] = clamp8(((x7 - x1) >> 14) + 128);
}

typedef struct {
    uint8_t counts[16];
    uint8_t syms[256];
    int maxcode[17];
    int valptr[17];
    int valid;
} JpgHuff;

typedef struct {
    int id;
    int hs, vs;
    int qt_id;
    int dc_id, ac_id;
    int dc_pred;
} JpgComp;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;
    int err;

    int width, height;
    int ncomp;
    int max_hs, max_vs;
    int restart_interval;
    uint16_t qtab[4][64];
    JpgHuff huff_dc[4];
    JpgHuff huff_ac[4];
    JpgComp comp[3];
} JpgCtx;

static void build_jpg_huff(JpgHuff *h) {
    int code = 0, idx = 0;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len] = idx - code;
        int c = h->counts[len - 1];
        if (c > 0) {
            code += c;
            idx += c;
            h->maxcode[len] = code - 1;
        } else {
            h->maxcode[len] = -1;
        }
        code <<= 1;
    }
    h->valid = 1;
}

static int next_byte_entropy(JpgCtx *j) {
    if (j->pos >= j->len) return 0;
    uint8_t b = j->data[j->pos++];
    if (b == 0xFF) {
        if (j->pos >= j->len) return 0;
        uint8_t nx = j->data[j->pos++];
        if (nx == 0x00) return 0xFF; /* byte stuffing */
        /* 遇到標記 (例如 RSTm 或 EOI)，回退指標讓上層處理 */
        j->pos -= 2;
        return 0;
    }
    return b;
}

static int jpg_get_bits(JpgCtx *j, int n) {
    if (n == 0) return 0;
    while (j->bitcnt < n) {
        int b = next_byte_entropy(j);
        j->bitbuf = (j->bitbuf << 8) | (uint32_t)b;
        j->bitcnt += 8;
    }
    j->bitcnt -= n;
    return (int)((j->bitbuf >> j->bitcnt) & ((1u << n) - 1u));
}

static int jpg_extend(int v, int n) {
    if (n == 0) return 0;
    int vt = 1 << (n - 1);
    if (v < vt) v += (-(1 << n)) + 1;
    return v;
}

static int jpg_decode_huff(JpgCtx *j, const JpgHuff *h) {
    if (!h->valid) { j->err = 1; return 0; }
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | jpg_get_bits(j, 1);
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len]) {
            int idx = h->valptr[len] + code;
            if (idx >= 0 && idx < 256) return h->syms[idx];
            j->err = 1;
            return 0;
        }
    }
    j->err = 1;
    return 0;
}

static void decode_block(JpgCtx *j, JpgComp *c, uint8_t *out, int stride) {
    int blk[64];
    memset(blk, 0, sizeof(blk));
    const uint16_t *qt = j->qtab[c->qt_id & 3];

    /* DC */
    int s = jpg_decode_huff(j, &j->huff_dc[c->dc_id & 3]);
    int diff = jpg_extend(jpg_get_bits(j, s & 15), s & 15);
    c->dc_pred += diff;
    blk[0] = c->dc_pred * (int)qt[0];

    /* AC */
    for (int k = 1; k < 64; ) {
        int rs = jpg_decode_huff(j, &j->huff_ac[c->ac_id & 3]);
        if (j->err) return;
        int run = (rs >> 4) & 15;
        int sz  = rs & 15;
        if (sz == 0) {
            if (run == 15) { k += 16; continue; }
            break; /* EOB */
        }
        k += run;
        if (k >= 64) break;
        int val = jpg_extend(jpg_get_bits(j, sz), sz);
        blk[ZIGZAG[k]] = val * (int)qt[k];
        k++;
    }

    for (int r = 0; r < 8; r++) idct_row(blk + r * 8);
    for (int col = 0; col < 8; col++) idct_col(blk + col, out + col, stride);
}

uint8_t *tiny_jpeg_decode(const uint8_t *data, size_t len, int *out_w, int *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!data || len < 16 || data[0] != 0xFF || data[1] != 0xD8) return NULL;

    JpgCtx *j = (JpgCtx *)calloc(1, sizeof(JpgCtx));
    if (!j) return NULL;
    j->data = data;
    j->len = len;
    j->pos = 2;

    int sos_found = 0;
    while (j->pos + 2 <= j->len) {
        if (j->data[j->pos] != 0xFF) { j->pos++; continue; }
        while (j->pos < j->len && j->data[j->pos] == 0xFF) j->pos++;
        if (j->pos >= j->len) break;
        uint8_t marker = j->data[j->pos++];
        if (marker == 0xD9) break; /* EOI */
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;

        if (j->pos + 2 > j->len) break;
        uint16_t mlen = ((uint16_t)j->data[j->pos] << 8) | j->data[j->pos + 1];
        if (mlen < 2 || j->pos + mlen > j->len) break;
        const uint8_t *mp = j->data + j->pos + 2;
        size_t pay = mlen - 2;
        j->pos += mlen;

        if (marker == 0xDB) {
            /* DQT */
            size_t o = 0;
            while (o < pay) {
                uint8_t info = mp[o++];
                int prec = info >> 4;
                int id = info & 15;
                if (id > 3) break;
                if (prec == 0 && o + 64 <= pay) {
                    for (int k = 0; k < 64; k++) j->qtab[id][k] = mp[o++];
                } else if (prec == 1 && o + 128 <= pay) {
                    for (int k = 0; k < 64; k++) {
                        j->qtab[id][k] = ((uint16_t)mp[o] << 8) | mp[o + 1];
                        o += 2;
                    }
                } else break;
            }
        } else if (marker == 0xC0 || marker == 0xC1) {
            /* SOF0 / SOF1 Baseline/Extended Sequential */
            if (pay < 6) break;
            j->height = ((int)mp[1] << 8) | mp[2];
            j->width  = ((int)mp[3] << 8) | mp[4];
            j->ncomp  = mp[5];
            if (j->width <= 0 || j->height <= 0 || j->width > 4096 || j->height > 4096) break;
            if (j->ncomp != 1 && j->ncomp != 3) break;
            if (pay < (size_t)(6 + j->ncomp * 3)) break;
            j->max_hs = 1; j->max_vs = 1;
            for (int i = 0; i < j->ncomp; i++) {
                j->comp[i].id = mp[6 + i * 3];
                j->comp[i].hs = mp[6 + i * 3 + 1] >> 4;
                j->comp[i].vs = mp[6 + i * 3 + 1] & 15;
                j->comp[i].qt_id = mp[6 + i * 3 + 2] & 3;
                if (j->comp[i].hs < 1 || j->comp[i].hs > 2 || j->comp[i].vs < 1 || j->comp[i].vs > 2) {
                    j->width = 0; break;
                }
                if (j->comp[i].hs > j->max_hs) j->max_hs = j->comp[i].hs;
                if (j->comp[i].vs > j->max_vs) j->max_vs = j->comp[i].vs;
            }
        } else if (marker == 0xC2) {
            /* Progressive JPEG 不支援 */
            free(j);
            return NULL;
        } else if (marker == 0xC4) {
            /* DHT */
            size_t o = 0;
            while (o + 17 <= pay) {
                uint8_t tc_th = mp[o++];
                int tc = tc_th >> 4;
                int th = tc_th & 15;
                if (th > 3) break;
                JpgHuff *h = (tc == 0) ? &j->huff_dc[th] : &j->huff_ac[th];
                int sum = 0;
                for (int i = 0; i < 16; i++) { h->counts[i] = mp[o++]; sum += h->counts[i]; }
                if (sum > 256 || o + (size_t)sum > pay) break;
                memcpy(h->syms, mp + o, (size_t)sum);
                o += (size_t)sum;
                build_jpg_huff(h);
            }
        } else if (marker == 0xDD) {
            /* DRI */
            if (pay >= 2) j->restart_interval = ((int)mp[0] << 8) | mp[1];
        } else if (marker == 0xDA) {
            /* SOS */
            if (pay < 1) break;
            int ns = mp[0];
            if (pay < (size_t)(1 + ns * 2)) break;
            for (int i = 0; i < ns; i++) {
                int cid = mp[1 + i * 2];
                uint8_t td_ta = mp[1 + i * 2 + 1];
                for (int c = 0; c < j->ncomp; c++) {
                    if (j->comp[c].id == cid) {
                        j->comp[c].dc_id = td_ta >> 4;
                        j->comp[c].ac_id = td_ta & 15;
                    }
                }
            }
            sos_found = 1;
            break;
        }
    }

    if (!sos_found || j->width <= 0 || j->height <= 0) {
        free(j);
        return NULL;
    }

    uint8_t *rgb = (uint8_t *)malloc((size_t)j->width * (size_t)j->height * 3u);
    if (!rgb) { free(j); return NULL; }

    int mcu_w = j->max_hs * 8;
    int mcu_h = j->max_vs * 8;
    int mcus_x = (j->width + mcu_w - 1) / mcu_w;
    int mcus_y = (j->height + mcu_h - 1) / mcu_h;

    uint8_t y_buf[16 * 16];
    uint8_t cb_buf[16 * 16];
    uint8_t cr_buf[16 * 16];
    int mcu_cnt = 0;

    for (int my = 0; my < mcus_y && !j->err; my++) {
        for (int mx = 0; mx < mcus_x && !j->err; mx++) {
            /* 處理 Restart Marker */
            if (j->restart_interval > 0 && mcu_cnt > 0 && (mcu_cnt % j->restart_interval) == 0) {
                j->bitbuf = 0;
                j->bitcnt = 0;
                for (int c = 0; c < j->ncomp; c++) j->comp[c].dc_pred = 0;
                while (j->pos + 1 < j->len && j->data[j->pos] != 0xFF) j->pos++;
                if (j->pos + 1 < j->len && j->data[j->pos] == 0xFF &&
                    j->data[j->pos + 1] >= 0xD0 && j->data[j->pos + 1] <= 0xD7) {
                    j->pos += 2;
                }
            }
            mcu_cnt++;

            if (j->ncomp == 1) {
                decode_block(j, &j->comp[0], y_buf, 8);
                for (int py = 0; py < 8; py++) {
                    int gy = my * 8 + py;
                    if (gy >= j->height) break;
                    for (int px = 0; px < 8; px++) {
                        int gx = mx * 8 + px;
                        if (gx >= j->width) break;
                        uint8_t v = y_buf[py * 8 + px];
                        uint8_t *dst = rgb + ((size_t)gy * (size_t)j->width + (size_t)gx) * 3u;
                        dst[0] = v; dst[1] = v; dst[2] = v;
                    }
                }
            } else {
                /* 3-component YCbCr */
                for (int v = 0; v < j->comp[0].vs; v++) {
                    for (int h = 0; h < j->comp[0].hs; h++) {
                        decode_block(j, &j->comp[0], y_buf + (v * 8) * mcu_w + h * 8, mcu_w);
                    }
                }
                int cb_stride = j->comp[1].hs * 8;
                for (int v = 0; v < j->comp[1].vs; v++) {
                    for (int h = 0; h < j->comp[1].hs; h++) {
                        decode_block(j, &j->comp[1], cb_buf + (v * 8) * cb_stride + h * 8, cb_stride);
                    }
                }
                int cr_stride = j->comp[2].hs * 8;
                for (int v = 0; v < j->comp[2].vs; v++) {
                    for (int h = 0; h < j->comp[2].hs; h++) {
                        decode_block(j, &j->comp[2], cr_buf + (v * 8) * cr_stride + h * 8, cr_stride);
                    }
                }

                int cb_sx = j->max_hs / j->comp[1].hs;
                int cb_sy = j->max_vs / j->comp[1].vs;
                int cr_sx = j->max_hs / j->comp[2].hs;
                int cr_sy = j->max_vs / j->comp[2].vs;

                for (int py = 0; py < mcu_h; py++) {
                    int gy = my * mcu_h + py;
                    if (gy >= j->height) break;
                    uint8_t *row_dst = rgb + ((size_t)gy * (size_t)j->width + (size_t)(mx * mcu_w)) * 3u;
                    for (int px = 0; px < mcu_w; px++) {
                        int gx = mx * mcu_w + px;
                        if (gx >= j->width) break;
                        int Y  = (int)y_buf[py * mcu_w + px];
                        int Cb = (int)cb_buf[(py / cb_sy) * cb_stride + (px / cb_sx)] - 128;
                        int Cr = (int)cr_buf[(py / cr_sy) * cr_stride + (px / cr_sx)] - 128;
                        int R = Y + ((359 * Cr + 128) >> 8);
                        int G = Y - ((88 * Cb + 183 * Cr + 128) >> 8);
                        int B = Y + ((454 * Cb + 128) >> 8);
                        row_dst[0] = clamp8(R);
                        row_dst[1] = clamp8(G);
                        row_dst[2] = clamp8(B);
                        row_dst += 3;
                    }
                }
            }
        }
    }

    int w = j->width, h = j->height;
    free(j);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return rgb;
}
