#include "pdf_engine.h"
#include "hal.h"
#include "inflate_tiny.h"
#include "jpeg_tiny.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PDF_MAX_FILE_BYTES  (24u * 1024u * 1024u)
#define PDF_MAX_STREAM      (4u * 1024u * 1024u)
#define PDF_MAX_OBJS        16384
#define PDF_MAX_PAGES       2048
#define PDF_MAX_OBJSTMS     256
#define PDF_CMAP_HASH       4096
#define PDF_MAX_FONT_CMAPS  24
#define PDF_IMG_CACHE_SLOTS 4
#define MAX_XOBJS           48
#define MAX_PAGE_FONTS      32

static inline float f_abs(float v) { return v < 0.0f ? -v : v; }

typedef struct {
    int obj_num;
    const uint8_t *base;
    size_t body_off;
    size_t body_len;
} PdfObjRec;

typedef struct {
    int obj_num;
    float media_x0, media_y0, media_w, media_h;
} PdfPageRec;

typedef struct {
    uint16_t src_code;
    uint16_t dst_u16;
    uint8_t  used;
} PdfCMapEntry;

typedef struct {
    int font_obj_num;
    PdfCMapEntry entries[PDF_CMAP_HASH];
    int count;
    int max_src_bytes;
} PdfCMap;

typedef struct {
    int obj_num;
    int w, h;
    uint8_t *rgb;
    unsigned stamp;
} PdfImgCache;

struct PdfDoc {
    uint8_t *data;
    size_t   len;

    uint8_t *objstm_bufs[PDF_MAX_OBJSTMS];
    int      objstm_count;

    PdfObjRec  *objs;
    int         obj_count;
    PdfPageRec *pages;
    int         page_count;

    PdfCMap  *font_cmaps;
    int       font_cmap_count;
    PdfCMap   global_cmap;

    int       is_encrypted;
    char      title[96];
    char      author[64];

    PdfImgCache img_cache[PDF_IMG_CACHE_SLOTS];
    unsigned    cache_clock;
};

static const char *find_mem(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hay_len) return NULL;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

static const char *find_ci(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hay_len) return NULL;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        while (j < nlen && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) j++;
        if (j == nlen) return hay + i;
    }
    return NULL;
}

static const char *find_stream_kw(const char *buf, size_t len, size_t *out_data_off) {
    size_t pos = 0;
    while (pos + 7 <= len) {
        const char *p = find_mem(buf + pos, len - pos, "stream");
        if (!p) return NULL;
        size_t off = (size_t)(p - buf);
        int ok_prev = (off == 0 || isspace((unsigned char)buf[off - 1]) || buf[off - 1] == '>' || buf[off - 1] == ']');
        size_t q = off + 6;
        while (q < len && (buf[q] == ' ' || buf[q] == '\t')) q++;
        int ok_next = 0;
        if (q < len && buf[q] == '\r') {
            q++;
            if (q < len && buf[q] == '\n') q++;
            ok_next = 1;
        } else if (q < len && buf[q] == '\n') {
            q++;
            ok_next = 1;
        }
        if (ok_prev && ok_next) {
            if (out_data_off) *out_data_off = q;
            return p;
        }
        pos = off + 6;
    }
    return NULL;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void cmap_put(PdfCMap *cm, uint16_t src, uint16_t dst, int src_bytes) {
    if (src_bytes > cm->max_src_bytes) cm->max_src_bytes = src_bytes;
    uint32_t idx = ((uint32_t)src * 2654435761u) & (PDF_CMAP_HASH - 1u);
    for (int step = 0; step < 32; step++) {
        uint32_t slot = (idx + (uint32_t)step) & (PDF_CMAP_HASH - 1u);
        if (!cm->entries[slot].used || cm->entries[slot].src_code == src) {
            if (!cm->entries[slot].used) cm->count++;
            cm->entries[slot].used = 1;
            cm->entries[slot].src_code = src;
            cm->entries[slot].dst_u16 = dst;
            return;
        }
    }
}

static uint16_t cmap_get(const PdfCMap *cm, uint16_t src) {
    if (!cm || cm->count == 0) return 0;
    uint32_t idx = ((uint32_t)src * 2654435761u) & (PDF_CMAP_HASH - 1u);
    for (int step = 0; step < 32; step++) {
        uint32_t slot = (idx + (uint32_t)step) & (PDF_CMAP_HASH - 1u);
        if (!cm->entries[slot].used) return 0;
        if (cm->entries[slot].src_code == src) return cm->entries[slot].dst_u16;
    }
    return 0;
}

static int parse_hex_tok(const char **pp, const char *end, uint32_t *out_val, int *out_digits) {
    const char *p = *pp;
    while (p < end && *p != '<') p++;
    if (p >= end) return 0;
    p++;
    uint32_t v = 0; int d = 0;
    while (p < end && *p != '>') {
        int hv = hex_val(*p);
        if (hv >= 0) { v = (v << 4) | (uint32_t)hv; d++; }
        p++;
    }
    if (p < end && *p == '>') p++;
    *pp = p;
    *out_val = v;
    if (out_digits) *out_digits = d;
    return (d > 0);
}

static void parse_tounicode_cmap(const char *buf, size_t len, PdfCMap *cm) {
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *bfc = find_ci(p, (size_t)(end - p), "beginbfchar");
        const char *bfr = find_ci(p, (size_t)(end - p), "beginbfrange");
        if (!bfc && !bfr) break;
        if (bfc && (!bfr || bfc < bfr)) {
            const char *cur = bfc + 11;
            const char *stop = find_ci(cur, (size_t)(end - cur), "endbfchar");
            if (!stop) break;
            while (cur < stop) {
                uint32_t sc = 0, dc = 0; int sd = 0, dd = 0;
                if (!parse_hex_tok(&cur, stop, &sc, &sd)) break;
                if (!parse_hex_tok(&cur, stop, &dc, &dd)) break;
                if (dd <= 4 && dc > 0) cmap_put(cm, (uint16_t)sc, (uint16_t)dc, (sd + 1) / 2);
            }
            p = stop + 9;
        } else {
            const char *cur = bfr + 12;
            const char *stop = find_ci(cur, (size_t)(end - cur), "endbfrange");
            if (!stop) break;
            while (cur < stop) {
                uint32_t s0 = 0, s1 = 0; int d0 = 0, d1 = 0;
                if (!parse_hex_tok(&cur, stop, &s0, &d0)) break;
                if (!parse_hex_tok(&cur, stop, &s1, &d1)) break;
                int sbytes = (d0 + 1) / 2;
                while (cur < stop && isspace((unsigned char)*cur)) cur++;
                if (cur < stop && *cur == '[') {
                    cur++;
                    for (uint32_t c = s0; c <= s1 && c < s0 + 256 && cur < stop; c++) {
                        while (cur < stop && isspace((unsigned char)*cur)) cur++;
                        if (cur >= stop || *cur == ']') break;
                        uint32_t dc = 0; int dd = 0;
                        if (parse_hex_tok(&cur, stop, &dc, &dd) && dd <= 4 && dc > 0)
                            cmap_put(cm, (uint16_t)c, (uint16_t)dc, sbytes);
                    }
                    while (cur < stop && *cur != ']') cur++;
                    if (cur < stop) cur++;
                } else {
                    uint32_t db = 0; int dd = 0;
                    if (parse_hex_tok(&cur, stop, &db, &dd) && dd <= 4) {
                        for (uint32_t c = s0; c <= s1 && c < s0 + 512; c++)
                            cmap_put(cm, (uint16_t)c, (uint16_t)(db + (c - s0)), sbytes);
                    }
                }
            }
            p = stop + 10;
        }
    }
}

static const PdfObjRec *find_obj(const PdfDoc *doc, int obj_num) {
    for (int i = doc->obj_count - 1; i >= 0; i--) {
        if (doc->objs[i].obj_num == obj_num) return &doc->objs[i];
    }
    return NULL;
}

static int parse_ref_after(const char *p, const char *end, int *out_obj) {
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || !isdigit((unsigned char)*p)) return 0;
    char *ep1 = NULL;
    long onum = strtol(p, &ep1, 10);
    if (!ep1 || ep1 >= end || !isspace((unsigned char)*ep1)) return 0;
    p = ep1;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || !isdigit((unsigned char)*p)) return 0;
    char *ep2 = NULL;
    (void)strtol(p, &ep2, 10);
    if (!ep2 || ep2 >= end) return 0;
    p = ep2;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p < end && *p == 'R') {
        *out_obj = (int)onum;
        return 1;
    }
    return 0;
}

static int dict_get_int_direct_or_ref(const PdfDoc *doc, const char *dict, size_t dlen, const char *key, int *out_val) {
    const char *p = find_mem(dict, dlen, key);
    if (!p) return 0;
    p += strlen(key);
    const char *end = dict + dlen;
    while (p < end && isspace((unsigned char)*p)) p++;
    int ref_num = 0;
    if (doc && parse_ref_after(p, end, &ref_num)) {
        const PdfObjRec *ro = find_obj(doc, ref_num);
        if (ro) {
            const char *rb = (const char *)(ro->base + ro->body_off);
            size_t i = 0;
            while (i < ro->body_len && isspace((unsigned char)rb[i])) i++;
            if (i < ro->body_len && (rb[i] == '-' || isdigit((unsigned char)rb[i]))) {
                *out_val = (int)strtol(rb + i, NULL, 10);
                return 1;
            }
        }
        return 0;
    }
    if (p >= end || (*p != '-' && !isdigit((unsigned char)*p))) return 0;
    *out_val = (int)strtol(p, NULL, 10);
    return 1;
}

static int dict_get_ref(const char *dict, size_t dlen, const char *key, int *out_obj) {
    const char *p = find_mem(dict, dlen, key);
    if (!p) return 0;
    p += strlen(key);
    return parse_ref_after(p, dict + dlen, out_obj);
}

static int dict_get_mediabox(const char *dict, size_t dlen, float *x0, float *y0, float *w, float *h) {
    const char *p = find_mem(dict, dlen, "/CropBox");
    if (!p) p = find_mem(dict, dlen, "/MediaBox");
    if (!p) return 0;
    const char *end = dict + dlen;
    while (p < end && *p != '[') p++;
    if (p >= end) return 0;
    p++;
    float vals[4] = { 0, 0, 595.0f, 842.0f };
    for (int i = 0; i < 4; i++) {
        while (p < end && isspace((unsigned char)*p)) p++;
        char *ep = NULL;
        vals[i] = strtof(p, &ep);
        if (!ep || ep == p) return 0;
        p = ep;
    }
    *x0 = vals[0]; *y0 = vals[1];
    *w = vals[2] - vals[0]; *h = vals[3] - vals[1];
    if (*w < 0) { *x0 += *w; *w = -*w; }
    if (*h < 0) { *y0 += *h; *h = -*h; }
    if (*w < 50.0f) *w = 595.0f;
    if (*h < 50.0f) *h = 842.0f;
    return 1;
}

static uint8_t *decode_ascii_hex(const uint8_t *src, size_t slen, size_t *out_len) {
    uint8_t *out = (uint8_t *)malloc(slen / 2 + 4);
    if (!out) return NULL;
    size_t o = 0; int hi = -1;
    for (size_t i = 0; i < slen; i++) {
        if (src[i] == '>') break;
        int hv = hex_val((char)src[i]);
        if (hv < 0) continue;
        if (hi < 0) hi = hv;
        else { out[o++] = (uint8_t)((hi << 4) | hv); hi = -1; }
    }
    if (hi >= 0) out[o++] = (uint8_t)(hi << 4);
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

static uint8_t *decode_ascii85(const uint8_t *src, size_t slen, size_t *out_len) {
    uint8_t *out = (uint8_t *)malloc(slen + 8);
    if (!out) return NULL;
    size_t o = 0; uint32_t tuple = 0; int cnt = 0;
    for (size_t i = 0; i < slen; i++) {
        uint8_t c = src[i];
        if (c == '~') break;
        if (isspace(c)) continue;
        if (c == 'z' && cnt == 0) {
            out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;
            continue;
        }
        if (c < '!' || c > 'u') continue;
        tuple = tuple * 85u + (uint32_t)(c - '!');
        cnt++;
        if (cnt == 5) {
            out[o++] = (uint8_t)(tuple >> 24);
            out[o++] = (uint8_t)(tuple >> 16);
            out[o++] = (uint8_t)(tuple >> 8);
            out[o++] = (uint8_t)(tuple);
            tuple = 0; cnt = 0;
        }
    }
    if (cnt > 1) {
        for (int k = cnt; k < 5; k++) tuple = tuple * 85u + 84u;
        for (int k = 0; k < cnt - 1; k++) out[o++] = (uint8_t)(tuple >> (24 - k * 8));
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

static uint8_t *extract_obj_stream(const PdfDoc *doc, const PdfObjRec *o, size_t *out_len, int *out_is_dct) {
    if (out_len) *out_len = 0;
    if (out_is_dct) *out_is_dct = 0;
    if (!o) return NULL;
    const char *body = (const char *)(o->base + o->body_off);
    size_t blen = o->body_len;

    size_t doff = 0;
    const char *st = find_stream_kw(body, blen, &doff);
    if (!st) return NULL;
    size_t dict_len = (size_t)(st - body);

    size_t dlen = 0;
    int decl_len = 0;
    if (dict_get_int_direct_or_ref(doc, body, dict_len, "/Length", &decl_len) && decl_len > 0 && doff + (size_t)decl_len <= blen) {
        dlen = (size_t)decl_len;
    } else {
        const char *en = find_mem(body + doff, blen - doff, "endstream");
        if (!en) return NULL;
        dlen = (size_t)(en - (body + doff));
        while (dlen > 0 && (body[doff + dlen - 1] == '\n' || body[doff + dlen - 1] == '\r')) dlen--;
    }

    int is_dct   = (find_mem(body, dict_len, "/DCTDecode") != NULL);
    int is_flate = (find_mem(body, dict_len, "/FlateDecode") != NULL);
    int is_a85   = (find_mem(body, dict_len, "/ASCII85Decode") != NULL);
    int is_ahex  = (find_mem(body, dict_len, "/ASCIIHexDecode") != NULL);
    if (out_is_dct) *out_is_dct = is_dct;

    const uint8_t *cur = (const uint8_t *)(body + doff);
    size_t clen = dlen;
    uint8_t *tmp_owned = NULL;

    if (is_a85) {
        size_t nlen = 0;
        uint8_t *dec = decode_ascii85(cur, clen, &nlen);
        if (!dec) return NULL;
        tmp_owned = dec; cur = dec; clen = nlen;
    } else if (is_ahex) {
        size_t nlen = 0;
        uint8_t *dec = decode_ascii_hex(cur, clen, &nlen);
        if (!dec) return NULL;
        tmp_owned = dec; cur = dec; clen = nlen;
    }

    if (is_flate) {
        size_t nlen = 0;
        uint8_t *dec = tiny_inflate_zlib_alloc(cur, clen, PDF_MAX_STREAM, &nlen);
        free(tmp_owned);
        if (!dec) return NULL;
        if (out_len) *out_len = nlen;
        return dec;
    }

    if (tmp_owned) {
        if (out_len) *out_len = clen;
        return tmp_owned;
    }

    uint8_t *buf = (uint8_t *)malloc(clen + 1);
    if (!buf) return NULL;
    memcpy(buf, cur, clen);
    buf[clen] = 0;
    if (out_len) *out_len = clen;
    return buf;
}

static void unpack_object_streams(PdfDoc *doc) {
    int orig_count = doc->obj_count;
    for (int i = 0; i < orig_count && doc->objstm_count < PDF_MAX_OBJSTMS; i++) {
        const PdfObjRec *o = &doc->objs[i];
        const char *b = (const char *)(o->base + o->body_off);
        size_t doff = 0;
        const char *st = find_stream_kw(b, o->body_len, &doff);
        if (!st) continue;
        size_t dlen = (size_t)(st - b);
        if (!find_mem(b, dlen, "/ObjStm")) continue;

        int n_objs = 0, first_off = 0;
        if (!dict_get_int_direct_or_ref(doc, b, dlen, "/N", &n_objs) || n_objs <= 0) continue;
        if (!dict_get_int_direct_or_ref(doc, b, dlen, "/First", &first_off) || first_off <= 0) continue;

        size_t slen = 0; int dct = 0;
        uint8_t *unpacked = extract_obj_stream(doc, o, &slen, &dct);
        if (!unpacked || dct || (size_t)first_off >= slen) { free(unpacked); continue; }

        doc->objstm_bufs[doc->objstm_count++] = unpacked;

        int *nums = (int *)malloc((size_t)n_objs * sizeof(int));
        int *offs = (int *)malloc((size_t)n_objs * sizeof(int));
        if (!nums || !offs) { free(nums); free(offs); continue; }

        const char *p = (const char *)unpacked;
        const char *hdr_end = p + first_off;
        int parsed = 0;
        for (int k = 0; k < n_objs; k++) {
            while (p < hdr_end && isspace((unsigned char)*p)) p++;
            if (p >= hdr_end) break;
            char *ep1 = NULL; long onum = strtol(p, &ep1, 10);
            if (!ep1 || ep1 == p) break;
            p = ep1;
            while (p < hdr_end && isspace((unsigned char)*p)) p++;
            if (p >= hdr_end) break;
            char *ep2 = NULL; long roff = strtol(p, &ep2, 10);
            if (!ep2 || ep2 == p) break;
            p = ep2;
            nums[parsed] = (int)onum;
            offs[parsed] = (int)roff;
            parsed++;
        }

        for (int k = 0; k < parsed && doc->obj_count < PDF_MAX_OBJS; k++) {
            size_t start = (size_t)first_off + (size_t)offs[k];
            size_t end = (k + 1 < parsed) ? ((size_t)first_off + (size_t)offs[k + 1]) : slen;
            if (start < slen && end >= start && end <= slen) {
                PdfObjRec *rec = &doc->objs[doc->obj_count++];
                rec->obj_num = nums[k];
                rec->base = unpacked;
                rec->body_off = start;
                rec->body_len = end - start;
            }
        }
        free(nums); free(offs);
    }
}

static void collect_pages_tree(PdfDoc *doc, int obj_num, float inh_x0, float inh_y0, float inh_w, float inh_h, int depth) {
    if (depth > 16 || doc->page_count >= PDF_MAX_PAGES) return;
    const PdfObjRec *o = find_obj(doc, obj_num);
    if (!o) return;

    const char *body = (const char *)(o->base + o->body_off);
    size_t blen = o->body_len;
    const char *st = find_stream_kw(body, blen, NULL);
    size_t dlen = st ? (size_t)(st - body) : blen;

    float x0 = inh_x0, y0 = inh_y0, w = inh_w, h = inh_h;
    dict_get_mediabox(body, dlen, &x0, &y0, &w, &h);

    const char *kids = find_mem(body, dlen, "/Kids");
    if (kids) {
        const char *p = kids + 5, *end = body + dlen;
        while (p < end && *p != '[') p++;
        if (p < end) {
            p++;
            while (p < end && *p != ']') {
                int child = 0;
                if (parse_ref_after(p, end, &child)) {
                    collect_pages_tree(doc, child, x0, y0, w, h, depth + 1);
                    while (p < end && *p != 'R' && *p != ']') p++;
                    if (p < end && *p == 'R') p++;
                } else p++;
            }
        }
        return;
    }

    PdfPageRec *pr = &doc->pages[doc->page_count++];
    pr->obj_num = obj_num;
    pr->media_x0 = x0; pr->media_y0 = y0;
    pr->media_w = w;   pr->media_h = h;
}

static int is_type_page_only(const char *dict, size_t dlen) {
    const char *p = find_mem(dict, dlen, "/Type");
    while (p) {
        const char *q = p + 5, *end = dict + dlen;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (q + 5 <= end && memcmp(q, "/Page", 5) == 0 && (q + 5 == end || !isalpha((unsigned char)q[5]))) return 1;
        p = find_mem(q, (size_t)(end - q), "/Type");
    }
    return 0;
}

static const PdfCMap *find_font_cmap(const PdfDoc *doc, int font_obj_num) {
    for (int i = 0; i < doc->font_cmap_count; i++) {
        if (doc->font_cmaps[i].font_obj_num == font_obj_num) return &doc->font_cmaps[i];
    }
    return &doc->global_cmap;
}

PdfDoc *pdf_open(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 16 || (size_t)sz > PDF_MAX_FILE_BYTES) { fclose(f); return NULL; }

    uint8_t *data = (uint8_t *)malloc((size_t)sz + 1);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[got] = 0;

    if (!find_mem((const char *)data, got < 1024 ? got : 1024, "%PDF-")) { free(data); return NULL; }

    PdfDoc *doc = (PdfDoc *)calloc(1, sizeof(PdfDoc));
    if (!doc) { free(data); return NULL; }
    doc->data = data; doc->len = got;
    doc->objs = (PdfObjRec *)calloc(PDF_MAX_OBJS, sizeof(PdfObjRec));
    doc->pages = (PdfPageRec *)calloc(PDF_MAX_PAGES, sizeof(PdfPageRec));
    doc->font_cmaps = (PdfCMap *)calloc(PDF_MAX_FONT_CMAPS, sizeof(PdfCMap));
    if (!doc->objs || !doc->pages || !doc->font_cmaps) { pdf_close(doc); return NULL; }

    const char *s = (const char *)data;
    if (find_mem(s, got, "/Encrypt")) doc->is_encrypted = 1;

    size_t pos = 0;
    while (pos + 8 < got && doc->obj_count < PDF_MAX_OBJS) {
        if (isdigit((unsigned char)s[pos]) && (pos == 0 || isspace((unsigned char)s[pos - 1]))) {
            char *ep1 = NULL;
            long onum = strtol(s + pos, &ep1, 10);
            if (ep1 && (size_t)(ep1 - s) < got && (*ep1 == ' ' || *ep1 == '\t')) {
                char *ep2 = NULL;
                long gen = strtol(ep1, &ep2, 10);
                if (gen >= 0 && ep2 && (size_t)(ep2 - s) + 4 <= got &&
                    (*ep2 == ' ' || *ep2 == '\t') && memcmp(ep2 + 1, "obj", 3) == 0 &&
                    ((size_t)(ep2 + 4 - s) == got || isspace((unsigned char)ep2[4]) || ep2[4] == '<' || ep2[4] == '[')) {
                    size_t boff = (size_t)(ep2 + 4 - s);
                    const char *eobj = find_mem(s + boff, got - boff, "endobj");
                    size_t blen = eobj ? (size_t)(eobj - (s + boff)) : (got - boff);

                    PdfObjRec *rec = &doc->objs[doc->obj_count++];
                    rec->obj_num = (int)onum;
                    rec->base = data;
                    rec->body_off = boff;
                    rec->body_len = blen;
                    pos = boff + blen + 6;
                    continue;
                }
            }
        }
        pos++;
    }

    unpack_object_streams(doc);

    int root_obj = 0, pages_obj = 0;
    const char *rp = s;
    while (rp < s + got) {
        const char *rk = find_mem(rp, (size_t)(s + got - rp), "/Root");
        if (!rk) break;
        int cand = 0;
        if (parse_ref_after(rk + 5, s + got, &cand) && cand > 0) root_obj = cand;
        rp = rk + 5;
    }
    if (root_obj > 0) {
        const PdfObjRec *ro = find_obj(doc, root_obj);
        if (ro) dict_get_ref((const char *)(ro->base + ro->body_off), ro->body_len, "/Pages", &pages_obj);
    }
    if (pages_obj > 0) collect_pages_tree(doc, pages_obj, 0, 0, 595.0f, 842.0f, 0);

    if (doc->page_count == 0) {
        for (int i = 0; i < doc->obj_count && doc->page_count < PDF_MAX_PAGES; i++) {
            const char *b = (const char *)(doc->objs[i].base + doc->objs[i].body_off);
            size_t bl = doc->objs[i].body_len;
            const char *st = find_stream_kw(b, bl, NULL);
            size_t dl = st ? (size_t)(st - b) : bl;
            if (is_type_page_only(b, dl)) {
                PdfPageRec *pr = &doc->pages[doc->page_count++];
                pr->obj_num = doc->objs[i].obj_num;
                pr->media_x0 = 0; pr->media_y0 = 0;
                pr->media_w = 595.0f; pr->media_h = 842.0f;
                dict_get_mediabox(b, dl, &pr->media_x0, &pr->media_y0, &pr->media_w, &pr->media_h);
            }
        }
    }

    for (int i = 0; i < doc->obj_count; i++) {
        const PdfObjRec *o = &doc->objs[i];
        const char *b = (const char *)(o->base + o->body_off);
        size_t bl = o->body_len;
        const char *st = find_stream_kw(b, bl, NULL);
        size_t dl = st ? (size_t)(st - b) : bl;

        int cm_obj = 0;
        if (dict_get_ref(b, dl, "/ToUnicode", &cm_obj)) {
            const PdfObjRec *co = find_obj(doc, cm_obj);
            if (co) {
                size_t clen = 0; int dct = 0;
                uint8_t *cd = extract_obj_stream(doc, co, &clen, &dct);
                if (cd && !dct) {
                    if (doc->font_cmap_count < PDF_MAX_FONT_CMAPS) {
                        PdfCMap *fc = &doc->font_cmaps[doc->font_cmap_count++];
                        fc->font_obj_num = o->obj_num;
                        parse_tounicode_cmap((const char *)cd, clen, fc);
                    }
                    parse_tounicode_cmap((const char *)cd, clen, &doc->global_cmap);
                }
                free(cd);
            }
        } else if (st && !find_mem(b, dl, "/Image") && !find_mem(b, dl, "/DCTDecode") && !find_mem(b, dl, "/ObjStm") && bl < 140000) {
            size_t clen = 0; int dct = 0;
            uint8_t *cd = extract_obj_stream(doc, o, &clen, &dct);
            if (cd && !dct && (find_ci((const char *)cd, clen, "beginbfchar") || find_ci((const char *)cd, clen, "beginbfrange"))) {
                parse_tounicode_cmap((const char *)cd, clen, &doc->global_cmap);
            }
            free(cd);
        }
    }

    if (doc->page_count == 0) { pdf_close(doc); return NULL; }
    return doc;
}

void pdf_close(PdfDoc *doc) {
    if (!doc) return;
    for (int i = 0; i < PDF_IMG_CACHE_SLOTS; i++) free(doc->img_cache[i].rgb);
    for (int i = 0; i < doc->objstm_count; i++) free(doc->objstm_bufs[i]);
    free(doc->font_cmaps);
    free(doc->pages);
    free(doc->objs);
    free(doc->data);
    free(doc);
}

int pdf_page_count(const PdfDoc *doc) { return doc ? doc->page_count : 0; }
const char *pdf_get_title(const PdfDoc *doc) { return doc ? doc->title : ""; }
const char *pdf_get_author(const PdfDoc *doc) { return doc ? doc->author : ""; }

void pdf_get_page_size(const PdfDoc *doc, int page_idx, float *out_w, float *out_h) {
    if (!doc || page_idx < 0 || page_idx >= doc->page_count) {
        if (out_w) *out_w = 595.0f;
        if (out_h) *out_h = 842.0f;
        return;
    }
    if (out_w) *out_w = doc->pages[page_idx].media_w;
    if (out_h) *out_h = doc->pages[page_idx].media_h;
}

static int paeth_pred(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static uint8_t *decode_pdf_image_obj(PdfDoc *doc, int obj_num, int *out_w, int *out_h, char *out_err, size_t err_sz) {
    *out_w = 0; *out_h = 0;
    for (int i = 0; i < PDF_IMG_CACHE_SLOTS; i++) {
        if (doc->img_cache[i].rgb && doc->img_cache[i].obj_num == obj_num) {
            doc->img_cache[i].stamp = ++doc->cache_clock;
            *out_w = doc->img_cache[i].w; *out_h = doc->img_cache[i].h;
            return doc->img_cache[i].rgb;
        }
    }

    const PdfObjRec *o = find_obj(doc, obj_num);
    if (!o) return NULL;
    const char *body = (const char *)(o->base + o->body_off);
    const char *st = find_stream_kw(body, o->body_len, NULL);
    if (!st) return NULL;
    size_t dlen = (size_t)(st - body);
    if (!find_mem(body, dlen, "/Image")) return NULL;

    if (find_mem(body, dlen, "/JPXDecode")) { if (out_err) snprintf(out_err, err_sz, "JPEG2000 (/JPXDecode)"); return NULL; }
    if (find_mem(body, dlen, "/JBIG2Decode")) { if (out_err) snprintf(out_err, err_sz, "JBIG2 (/JBIG2Decode)"); return NULL; }
    if (find_mem(body, dlen, "/CCITTFaxDecode")) { if (out_err) snprintf(out_err, err_sz, "CCITT Fax"); return NULL; }

    int iw = 0, ih = 0, bpc = 8, predictor = 1;
    dict_get_int_direct_or_ref(doc, body, dlen, "/Width", &iw);
    dict_get_int_direct_or_ref(doc, body, dlen, "/Height", &ih);
    dict_get_int_direct_or_ref(doc, body, dlen, "/BitsPerComponent", &bpc);
    dict_get_int_direct_or_ref(doc, body, dlen, "/Predictor", &predictor);

    size_t slen = 0; int is_dct = 0;
    uint8_t *raw = extract_obj_stream(doc, o, &slen, &is_dct);
    if (!raw || slen == 0) { free(raw); return NULL; }

    uint8_t *rgb = NULL;
    if (is_dct) {
        rgb = tiny_jpeg_decode(raw, slen, &iw, &ih);
        if (!rgb && out_err) snprintf(out_err, err_sz, "漸進式 JPEG");
        free(raw);
    } else {
        if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096) { free(raw); return NULL; }
        if (bpc == 1) {
            size_t row_bytes = (size_t)((iw + 7) / 8);
            if (slen >= (size_t)ih * row_bytes) {
                rgb = (uint8_t *)malloc((size_t)iw * (size_t)ih * 3u);
                if (rgb) {
                    int inv_mask = (find_mem(body, dlen, "/ImageMask") != NULL);
                    for (int y = 0; y < ih; y++) {
                        const uint8_t *sr = raw + (size_t)y * row_bytes;
                        uint8_t *dr = rgb + (size_t)y * (size_t)iw * 3u;
                        for (int x = 0; x < iw; x++) {
                            int bit = (sr[x >> 3] >> (7 - (x & 7))) & 1;
                            uint8_t v = (bit ^ inv_mask) ? 255 : 0;
                            dr[x * 3] = dr[x * 3 + 1] = dr[x * 3 + 2] = v;
                        }
                    }
                }
            }
            free(raw);
        } else if (bpc == 8) {
            int comps = 3;
            if (find_mem(body, dlen, "/Indexed") || find_mem(body, dlen, "/DeviceGray")) comps = 1;
            else if (find_mem(body, dlen, "/DeviceCMYK")) comps = 4;

            size_t stride = (size_t)iw * (size_t)comps;
            uint8_t *pix = raw, *unfilt = NULL;
            if (predictor >= 10 && slen >= (size_t)ih * (stride + 1)) {
                unfilt = (uint8_t *)calloc((size_t)ih, stride);
                if (unfilt) {
                    for (int y = 0; y < ih; y++) {
                        const uint8_t *src = raw + (size_t)y * (stride + 1);
                        uint8_t ftype = src[0]; src++;
                        uint8_t *dst = unfilt + (size_t)y * stride;
                        const uint8_t *prev = (y > 0) ? (unfilt + (size_t)(y - 1) * stride) : NULL;
                        for (size_t x = 0; x < stride; x++) {
                            int a = (x >= (size_t)comps) ? dst[x - (size_t)comps] : 0;
                            int b = prev ? prev[x] : 0;
                            int c = (prev && x >= (size_t)comps) ? prev[x - (size_t)comps] : 0;
                            int v = src[x];
                            if (ftype == 1) v = (v + a) & 0xFF;
                            else if (ftype == 2) v = (v + b) & 0xFF;
                            else if (ftype == 3) v = (v + ((a + b) >> 1)) & 0xFF;
                            else if (ftype == 4) v = (v + paeth_pred(a, b, c)) & 0xFF;
                            dst[x] = (uint8_t)v;
                        }
                    }
                    pix = unfilt; slen = (size_t)ih * stride;
                }
            }

            if (slen >= (size_t)ih * stride) {
                rgb = (uint8_t *)malloc((size_t)iw * (size_t)ih * 3u);
                if (rgb) {
                    size_t total = (size_t)iw * (size_t)ih;
                    if (comps == 3) memcpy(rgb, pix, total * 3u);
                    else if (comps == 1) {
                        for (size_t k = 0; k < total; k++) rgb[k * 3] = rgb[k * 3 + 1] = rgb[k * 3 + 2] = pix[k];
                    } else {
                        for (size_t k = 0; k < total; k++) {
                            int c = pix[k * 4], m = pix[k * 4 + 1], y = pix[k * 4 + 2], kk = pix[k * 4 + 3];
                            rgb[k * 3 + 0] = (uint8_t)(((255 - c) * (255 - kk)) / 255);
                            rgb[k * 3 + 1] = (uint8_t)(((255 - m) * (255 - kk)) / 255);
                            rgb[k * 3 + 2] = (uint8_t)(((255 - y) * (255 - kk)) / 255);
                        }
                    }
                }
            }
            free(unfilt); free(raw);
        } else {
            free(raw);
        }
    }

    if (!rgb) return NULL;
    int slot = 0;
    for (int i = 0; i < PDF_IMG_CACHE_SLOTS; i++) {
        if (!doc->img_cache[i].rgb) { slot = i; break; }
        if (doc->img_cache[i].stamp < doc->img_cache[slot].stamp) slot = i;
    }
    free(doc->img_cache[slot].rgb);
    doc->img_cache[slot].obj_num = obj_num;
    doc->img_cache[slot].w = iw; doc->img_cache[slot].h = ih;
    doc->img_cache[slot].rgb = rgb; doc->img_cache[slot].stamp = ++doc->cache_clock;
    *out_w = iw; *out_h = ih;
    return rgb;
}

typedef struct { char name[32]; int obj_num; } ResMap;

static void parse_subdict_refs(const PdfDoc *doc, const char *res, size_t rlen, const char *subkey, ResMap *maps, int *cnt, int max_cnt) {
    const char *sub = find_mem(res, rlen, subkey);
    if (!sub) return;
    const char *p = sub + strlen(subkey), *end = res + rlen;
    while (p < end && isspace((unsigned char)*p)) p++;

    int ref_obj = 0;
    if (parse_ref_after(p, end, &ref_obj)) {
        const PdfObjRec *o = find_obj(doc, ref_obj);
        if (!o) return;
        p = (const char *)(o->base + o->body_off);
        end = p + o->body_len;
    }

    const char *op = find_mem(p, (size_t)(end - p), "<<");
    if (!op) return;
    p = op + 2;
    int depth = 1;
    while (p < end && depth > 0 && *cnt < max_cnt) {
        if (p + 1 < end && p[0] == '<' && p[1] == '<') { depth++; p += 2; continue; }
        if (p + 1 < end && p[0] == '>' && p[1] == '>') { depth--; p += 2; continue; }
        if (depth == 1 && *p == '/') {
            p++;
            char nm[32]; size_t ni = 0;
            while (p < end && ni + 1 < sizeof(nm) && !isspace((unsigned char)*p) && *p != '/' && *p != '<' && *p != '>') nm[ni++] = *p++;
            nm[ni] = '\0';
            int tobj = 0;
            if (nm[0] && parse_ref_after(p, end, &tobj)) {
                snprintf(maps[*cnt].name, sizeof(maps[*cnt].name), "%s", nm);
                maps[*cnt].obj_num = tobj;
                (*cnt)++;
            }
        } else p++;
    }
}

static void collect_resources(const PdfDoc *doc, const PdfObjRec *obj,
                              ResMap *xobjs, int *xcnt, ResMap *fonts, int *fcnt) {
    const char *body = (const char *)(obj->base + obj->body_off);
    const char *st = find_stream_kw(body, obj->body_len, NULL);
    size_t dlen = st ? (size_t)(st - body) : obj->body_len;

    const char *res = find_mem(body, dlen, "/Resources");
    if (res) {
        int res_obj = 0;
        if (dict_get_ref(res, (size_t)(body + dlen - res), "/Resources", &res_obj)) {
            const PdfObjRec *ro = find_obj(doc, res_obj);
            if (ro) {
                const char *rb = (const char *)(ro->base + ro->body_off);
                parse_subdict_refs(doc, rb, ro->body_len, "/XObject", xobjs, xcnt, MAX_XOBJS);
                parse_subdict_refs(doc, rb, ro->body_len, "/Font", fonts, fcnt, MAX_PAGE_FONTS);
            }
        } else {
            size_t rrem = (size_t)(body + dlen - res);
            parse_subdict_refs(doc, res, rrem, "/XObject", xobjs, xcnt, MAX_XOBJS);
            parse_subdict_refs(doc, res, rrem, "/Font", fonts, fcnt, MAX_PAGE_FONTS);
        }
    }
    int parent_obj = 0;
    if (dict_get_ref(body, dlen, "/Parent", &parent_obj)) {
        const PdfObjRec *po = find_obj(doc, parent_obj);
        if (po) {
            const char *pb = (const char *)(po->base + po->body_off);
            parse_subdict_refs(doc, pb, po->body_len, "/XObject", xobjs, xcnt, MAX_XOBJS);
            parse_subdict_refs(doc, pb, po->body_len, "/Font", fonts, fcnt, MAX_PAGE_FONTS);
        }
    }
}

static uint8_t *get_page_content_stream(const PdfDoc *doc, const PdfObjRec *page_obj, size_t *out_len) {
    *out_len = 0;
    const char *body = (const char *)(page_obj->base + page_obj->body_off);
    const char *st = find_stream_kw(body, page_obj->body_len, NULL);
    size_t dlen = st ? (size_t)(st - body) : page_obj->body_len;

    const char *cptr = find_mem(body, dlen, "/Contents");
    if (!cptr) return NULL;
    const char *p = cptr + 9, *end = body + dlen;
    while (p < end && isspace((unsigned char)*p)) p++;

    int single_obj = 0;
    if (parse_ref_after(p, end, &single_obj)) {
        const PdfObjRec *co = find_obj(doc, single_obj);
        int dct = 0;
        return extract_obj_stream(doc, co, out_len, &dct);
    }

    if (p < end && *p == '[') {
        p++;
        uint8_t *merged = NULL; size_t mlen = 0;
        while (p < end && *p != ']') {
            int co_num = 0;
            if (parse_ref_after(p, end, &co_num)) {
                const PdfObjRec *co = find_obj(doc, co_num);
                size_t clen = 0; int dct = 0;
                uint8_t *part = extract_obj_stream(doc, co, &clen, &dct);
                if (part && clen > 0 && mlen + clen + 2 < PDF_MAX_STREAM) {
                    uint8_t *nm = (uint8_t *)realloc(merged, mlen + clen + 2);
                    if (nm) {
                        merged = nm; memcpy(merged + mlen, part, clen);
                        mlen += clen; merged[mlen++] = '\n'; merged[mlen] = 0;
                    }
                }
                free(part);
                while (p < end && *p != 'R' && *p != ']') p++;
                if (p < end && *p == 'R') p++;
            } else p++;
        }
        *out_len = mlen;
        return merged;
    }
    return NULL;
}

typedef struct { float a, b, c, d, e, f; } Mat2D;
static Mat2D mat_ident(void) { Mat2D m = {1, 0, 0, 1, 0, 0}; return m; }
static Mat2D mat_mul(Mat2D m1, Mat2D m2) {
    Mat2D r;
    r.a = m1.a * m2.a + m1.b * m2.c;
    r.b = m1.a * m2.b + m1.b * m2.d;
    r.c = m1.c * m2.a + m1.d * m2.c;
    r.d = m1.c * m2.b + m1.d * m2.d;
    r.e = m1.e * m2.a + m1.f * m2.c + m2.e;
    r.f = m1.e * m2.b + m1.f * m2.d + m2.f;
    return r;
}
static void mat_pt(Mat2D m, float x, float y, float *ox, float *oy) {
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

typedef struct { Mat2D ctm; uint32_t fill_rgb; uint32_t stroke_rgb; } GState;

typedef struct {
    PdfDoc *doc;
    float media_x0, media_y0, media_w, media_h;
    int page_sx, page_sy, page_pw, page_ph;
    float scale;
    int vx, vy, vw, vh;
    int night_invert;
    uint32_t paper_rgb;

    char  *text_out;
    size_t text_len;
    size_t text_cap;

    GState gstack[16];
    int gtop;
    int images_drawn;
    int text_runs_drawn;
    int rects_drawn;
    char img_err[64];
} PdfRenderCtx;

static void pdf_to_screen(const PdfRenderCtx *rc, float px, float py, int *sx, int *sy) {
    *sx = rc->page_sx + (int)((px - rc->media_x0) * rc->scale);
    *sy = rc->page_sy + (int)((rc->media_h - (py - rc->media_y0)) * rc->scale);
}

static uint32_t maybe_invert(uint32_t rgb, int inv) {
    if (!inv) return rgb;
    return ((255u - ((rgb >> 16) & 0xFFu)) << 16) | ((255u - ((rgb >> 8) & 0xFFu)) << 8) | (255u - (rgb & 0xFFu));
}

static uint32_t rgb_from_floats(float r, float g, float b) {
    int ri = (int)(r * 255.0f), gi = (int)(g * 255.0f), bi = (int)(b * 255.0f);
    if (ri < 0) ri = 0;
    if (ri > 255) ri = 255;
    if (gi < 0) gi = 0;
    if (gi > 255) gi = 255;
    if (bi < 0) bi = 0;
    if (bi > 255) bi = 255;
    return ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
}

static uint32_t rgb_from_cmyk(float c, float m, float y, float k) {
    return rgb_from_floats((1.0f - c) * (1.0f - k), (1.0f - m) * (1.0f - k), (1.0f - y) * (1.0f - k));
}

static void append_utf8_buf(char *dst, size_t cap, size_t *len, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFFu) return;
    if (cp < 0x80u) {
        if (*len + 1 < cap) dst[(*len)++] = (char)cp;
    } else if (cp < 0x800u) {
        if (*len + 2 < cap) { dst[(*len)++] = (char)(0xC0u | (cp >> 6)); dst[(*len)++] = (char)(0x80u | (cp & 0x3Fu)); }
    } else if (cp < 0x10000u) {
        if (*len + 3 < cap) {
            dst[(*len)++] = (char)(0xE0u | (cp >> 12));
            dst[(*len)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[(*len)++] = (char)(0x80u | (cp & 0x3Fu));
        }
    } else {
        if (*len + 4 < cap) {
            dst[(*len)++] = (char)(0xF0u | (cp >> 18));
            dst[(*len)++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            dst[(*len)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[(*len)++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    dst[*len] = '\0';
}

static void decode_pdf_bytes_into(const uint8_t *raw, size_t rlen, const PdfCMap *cm, const PdfCMap *fallback_cm,
                                  char *out, size_t cap, size_t *olen) {
    if (rlen == 0) return;
    if (rlen >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        for (size_t k = 2; k + 1 < rlen; k += 2)
            append_utf8_buf(out, cap, olen, (uint32_t)((raw[k] << 8) | raw[k + 1]));
        return;
    }

    const PdfCMap *use_cm = (cm && cm->count > 0) ? cm : fallback_cm;
    if (use_cm && use_cm->count > 0 && (rlen % 2) == 0) {
        int hits = 0;
        for (size_t k = 0; k + 1 < rlen; k += 2) {
            uint16_t code = (uint16_t)((raw[k] << 8) | raw[k + 1]);
            if (cmap_get(use_cm, code) != 0) hits++;
        }
        if (hits > 0) {
            for (size_t k = 0; k + 1 < rlen; k += 2) {
                uint16_t code = (uint16_t)((raw[k] << 8) | raw[k + 1]);
                uint16_t u16 = cmap_get(use_cm, code);
                if (u16 == 0 && fallback_cm && fallback_cm != use_cm) u16 = cmap_get(fallback_cm, code);
                if (u16 > 0) append_utf8_buf(out, cap, olen, u16);
                else if (code >= 0x20 && code < 0x7F) append_utf8_buf(out, cap, olen, code);
            }
            return;
        }
    }

    for (size_t k = 0; k < rlen; k++) {
        uint16_t u16 = use_cm ? cmap_get(use_cm, raw[k]) : 0;
        if (u16 > 0) append_utf8_buf(out, cap, olen, u16);
        else if (raw[k] >= 0x20 && raw[k] < 0x7F) append_utf8_buf(out, cap, olen, raw[k]);
        else if ((raw[k] & 0xE0u) == 0xE0u && k + 2 < rlen &&
                 (raw[k + 1] & 0xC0u) == 0x80u && (raw[k + 2] & 0xC0u) == 0x80u) {
            if (*olen + 3 < cap) {
                out[(*olen)++] = (char)raw[k];
                out[(*olen)++] = (char)raw[k + 1];
                out[(*olen)++] = (char)raw[k + 2];
                out[*olen] = '\0';
            }
            k += 2;
        } else if (raw[k] == '\t') append_utf8_buf(out, cap, olen, ' ');
    }
}

static void decode_hex_into(const char *s, size_t slen, const PdfCMap *cm, const PdfCMap *fb,
                            char *out, size_t cap, size_t *olen) {
    uint8_t bytes[512]; size_t blen = 0; int hi = -1;
    for (size_t i = 0; i < slen && blen < sizeof(bytes); i++) {
        int hv = hex_val(s[i]);
        if (hv < 0) continue;
        if (hi < 0) hi = hv;
        else { bytes[blen++] = (uint8_t)((hi << 4) | hv); hi = -1; }
    }
    if (hi >= 0 && blen < sizeof(bytes)) bytes[blen++] = (uint8_t)(hi << 4);
    decode_pdf_bytes_into(bytes, blen, cm, fb, out, cap, olen);
}

static size_t decode_paren_into(const char *s, size_t rem, const PdfCMap *cm, const PdfCMap *fb,
                                char *out, size_t cap, size_t *olen) {
    size_t i = 1; int depth = 1;
    uint8_t raw[512]; size_t rlen = 0;
    while (i < rem && depth > 0) {
        char c = s[i++];
        if (c == '\\' && i < rem) {
            char nx = s[i++];
            if (nx == '\r') { if (i < rem && s[i] == '\n') i++; continue; }
            if (nx == '\n') continue;
            uint8_t oc = (uint8_t)nx;
            if (nx == 'n') oc = '\n';
            else if (nx == 'r') oc = '\r';
            else if (nx == 't') oc = '\t';
            else if (nx == 'b') oc = '\b';
            else if (nx == 'f') oc = '\f';
            else if (nx >= '0' && nx <= '7') {
                int oct = nx - '0';
                if (i < rem && s[i] >= '0' && s[i] <= '7') oct = oct * 8 + (s[i++] - '0');
                if (i < rem && s[i] >= '0' && s[i] <= '7') oct = oct * 8 + (s[i++] - '0');
                oc = (uint8_t)oct;
            }
            if (rlen < sizeof(raw)) raw[rlen++] = oc;
        } else if (c == '(') { depth++; if (rlen < sizeof(raw)) raw[rlen++] = '('; }
        else if (c == ')') { depth--; if (depth > 0 && rlen < sizeof(raw)) raw[rlen++] = ')'; }
        else { if (rlen < sizeof(raw)) raw[rlen++] = (uint8_t)c; }
    }
    decode_pdf_bytes_into(raw, rlen, cm, fb, out, cap, olen);
    return i;
}

static void text_out_append(PdfRenderCtx *rc, const char *s, size_t n) {
    if (!rc->text_out || n == 0) return;
    if (rc->text_len + n + 2 > rc->text_cap) {
        size_t ncap = rc->text_cap ? rc->text_cap * 2 : 4096;
        while (ncap < rc->text_len + n + 2) ncap *= 2;
        if (ncap > 1024 * 1024) return;
        char *nb = (char *)realloc(rc->text_out, ncap);
        if (!nb) return;
        rc->text_out = nb; rc->text_cap = ncap;
    }
    memcpy(rc->text_out + rc->text_len, s, n);
    rc->text_len += n; rc->text_out[rc->text_len] = '\0';
}

static void draw_clipped_rect(const PdfRenderCtx *rc, int x, int y, int w, int h, uint32_t col, int filled) {
    int cx0 = rc->vx > rc->page_sx ? rc->vx : rc->page_sx;
    int cy0 = rc->vy > rc->page_sy ? rc->vy : rc->page_sy;
    int cx1 = (rc->vx + rc->vw) < (rc->page_sx + rc->page_pw) ? (rc->vx + rc->vw) : (rc->page_sx + rc->page_pw);
    int cy1 = (rc->vy + rc->vh) < (rc->page_sy + rc->page_ph) ? (rc->vy + rc->vh) : (rc->page_sy + rc->page_ph);
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (filled) {
        int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
        int x1 = (x + w) > cx1 ? cx1 : (x + w), y1 = (y + h) > cy1 ? cy1 : (y + h);
        if (x1 > x0 && y1 > y0) hal_fill_rect(x0, y0, x1 - x0, y1 - y0, col);
    } else {
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        draw_clipped_rect(rc, x, y, w, 1, col, 1);
        draw_clipped_rect(rc, x, y + h - 1, w, 1, col, 1);
        draw_clipped_rect(rc, x, y, 1, h, col, 1);
        draw_clipped_rect(rc, x + w - 1, y, 1, h, col, 1);
    }
}

static int color_diff(uint32_t a, uint32_t b) {
    return abs((int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF))
         + abs((int)((a >> 8) & 0xFF)  - (int)((b >> 8) & 0xFF))
         + abs((int)(a & 0xFF)         - (int)(b & 0xFF));
}

static void execute_pdf_stream(PdfRenderCtx *rc, const char *s, size_t len,
                               const ResMap *xobjs, int xobj_cnt,
                               const ResMap *fonts, int font_cnt, int form_depth) {
    if (!s || len == 0 || form_depth > 4) return;

    float nums[16]; int ncnt = 0;
    char last_name[32] = {0};
    int has_rect = 0; float rx0 = 0, ry0 = 0, rw = 0, rh = 0;
    int has_line = 0; float lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;

    int in_bt = 0;
    Mat2D tm = mat_ident(), tlm = mat_ident();
    float t_leading = 14.0f, font_size = 12.0f;
    const PdfCMap *cur_cmap = &rc->doc->global_cmap;

    char pending_str[1024]; size_t pending_len = 0;
    pending_str[0] = '\0';

    size_t i = 0;
    while (i < len) {
        while (i < len && (isspace((unsigned char)s[i]) || s[i] == '%')) {
            if (s[i] == '%') { while (i < len && s[i] != '\n' && s[i] != '\r') i++; }
            else i++;
        }
        if (i >= len) break;
        char c = s[i];

        if (c == '(') {
            pending_len = 0; pending_str[0] = '\0';
            i += decode_paren_into(s + i, len - i, cur_cmap, &rc->doc->global_cmap, pending_str, sizeof(pending_str), &pending_len);
            continue;
        }
        if (c == '<') {
            if (i + 1 < len && s[i + 1] == '<') {
                i += 2; int d = 1;
                while (i + 1 < len && d > 0) {
                    if (s[i] == '<' && s[i + 1] == '<') { d++; i += 2; }
                    else if (s[i] == '>' && s[i + 1] == '>') { d--; i += 2; }
                    else i++;
                }
            } else {
                size_t j = i + 1;
                while (j < len && s[j] != '>') j++;
                pending_len = 0; pending_str[0] = '\0';
                decode_hex_into(s + i + 1, j - (i + 1), cur_cmap, &rc->doc->global_cmap, pending_str, sizeof(pending_str), &pending_len);
                i = (j < len) ? (j + 1) : len;
            }
            continue;
        }
        if (c == '[') {
            pending_len = 0; pending_str[0] = '\0';
            i++;
            while (i < len && s[i] != ']') {
                if (s[i] == '(') {
                    i += decode_paren_into(s + i, len - i, cur_cmap, &rc->doc->global_cmap, pending_str, sizeof(pending_str), &pending_len);
                } else if (s[i] == '<' && (i + 1 >= len || s[i + 1] != '<')) {
                    size_t j = i + 1;
                    while (j < len && s[j] != '>') j++;
                    decode_hex_into(s + i + 1, j - (i + 1), cur_cmap, &rc->doc->global_cmap, pending_str, sizeof(pending_str), &pending_len);
                    i = (j < len) ? (j + 1) : len;
                } else if (s[i] == '-' || isdigit((unsigned char)s[i])) {
                    long kern = strtol(s + i, NULL, 10);
                    if (kern <= -180 && pending_len > 0 && pending_str[pending_len - 1] != ' ')
                        append_utf8_buf(pending_str, sizeof(pending_str), &pending_len, ' ');
                    while (i < len && (s[i] == '-' || s[i] == '.' || isdigit((unsigned char)s[i]))) i++;
                } else i++;
            }
            if (i < len && s[i] == ']') i++;
            continue;
        }
        if (c == '/') {
            i++; size_t ni = 0;
            while (i < len && ni + 1 < sizeof(last_name) &&
                   !isspace((unsigned char)s[i]) && s[i] != '/' && s[i] != '[' && s[i] != ']' && s[i] != '(' && s[i] != '<')
                last_name[ni++] = s[i++];
            last_name[ni] = '\0';
            continue;
        }
        if (c == '-' || c == '+' || c == '.' || isdigit((unsigned char)c)) {
            char *ep = NULL;
            float v = strtof(s + i, &ep);
            if (ep && ep > s + i) {
                if (ncnt < 16) nums[ncnt++] = v;
                i = (size_t)(ep - s);
                continue;
            }
        }

        char op[24]; size_t oi = 0;
        while (i < len && oi + 1 < sizeof(op) &&
               !isspace((unsigned char)s[i]) && s[i] != '/' && s[i] != '[' && s[i] != ']' && s[i] != '(' && s[i] != '<')
            op[oi++] = s[i++];
        op[oi] = '\0';
        if (oi == 0) { i++; continue; }

        GState *gs = &rc->gstack[rc->gtop];

        if (strcmp(op, "q") == 0) {
            if (rc->gtop + 1 < 16) { rc->gstack[rc->gtop + 1] = *gs; rc->gtop++; }
        } else if (strcmp(op, "Q") == 0) {
            if (rc->gtop > 0) rc->gtop--;
        } else if (strcmp(op, "cm") == 0 && ncnt >= 6) {
            Mat2D m = { nums[ncnt-6], nums[ncnt-5], nums[ncnt-4], nums[ncnt-3], nums[ncnt-2], nums[ncnt-1] };
            gs->ctm = mat_mul(m, gs->ctm);
        } else if (strcmp(op, "rg") == 0 && ncnt >= 3) {
            gs->fill_rgb = rgb_from_floats(nums[ncnt-3], nums[ncnt-2], nums[ncnt-1]);
        } else if (strcmp(op, "RG") == 0 && ncnt >= 3) {
            gs->stroke_rgb = rgb_from_floats(nums[ncnt-3], nums[ncnt-2], nums[ncnt-1]);
        } else if (strcmp(op, "g") == 0 && ncnt >= 1) {
            gs->fill_rgb = rgb_from_floats(nums[ncnt-1], nums[ncnt-1], nums[ncnt-1]);
        } else if (strcmp(op, "G") == 0 && ncnt >= 1) {
            gs->stroke_rgb = rgb_from_floats(nums[ncnt-1], nums[ncnt-1], nums[ncnt-1]);
        } else if (strcmp(op, "k") == 0 && ncnt >= 4) {
            gs->fill_rgb = rgb_from_cmyk(nums[ncnt-4], nums[ncnt-3], nums[ncnt-2], nums[ncnt-1]);
        } else if (strcmp(op, "K") == 0 && ncnt >= 4) {
            gs->stroke_rgb = rgb_from_cmyk(nums[ncnt-4], nums[ncnt-3], nums[ncnt-2], nums[ncnt-1]);
        } else if (strcmp(op, "sc") == 0 || strcmp(op, "scn") == 0) {
            if (ncnt == 1) gs->fill_rgb = rgb_from_floats(nums[0], nums[0], nums[0]);
            else if (ncnt == 3) gs->fill_rgb = rgb_from_floats(nums[0], nums[1], nums[2]);
            else if (ncnt >= 4) gs->fill_rgb = rgb_from_cmyk(nums[ncnt-4], nums[ncnt-3], nums[ncnt-2], nums[ncnt-1]);
        } else if (strcmp(op, "SC") == 0 || strcmp(op, "SCN") == 0) {
            if (ncnt == 1) gs->stroke_rgb = rgb_from_floats(nums[0], nums[0], nums[0]);
            else if (ncnt == 3) gs->stroke_rgb = rgb_from_floats(nums[0], nums[1], nums[2]);
            else if (ncnt >= 4) gs->stroke_rgb = rgb_from_cmyk(nums[ncnt-4], nums[ncnt-3], nums[ncnt-2], nums[ncnt-1]);
        } else if (strcmp(op, "re") == 0 && ncnt >= 4) {
            rx0 = nums[ncnt-4]; ry0 = nums[ncnt-3]; rw = nums[ncnt-2]; rh = nums[ncnt-1];
            has_rect = 1;
        } else if (strcmp(op, "m") == 0 && ncnt >= 2) {
            lx0 = nums[ncnt-2]; ly0 = nums[ncnt-1]; has_line = 1;
        } else if (strcmp(op, "l") == 0 && ncnt >= 2) {
            lx1 = nums[ncnt-2]; ly1 = nums[ncnt-1];
            if (has_line == 1) has_line = 2;
        } else if (strcmp(op, "f") == 0 || strcmp(op, "F") == 0 || strcmp(op, "f*") == 0 ||
                   strcmp(op, "S") == 0 || strcmp(op, "s") == 0 || strcmp(op, "B") == 0 || strcmp(op, "b") == 0) {
            if (!rc->text_out) {
                int do_fill = (op[0] == 'f' || op[0] == 'F' || op[0] == 'B' || op[0] == 'b');
                int do_stroke = (op[0] == 'S' || op[0] == 's' || op[0] == 'B' || op[0] == 'b');
                if (has_rect) {
                    float px0, py0, px1, py1;
                    mat_pt(gs->ctm, rx0, ry0, &px0, &py0);
                    mat_pt(gs->ctm, rx0 + rw, ry0 + rh, &px1, &py1);
                    int sx0, sy0, sx1, sy1;
                    pdf_to_screen(rc, px0, py0, &sx0, &sy0);
                    pdf_to_screen(rc, px1, py1, &sx1, &sy1);
                    uint32_t fc = maybe_invert(gs->fill_rgb, rc->night_invert);
                    if (do_fill) {
                        draw_clipped_rect(rc, sx0, sy0, sx1 - sx0, sy1 - sy0, fc, 1);
                        if (color_diff(fc, rc->paper_rgb) > 25) rc->rects_drawn++;
                    }
                    if (do_stroke) {
                        draw_clipped_rect(rc, sx0, sy0, sx1 - sx0, sy1 - sy0, maybe_invert(gs->stroke_rgb, rc->night_invert), 0);
                        rc->rects_drawn++;
                    }
                }
                if (has_line == 2 && do_stroke) {
                    float px0, py0, px1, py1;
                    mat_pt(gs->ctm, lx0, ly0, &px0, &py0);
                    mat_pt(gs->ctm, lx1, ly1, &px1, &py1);
                    int sx0, sy0, sx1, sy1;
                    pdf_to_screen(rc, px0, py0, &sx0, &sy0);
                    pdf_to_screen(rc, px1, py1, &sx1, &sy1);
                    uint32_t sc = maybe_invert(gs->stroke_rgb, rc->night_invert);
                    if (abs(sy1 - sy0) <= 2) draw_clipped_rect(rc, sx0 < sx1 ? sx0 : sx1, sy0, abs(sx1 - sx0) + 1, 1, sc, 1);
                    else if (abs(sx1 - sx0) <= 2) draw_clipped_rect(rc, sx0, sy0 < sy1 ? sy0 : sy1, 1, abs(sy1 - sy0) + 1, sc, 1);
                    else if (sx0 >= rc->vx && sx0 < rc->vx + rc->vw && sy0 >= rc->vy && sy0 < rc->vy + rc->vh &&
                             sx1 >= rc->vx && sx1 < rc->vx + rc->vw && sy1 >= rc->vy && sy1 < rc->vy + rc->vh)
                        hal_draw_line(sx0, sy0, sx1, sy1, sc);
                    rc->rects_drawn++;
                }
            }
            has_rect = 0; has_line = 0;
        } else if (strcmp(op, "n") == 0) {
            has_rect = 0; has_line = 0;
        } else if (strcmp(op, "BI") == 0) {
            const char *ei = find_mem(s + i, len - i, "EI");
            if (ei) i = (size_t)(ei - s) + 2;
        } else if (strcmp(op, "Do") == 0 && last_name[0]) {
            int xobj_num = 0;
            for (int k = 0; k < xobj_cnt; k++) {
                if (strcmp(xobjs[k].name, last_name) == 0) { xobj_num = xobjs[k].obj_num; break; }
            }
            if (xobj_num > 0) {
                const PdfObjRec *xo = find_obj(rc->doc, xobj_num);
                if (xo) {
                    const char *xb = (const char *)(xo->base + xo->body_off);
                    const char *xst = find_stream_kw(xb, xo->body_len, NULL);
                    size_t xdl = xst ? (size_t)(xst - xb) : xo->body_len;
                    if (find_mem(xb, xdl, "/Form")) {
                        size_t flen = 0; int dct = 0;
                        uint8_t *fd = extract_obj_stream(rc->doc, xo, &flen, &dct);
                        if (fd && !dct) {
                            ResMap sub_x[MAX_XOBJS]; int sub_xc = 0;
                            ResMap sub_f[MAX_PAGE_FONTS]; int sub_fc = 0;
                            collect_resources(rc->doc, xo, sub_x, &sub_xc, sub_f, &sub_fc);
                            const ResMap *ux = (sub_xc > 0) ? sub_x : xobjs;
                            int uxc = (sub_xc > 0) ? sub_xc : xobj_cnt;
                            const ResMap *uf = (sub_fc > 0) ? sub_f : fonts;
                            int ufc = (sub_fc > 0) ? sub_fc : font_cnt;
                            execute_pdf_stream(rc, (const char *)fd, flen, ux, uxc, uf, ufc, form_depth + 1);
                        }
                        free(fd);
                    } else if (!rc->text_out && find_mem(xb, xdl, "/Image")) {
                        int iw = 0, ih = 0;
                        uint8_t *rgb = decode_pdf_image_obj(rc->doc, xobj_num, &iw, &ih, rc->img_err, sizeof(rc->img_err));
                        if (rgb && iw > 0 && ih > 0) {
                            float p0x, p0y, p1x, p1y;
                            mat_pt(gs->ctm, 0.0f, 1.0f, &p0x, &p0y);
                            mat_pt(gs->ctm, 1.0f, 0.0f, &p1x, &p1y);
                            int sx0, sy0, sx1, sy1;
                            pdf_to_screen(rc, p0x, p0y, &sx0, &sy0);
                            pdf_to_screen(rc, p1x, p1y, &sx1, &sy1);
                            if (sx1 < sx0) { int t = sx0; sx0 = sx1; sx1 = t; }
                            if (sy1 < sy0) { int t = sy0; sy0 = sy1; sy1 = t; }
                            int dw = sx1 - sx0, dh = sy1 - sy0;
                            if (dw >= 2 && dh >= 2) {
                                int cx0 = rc->vx > rc->page_sx ? rc->vx : rc->page_sx;
                                int cy0 = rc->vy > rc->page_sy ? rc->vy : rc->page_sy;
                                int cx1 = (rc->vx + rc->vw) < (rc->page_sx + rc->page_pw) ? (rc->vx + rc->vw) : (rc->page_sx + rc->page_pw);
                                int cy1 = (rc->vy + rc->vh) < (rc->page_sy + rc->page_ph) ? (rc->vy + rc->vh) : (rc->page_sy + rc->page_ph);
                                hal_blit_rgb888_scaled(rgb, iw, ih, sx0, sy0, dw, dh, cx0, cy0, cx1 - cx0, cy1 - cy0, rc->night_invert);
                                rc->images_drawn++;
                            }
                        }
                    }
                }
            }
        } else if (strcmp(op, "BT") == 0) {
            in_bt = 1; tm = mat_ident(); tlm = mat_ident();
        } else if (strcmp(op, "ET") == 0) {
            in_bt = 0;
            if (rc->text_out && rc->text_len > 0 && rc->text_out[rc->text_len - 1] != '\n')
                text_out_append(rc, "\n", 1);
        } else if (in_bt && strcmp(op, "Tf") == 0 && ncnt >= 1) {
            font_size = f_abs(nums[ncnt - 1]);
            if (font_size < 6.0f) font_size = 12.0f;
            cur_cmap = &rc->doc->global_cmap;
            if (last_name[0]) {
                for (int k = 0; k < font_cnt; k++) {
                    if (strcmp(fonts[k].name, last_name) == 0) {
                        cur_cmap = find_font_cmap(rc->doc, fonts[k].obj_num);
                        break;
                    }
                }
            }
        } else if (in_bt && strcmp(op, "TL") == 0 && ncnt >= 1) {
            t_leading = nums[ncnt - 1];
        } else if (in_bt && strcmp(op, "Tm") == 0 && ncnt >= 6) {
            Mat2D m = { nums[ncnt-6], nums[ncnt-5], nums[ncnt-4], nums[ncnt-3], nums[ncnt-2], nums[ncnt-1] };
            tm = m; tlm = m;
        } else if (in_bt && (strcmp(op, "Td") == 0 || strcmp(op, "TD") == 0) && ncnt >= 2) {
            float tx = nums[ncnt - 2], ty = nums[ncnt - 1];
            if (op[1] == 'D') t_leading = -ty;
            Mat2D tr = { 1, 0, 0, 1, tx, ty };
            tlm = mat_mul(tr, tlm); tm = tlm;
            if (rc->text_out && f_abs(ty) > 1.0f && rc->text_len > 0 && rc->text_out[rc->text_len - 1] != '\n')
                text_out_append(rc, "\n", 1);
        } else if (in_bt && strcmp(op, "T*") == 0) {
            Mat2D tr = { 1, 0, 0, 1, 0, -t_leading };
            tlm = mat_mul(tr, tlm); tm = tlm;
            if (rc->text_out && rc->text_len > 0 && rc->text_out[rc->text_len - 1] != '\n')
                text_out_append(rc, "\n", 1);
        } else if (in_bt && (strcmp(op, "Tj") == 0 || strcmp(op, "TJ") == 0 || strcmp(op, "'") == 0 || strcmp(op, "\"") == 0)) {
            if (op[0] == '\'' || op[0] == '"') {
                Mat2D tr = { 1, 0, 0, 1, 0, -t_leading };
                tlm = mat_mul(tr, tlm); tm = tlm;
            }
            if (pending_len > 0) {
                if (rc->text_out) {
                    text_out_append(rc, pending_str, pending_len);
                } else {
                    Mat2D full = mat_mul(tm, gs->ctm);
                    int sx, sy;
                    pdf_to_screen(rc, full.e, full.f, &sx, &sy);
                    sy -= (int)(font_size * rc->scale * 0.85f);
                    if (sy < rc->page_sy + 2) sy = rc->page_sy + 2;

                    int cx0 = rc->vx > rc->page_sx ? rc->vx : rc->page_sx;
                    int cy0 = rc->vy > rc->page_sy ? rc->vy : rc->page_sy;
                    int cx1 = (rc->vx + rc->vw) < (rc->page_sx + rc->page_pw) ? (rc->vx + rc->vw) : (rc->page_sx + rc->page_pw);
                    int cy1 = (rc->vy + rc->vh) < (rc->page_sy + rc->page_ph) ? (rc->vy + rc->vh) : (rc->page_sy + rc->page_ph);

                    if (sy >= cy0 - 2 && sy + 14 <= cy1 && sx < cx1 - 8 && sx + 420 > cx0) {
                        int draw_x = sx < cx0 ? cx0 : sx;
                        int max_w = cx1 - draw_x - 4;
                        if (max_w > 12) {
                            uint32_t tcol = maybe_invert(gs->fill_rgb, rc->night_invert);
                            if (color_diff(tcol, rc->paper_rgb) < 48)
                                tcol = rc->night_invert ? 0xE2E8F0 : 0x1A1A1A;
                            hal_set_text_bg(rc->paper_rgb);
                            hal_draw_line_utf8(draw_x, sy, pending_str, (int)pending_len, max_w, tcol);
                            rc->text_runs_drawn++;
                        }
                    }
                    tm.e += (float)pending_len * font_size * 0.45f;
                }
                pending_len = 0; pending_str[0] = '\0';
            }
        }
        ncnt = 0;
    }
}

void pdf_render_page(PdfDoc *doc, int page_idx,
                     int vx, int vy, int vw, int vh,
                     int zoom_mode, int pan_x, int pan_y,
                     uint32_t backdrop_rgb, int night_invert,
                     int *out_page_w, int *out_page_h) {
    if (out_page_w) *out_page_w = vw;
    if (out_page_h) *out_page_h = vh;
    if (!doc || page_idx < 0 || page_idx >= doc->page_count) return;

    const PdfPageRec *pr = &doc->pages[page_idx];
    const PdfObjRec *po = find_obj(doc, pr->obj_num);
    if (!po) return;

    float fit_h = (float)(vh - 16) / pr->media_h;
    float fit_w = (float)(vw - 24) / pr->media_w;
    float fit_page = (fit_h < fit_w) ? fit_h : fit_w;
    float scale = fit_page;
    if (zoom_mode == 1) scale = fit_w;
    else if (zoom_mode == 2) scale = fit_w * 1.35f;
    else if (zoom_mode == 3) scale = fit_w * 1.75f;
    else if (zoom_mode == 4) scale = fit_w * 2.25f;
    if (scale < 0.15f) scale = 0.15f;

    int pw = (int)(pr->media_w * scale);
    int ph = (int)(pr->media_h * scale);
    if (out_page_w) *out_page_w = pw;
    if (out_page_h) *out_page_h = ph;

    int px = (pw <= vw) ? (vx + (vw - pw) / 2) : (vx - pan_x);
    int py = (ph <= vh) ? (vy + (vh - ph) / 2) : (vy - pan_y);

    hal_fill_rect(vx, vy, vw, vh, backdrop_rgb);
    uint32_t paper  = night_invert ? 0x1A1E26 : 0xFFFFFF;
    uint32_t shadow = night_invert ? 0x090B0E : 0xB8B2A6;
    uint32_t border = night_invert ? 0x333C4A : 0xC9C2B6;

    int sx0 = px < vx ? vx : px, sy0 = py < vy ? vy : py;
    int sx1 = (px + pw) > (vx + vw) ? (vx + vw) : (px + pw);
    int sy1 = (py + ph) > (vy + vh) ? (vy + vh) : (py + ph);

    if (sx1 > sx0 && sy1 > sy0) {
        int shx1 = (sx1 + 5 > vx + vw) ? (vx + vw) : (sx1 + 5);
        int shy1 = (sy1 + 5 > vy + vh) ? (vy + vh) : (sy1 + 5);
        hal_fill_rect(sx0 + 4, sy0 + 4, shx1 - (sx0 + 4), shy1 - (sy0 + 4), shadow);
        hal_fill_rect(sx0, sy0, sx1 - sx0, sy1 - sy0, paper);
        hal_draw_rect(sx0, sy0, sx1 - sx0, sy1 - sy0, border);
    }

    ResMap xobjs[MAX_XOBJS]; int xcnt = 0;
    ResMap fonts[MAX_PAGE_FONTS]; int fcnt = 0;
    collect_resources(doc, po, xobjs, &xcnt, fonts, &fcnt);

    PdfRenderCtx rc;
    memset(&rc, 0, sizeof(rc));
    rc.doc = doc;
    rc.media_x0 = pr->media_x0; rc.media_y0 = pr->media_y0;
    rc.media_w = pr->media_w;   rc.media_h = pr->media_h;
    rc.page_sx = px; rc.page_sy = py; rc.page_pw = pw; rc.page_ph = ph;
    rc.scale = scale; rc.vx = vx; rc.vy = vy; rc.vw = vw; rc.vh = vh;
    rc.night_invert = night_invert; rc.paper_rgb = paper;
    rc.gstack[0].ctm = mat_ident();
    rc.gstack[0].fill_rgb = 0x111111;
    rc.gstack[0].stroke_rgb = 0x111111;

    size_t clen = 0;
    uint8_t *content = get_page_content_stream(doc, po, &clen);
    if (content && clen > 0) {
        execute_pdf_stream(&rc, (const char *)content, clen, xobjs, xcnt, fonts, fcnt, 0);
    }
    free(content);

    if (rc.images_drawn == 0 && rc.text_runs_drawn == 0 && xcnt > 0) {
        for (int k = 0; k < xcnt; k++) {
            int iw = 0, ih = 0;
            uint8_t *rgb = decode_pdf_image_obj(doc, xobjs[k].obj_num, &iw, &ih, rc.img_err, sizeof(rc.img_err));
            if (rgb && iw > 0 && ih > 0) {
                hal_blit_rgb888_scaled(rgb, iw, ih, px, py, pw, ph, sx0, sy0, sx1 - sx0, sy1 - sy0, night_invert);
                rc.images_drawn++;
                break;
            }
        }
    }

    if (rc.images_drawn == 0 && rc.text_runs_drawn == 0 && rc.rects_drawn == 0) {
        uint32_t fg = night_invert ? 0xE2E8F0 : 0x334155;
        uint32_t acc = 0xEF4444;
        int dx = sx0 + 18, dy = sy0 + 28, dw = (sx1 - sx0) - 36;
        if (dw < 180) { dx = vx + 40; dw = vw - 80; }
        hal_set_text_bg(paper);
        if (doc->is_encrypted) {
            const char *m1 = "【此 PDF 設有加密保護 (/Encrypt)】";
            const char *m2 = "加密 PDF 需先在電腦解除密碼保護後再開啟。";
            hal_draw_line_utf8(dx, dy, m1, (int)strlen(m1), dw, acc);
            hal_draw_line_utf8(dx, dy + 32, m2, (int)strlen(m2), dw, fg);
        } else if (rc.img_err[0]) {
            char m1[128];
            snprintf(m1, sizeof(m1), "【本頁圖片格式暫不支援：%s】", rc.img_err);
            const char *m2 = "提示：本閱讀器已原生支援 .png / .bmp 圖片書！";
            const char *m3 = "可把特殊掃描 PDF 轉成 001.png、002.png 直接翻閱。";
            hal_draw_line_utf8(dx, dy, m1, (int)strlen(m1), dw, acc);
            hal_draw_line_utf8(dx, dy + 32, m2, (int)strlen(m2), dw, fg);
            hal_draw_line_utf8(dx, dy + 60, m3, (int)strlen(m3), dw, fg);
        } else {
            char diag[160];
            snprintf(diag, sizeof(diag), "本頁無可解碼內容 (Obj=%d, Stm=%zuB, XObj=%d, CMap=%d)",
                     doc->obj_count, clen, xcnt, doc->global_cmap.count);
            const char *m2 = "若為向量筆畫字型或特殊編碼 PDF，建議按 [R] 試重排或轉 PNG。";
            hal_draw_line_utf8(dx, dy, diag, (int)strlen(diag), dw, acc);
            hal_draw_line_utf8(dx, dy + 32, m2, (int)strlen(m2), dw, fg);
        }
    }
}

char *pdf_extract_page_text(PdfDoc *doc, int page_idx, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!doc || page_idx < 0 || page_idx >= doc->page_count) return NULL;
    const PdfObjRec *po = find_obj(doc, doc->pages[page_idx].obj_num);
    if (!po) return NULL;

    ResMap xobjs[MAX_XOBJS]; int xcnt = 0;
    ResMap fonts[MAX_PAGE_FONTS]; int fcnt = 0;
    collect_resources(doc, po, xobjs, &xcnt, fonts, &fcnt);

    PdfRenderCtx rc;
    memset(&rc, 0, sizeof(rc));
    rc.doc = doc;
    rc.text_cap = 2048;
    rc.text_out = (char *)calloc(1, rc.text_cap);
    if (!rc.text_out) return NULL;
    rc.gstack[0].ctm = mat_ident();

    size_t clen = 0;
    uint8_t *content = get_page_content_stream(doc, po, &clen);
    if (content && clen > 0) {
        execute_pdf_stream(&rc, (const char *)content, clen, xobjs, xcnt, fonts, fcnt, 0);
    }
    free(content);
    if (out_len) *out_len = rc.text_len;
    return rc.text_out;
}
