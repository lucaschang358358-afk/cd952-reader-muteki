#include "inflate_tiny.h"
#include <stdlib.h>
#include <string.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const uint8_t *in;
    size_t in_len;
    size_t in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t out_cap;
    size_t out_pos;
} InfState;

typedef struct {
    int16_t count[MAXBITS + 1];
    int16_t symbol[MAXLCODES];
} HuffTable;

static int bits(InfState *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) return -1;
        val |= ((uint32_t)s->in[s->in_pos++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1u));
}

static int build_huff(HuffTable *h, const int16_t *lengths, int n) {
    int16_t offs[MAXBITS + 1];
    for (int i = 0; i <= MAXBITS; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) {
        if (lengths[i] < 0 || lengths[i] > MAXBITS) return -1;
        h->count[lengths[i]]++;
    }
    if (h->count[0] == n) return 0;

    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }

    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) {
        offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    }
    for (int sym = 0; sym < n; sym++) {
        if (lengths[sym] != 0) {
            h->symbol[offs[lengths[sym]]++] = (int16_t)sym;
        }
    }
    return 0;
}

static int decode_huff(InfState *s, const HuffTable *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int cnt = h->count[len];
        if (code - cnt < first) {
            return h->symbol[index + (code - first)];
        }
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_stored(InfState *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->in_pos + 4 > s->in_len) return -1;
    uint16_t len = (uint16_t)s->in[s->in_pos] | ((uint16_t)s->in[s->in_pos + 1] << 8);
    uint16_t nlen = (uint16_t)s->in[s->in_pos + 2] | ((uint16_t)s->in[s->in_pos + 3] << 8);
    s->in_pos += 4;
    if (((unsigned)len ^ (unsigned)nlen) != 0xFFFFu) return -1;
    if (s->in_pos + len > s->in_len) return -1;
    if (s->out_pos + len > s->out_cap) return -2;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return 0;
}

static int inflate_codes(InfState *s, const HuffTable *lencode, const HuffTable *distcode) {
    static const uint16_t lens[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t lext[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t dists[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const uint8_t dext[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for (;;) {
        int sym = decode_huff(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_cap) return -2;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int ex = bits(s, lext[sym]);
            if (ex < 0) return -1;
            size_t len = (size_t)lens[sym] + (size_t)ex;

            int dsym = decode_huff(s, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            int dex = bits(s, dext[dsym]);
            if (dex < 0) return -1;
            size_t dist = (size_t)dists[dsym] + (size_t)dex;

            if (dist == 0 || dist > s->out_pos) return -1;
            if (s->out_pos + len > s->out_cap) return -2;

            size_t src_off = s->out_pos - dist;
            while (len--) s->out[s->out_pos++] = s->out[src_off++];
        }
    }
}

static int inflate_fixed(InfState *s) {
    HuffTable lencode, distcode;
    int16_t lengths[FIXLCODES];
    int i;
    for (i = 0; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < FIXLCODES; i++) lengths[i] = 8;
    if (build_huff(&lencode, lengths, FIXLCODES) != 0) return -1;
    for (i = 0; i < MAXDCODES; i++) lengths[i] = 5;
    if (build_huff(&distcode, lengths, MAXDCODES) != 0) return -1;
    return inflate_codes(s, &lencode, &distcode);
}

static int inflate_dynamic(InfState *s) {
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -1;

    int16_t lengths[MAXLCODES + MAXDCODES];
    for (int i = 0; i < 19; i++) lengths[i] = 0;
    for (int i = 0; i < ncode; i++) {
        int v = bits(s, 3);
        if (v < 0) return -1;
        lengths[order[i]] = (int16_t)v;
    }

    HuffTable codehuff;
    if (build_huff(&codehuff, lengths, 19) != 0) return -1;

    int idx = 0, total = nlen + ndist;
    while (idx < total) {
        int sym = decode_huff(s, &codehuff);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[idx++] = (int16_t)sym;
        } else {
            int16_t rep_val = 0;
            int rep_cnt = 0;
            if (sym == 16) {
                if (idx == 0) return -1;
                rep_val = lengths[idx - 1];
                int ex = bits(s, 2); if (ex < 0) return -1;
                rep_cnt = 3 + ex;
            } else if (sym == 17) {
                int ex = bits(s, 3); if (ex < 0) return -1;
                rep_cnt = 3 + ex;
            } else if (sym == 18) {
                int ex = bits(s, 7); if (ex < 0) return -1;
                rep_cnt = 11 + ex;
            } else return -1;
            if (idx + rep_cnt > total) return -1;
            while (rep_cnt--) lengths[idx++] = rep_val;
        }
    }

    if (lengths[256] == 0) return -1;
    HuffTable lencode, distcode;
    if (build_huff(&lencode, lengths, nlen) != 0) return -1;
    if (build_huff(&distcode, lengths + nlen, ndist) != 0) return -1;
    return inflate_codes(s, &lencode, &distcode);
}

static int inflate_core(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap, size_t *out_len) {
    InfState s;
    memset(&s, 0, sizeof(s));
    s.in = src; s.in_len = src_len;
    s.out = dst; s.out_cap = dst_cap;

    int last = 0;
    do {
        last = bits(&s, 1);
        int btype = bits(&s, 2);
        if (last < 0 || btype < 0) return -1;
        int err = 0;
        if (btype == 0) err = inflate_stored(&s);
        else if (btype == 1) err = inflate_fixed(&s);
        else if (btype == 2) err = inflate_dynamic(&s);
        else return -1;
        if (err != 0) return err;
    } while (!last);

    *out_len = s.out_pos;
    return 0;
}

int tiny_inflate_raw(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    size_t out_len = 0;
    return (inflate_core(src, src_len, dst, dst_cap, &out_len) == 0) ? (int)out_len : -1;
}

int tiny_inflate_zlib(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    if (!src || src_len < 6) return -1;
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0Fu) != 8u) return -1;
    if ((((uint32_t)cmf << 8) | flg) % 31u != 0u) return -1;
    size_t hdr = 2 + ((flg & 0x20u) ? 4u : 0u);
    if (hdr >= src_len) return -1;
    return tiny_inflate_raw(src + hdr, src_len - hdr, dst, dst_cap);
}

uint8_t *tiny_inflate_raw_alloc(const uint8_t *src, size_t src_len,
                                size_t hint_out_size, size_t max_out_size, size_t *out_len) {
    if (out_len) *out_len = 0;
    size_t cap = hint_out_size > 0 ? hint_out_size : (src_len * 4 + 1024);
    if (cap < 256) cap = 256;
    if (cap > max_out_size) cap = max_out_size;

    while (cap <= max_out_size) {
        uint8_t *buf = (uint8_t *)malloc(cap + 1);
        if (!buf) return NULL;
        size_t got = 0;
        int err = inflate_core(src, src_len, buf, cap, &got);
        if (err == 0) {
            buf[got] = 0;
            if (out_len) *out_len = got;
            return buf;
        }
        free(buf);
        if (err != -2 || cap == max_out_size) return NULL;
        size_t ncap = cap * 2;
        if (ncap > max_out_size) ncap = max_out_size;
        cap = ncap;
    }
    return NULL;
}

uint8_t *tiny_inflate_zlib_alloc(const uint8_t *src, size_t src_len,
                                 size_t max_out_size, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!src || src_len < 6) return NULL;
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0Fu) != 8u) return NULL;
    if ((((uint32_t)cmf << 8) | flg) % 31u != 0u) return NULL;
    size_t hdr = 2 + ((flg & 0x20u) ? 4u : 0u);
    if (hdr >= src_len) return NULL;
    return tiny_inflate_raw_alloc(src + hdr, src_len - hdr, src_len * 4 + 1024, max_out_size, out_len);
}
