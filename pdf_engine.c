#include "pdf_engine.h"
#include "hal.h"
#include "inflate_tiny.h"
#include "jpeg_tiny.h"
#include <ctype.h>
static inline float f_abs(float v) { return v < 0.0f ? -v : v; }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PDF_MAX_FILE_BYTES  (24u * 1024u * 1024u)
#define PDF_MAX_STREAM      (4u * 1024u * 1024u)
#define PDF_MAX_OBJS        8192
#define PDF_MAX_PAGES       2048
#define PDF_CMAP_HASH       4096
#define PDF_IMG_CACHE_SLOTS 4

typedef struct {
    int obj_num;
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
    PdfCMapEntry entries[PDF_CMAP_HASH];
    int count;
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

    PdfObjRec *objs;
    int        obj_count;

    PdfPageRec *pages;
    int         page_count;

    PdfCMap    cmap;
    char       title[96];
    char       author[64];

    PdfImgCache img_cache[PDF_IMG_CACHE_SLOTS];
    unsigned    cache_clock;
};

/* --- 基礎字串與解析輔助 --- */
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

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void cmap_put(PdfCMap *cm, uint16_t src, uint16_t dst) {
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
    if (cm->count == 0) return 0;
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
    uint32_t v = 0;
    int d = 0;
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
                if (dd <= 4 && dc > 0) cmap_put(cm, (uint16_t)sc, (uint16_t)dc);
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
                while (cur < stop && isspace((unsigned char)*cur)) cur++;
                if (cur < stop && *cur == '[') {
                    cur++;
                    for (uint32_t c = s0; c <= s1 && c < s0 + 256 && cur < stop; c++) {
                        while (cur < stop && isspace((unsigned char)*cur)) cur++;
                        if (cur >= stop || *cur == ']') break;
                        uint32_t dc = 0; int dd = 0;
                        if (parse_hex_tok(&cur, stop, &dc, &dd) && dd <= 4 && dc > 0)
                            cmap_put(cm, (uint16_t)c, (uint16_t)dc);
                    }
                    while (cur < stop && *cur != ']') cur++;
                    if (cur < stop) cur++;
                } else {
                    uint32_t db = 0; int dd = 0;
                    if (parse_hex_tok(&cur, stop, &db, &dd) && dd <= 4) {
                        for (uint32_t c = s0; c <= s1 && c < s0 + 512; c++)
                            cmap_put(cm, (uint16_t)c, (uint16_t)(db + (c - s0)));
                    }
                }
            }
            p = stop + 10;
        }
    }
}

static const PdfObjRec *find_obj(const PdfDoc *doc, int obj_num) {
    /* 二分搜尋或線性搜尋 (obj_num 通常遞增，先快掃) */
    for (int i = 0; i < doc->obj_count; i++) {
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

static int dict_get_int(const char *dict, size_t dlen, const char *key, int *out_val) {
    const char *p = find_mem(dict, dlen, key);
    if (!p) return 0;
    p += strlen(key);
    const char *end = dict + dlen;
    while (p < end && isspace((unsigned char)*p)) p++;
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
    const char *p = find_mem(dict, dlen, "/MediaBox");
    if (!p) p = find_mem(dict, dlen, "/CropBox");
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
    *w = vals[2] - vals[0];
    *h = vals[3] - vals[1];
    if (*w < 50.0f) *w = 595.0f;
    if (*h < 50.0f) *h = 842.0f;
    return 1;
}

/* 抽取物件中的 stream 並自動解壓 FlateDecode */
static uint8_t *extract_obj_stream(const PdfDoc *doc, const PdfObjRec *o, size_t *out_len, int *out_is_dct) {
    if (out_len) *out_len = 0;
    if (out_is_dct) *out_is_dct = 0;
    if (!o) return NULL;
    const char *body = (const char *)(doc->data + o->body_off);
    size_t blen = o->body_len;

    const char *st = find_mem(body, blen, "stream");
    if (!st) return NULL;
    size_t dict_len = (size_t)(st - body);

    size_t doff = dict_len + 6;
    if (doff < blen && body[doff] == '\r') doff++;
    if (doff < blen && body[doff] == '\n') doff++;
    else return NULL;

    const char *en = find_mem(body + doff, blen - doff, "endstream");
    if (!en) return NULL;
    size_t dlen = (size_t)(en - (body + doff));
    while (dlen > 0 && (body[doff + dlen - 1] == '\n' || body[doff + dlen - 1] == '\r')) dlen--;

    int is_dct = (find_mem(body, dict_len, "/DCTDecode") != NULL);
    int is_flate = (find_mem(body, dict_len, "/FlateDecode") != NULL);
    if (out_is_dct) *out_is_dct = is_dct;

    if (is_dct) {
        /* 保持原始 JPEG 位元流交給 tiny_jpeg_decode */
        uint8_t *buf = (uint8_t *)malloc(dlen + 1);
        if (!buf) return NULL;
        memcpy(buf, body + doff, dlen);
        buf[dlen] = 0;
        if (out_len) *out_len = dlen;
        return buf;
    }
    if (is_flate) {
        return tiny_inflate_zlib_alloc((const uint8_t *)(body + doff), dlen, PDF_MAX_STREAM, out_len);
    }

    uint8_t *buf = (uint8_t *)malloc(dlen + 1);
    if (!buf) return NULL;
    memcpy(buf, body + doff, dlen);
    buf[dlen] = 0;
    if (out_len) *out_len = dlen;
    return buf;
}

/* 遞迴走訪 /Pages -> /Kids 建立正確順序的頁面表 */
static void collect_pages_tree(PdfDoc *doc, int obj_num, float inh_x0, float inh_y0, float inh_w, float inh_h, int depth) {
    if (depth > 16 || doc->page_count >= PDF_MAX_PAGES) return;
    const PdfObjRec *o = find_obj(doc, obj_num);
    if (!o) return;

    const char *body = (const char *)(doc->data + o->body_off);
    size_t blen = o->body_len;
    const char *st = find_mem(body, blen, "stream");
    size_t dlen = st ? (size_t)(st - body) : blen;

    float x0 = inh_x0, y0 = inh_y0, w = inh_w, h = inh_h;
    dict_get_mediabox(body, dlen, &x0, &y0, &w, &h);

    const char *kids = find_mem(body, dlen, "/Kids");
    if (kids) {
        const char *p = kids + 5;
        const char *end = body + dlen;
        while (p < end && *p != '[') p++;
        if (p < end) {
            p++;
            while (p < end && *p != ']') {
                int child = 0;
                if (parse_ref_after(p, end, &child)) {
                    collect_pages_tree(doc, child, x0, y0, w, h, depth + 1);
                    while (p < end && *p != 'R' && *p != ']') p++;
                    if (p < end && *p == 'R') p++;
                } else {
                    p++;
                }
            }
        }
        return;
    }

    /* 葉節點：加入頁面列表 */
    PdfPageRec *pr = &doc->pages[doc->page_count++];
    pr->obj_num = obj_num;
    pr->media_x0 = x0; pr->media_y0 = y0;
    pr->media_w = w;   pr->media_h = h;
}

static int is_type_page_only(const char *dict, size_t dlen) {
    const char *p = find_mem(dict, dlen, "/Type");
    while (p) {
        const char *q = p + 5;
        const char *end = dict + dlen;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (q + 5 <= end && memcmp(q, "/Page", 5) == 0) {
            if (q + 5 == end || (!isalpha((unsigned char)q[5]))) return 1;
        }
        p = find_mem(q, (size_t)(end - q), "/Type");
    }
    return 0;
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

    if (got < 8 || memcmp(data, "%PDF-", 5) != 0) { free(data); return NULL; }

    PdfDoc *doc = (PdfDoc *)calloc(1, sizeof(PdfDoc));
    if (!doc) { free(data); return NULL; }
    doc->data = data;
    doc->len = got;
    doc->objs = (PdfObjRec *)calloc(PDF_MAX_OBJS, sizeof(PdfObjRec));
    doc->pages = (PdfPageRec *)calloc(PDF_MAX_PAGES, sizeof(PdfPageRec));
    if (!doc->objs || !doc->pages) { pdf_close(doc); return NULL; }

    /* 1. 掃描所有 "N G obj ... endobj" 建立物件索引表 */
    const char *s = (const char *)data;
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

                    doc->objs[doc->obj_count].obj_num = (int)onum;
                    doc->objs[doc->obj_count].body_off = boff;
                    doc->objs[doc->obj_count].body_len = blen;
                    doc->obj_count++;

                    pos = boff + blen + 6;
                    continue;
                }
            }
        }
        pos++;
    }

    /* 2. 優先依 /Root -> /Pages -> /Kids 樹狀建立正確順序頁面表 */
    int root_obj = 0, pages_obj = 0;
    const char *root_kw = find_mem(s, got, "/Root");
    if (root_kw) parse_ref_after(root_kw + 5, s + got, &root_obj);
    if (root_obj > 0) {
        const PdfObjRec *ro = find_obj(doc, root_obj);
        if (ro) dict_get_ref(s + ro->body_off, ro->body_len, "/Pages", &pages_obj);
    }
    if (pages_obj > 0) {
        collect_pages_tree(doc, pages_obj, 0, 0, 595.0f, 842.0f, 0);
    }

    /* 若未找到樹狀 /Pages，退回直接掃描所有 /Type /Page 物件 */
    if (doc->page_count == 0) {
        for (int i = 0; i < doc->obj_count && doc->page_count < PDF_MAX_PAGES; i++) {
            const char *b = s + doc->objs[i].body_off;
            size_t bl = doc->objs[i].body_len;
            const char *st = find_mem(b, bl, "stream");
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

    /* 3. 快速收集所有 /ToUnicode CMap (通常僅 1~4 個小串流，瞬間完成) */
    for (int i = 0; i < doc->obj_count; i++) {
        const char *b = s + doc->objs[i].body_off;
        size_t bl = doc->objs[i].body_len;
        const char *st = find_mem(b, bl, "stream");
        size_t dl = st ? (size_t)(st - b) : bl;
        if (find_mem(b, dl, "/ToUnicode")) {
            int cm_obj = 0;
            if (dict_get_ref(b, dl, "/ToUnicode", &cm_obj)) {
                const PdfObjRec *co = find_obj(doc, cm_obj);
                if (co) {
                    size_t clen = 0; int dct = 0;
                    uint8_t *cd = extract_obj_stream(doc, co, &clen, &dct);
                    if (cd && !dct) parse_tounicode_cmap((const char *)cd, clen, &doc->cmap);
                    free(cd);
                }
            }
        } else if (st && !find_mem(b, dl, "/Image") && !find_mem(b, dl, "/DCTDecode") && bl < 120000) {
            /* 有些 PDF 直接把 CMap 放在獨立物件未從 /ToUnicode 索引 */
            size_t clen = 0; int dct = 0;
            uint8_t *cd = extract_obj_stream(doc, &doc->objs[i], &clen, &dct);
            if (cd && !dct && (find_ci((const char *)cd, clen, "beginbfchar") || find_ci((const char *)cd, clen, "beginbfrange"))) {
                parse_tounicode_cmap((const char *)cd, clen, &doc->cmap);
            }
            free(cd);
        }
    }

    if (doc->page_count == 0) {
        pdf_close(doc);
        return NULL;
    }
    return doc;
}

void pdf_close(PdfDoc *doc) {
    if (!doc) return;
    for (int i = 0; i < PDF_IMG_CACHE_SLOTS; i++) free(doc->img_cache[i].rgb);
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

/* ============================================================
 * 內嵌圖片解碼與 LRU 快取 (JPEG / Flate RGB / Gray / CMYK)
 * ============================================================ */
static int paeth_pred(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static uint8_t *decode_pdf_image_obj(PdfDoc *doc, int obj_num, int *out_w, int *out_h) {
    *out_w = 0; *out_h = 0;
    /* 1. 先查 LRU 快取，平移與縮放時 0ms 瞬間出圖！ */
    for (int i = 0; i < PDF_IMG_CACHE_SLOTS; i++) {
        if (doc->img_cache[i].rgb && doc->img_cache[i].obj_num == obj_num) {
            doc->img_cache[i].stamp = ++doc->cache_clock;
            *out_w = doc->img_cache[i].w;
            *out_h = doc->img_cache[i].h;
            return doc->img_cache[i].rgb;
        }
    }

    const PdfObjRec *o = find_obj(doc, obj_num);
    if (!o) return NULL;
    const char *body = (const char *)(doc->data + o->body_off);
    size_t blen = o->body_len;
    const char *st = find_mem(body, blen, "stream");
    if (!st) return NULL;
    size_t dlen = (size_t)(st - body);

    if (!find_mem(body, dlen, "/Image")) return NULL;

    int iw = 0, ih = 0, bpc = 8, predictor = 1;
    dict_get_int(body, dlen, "/Width", &iw);
    dict_get_int(body, dlen, "/Height", &ih);
    dict_get_int(body, dlen, "/BitsPerComponent", &bpc);
    dict_get_int(body, dlen, "/Predictor", &predictor);

    size_t slen = 0;
    int is_dct = 0;
    uint8_t *raw = extract_obj_stream(doc, o, &slen, &is_dct);
    if (!raw || slen == 0) { free(raw); return NULL; }

    uint8_t *rgb = NULL;
    if (is_dct) {
        rgb = tiny_jpeg_decode(raw, slen, &iw, &ih);
        free(raw);
    } else {
        if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096 || bpc != 8) {
            free(raw);
            return NULL;
        }
        int comps = 3;
        if (find_mem(body, dlen, "/DeviceGray")) comps = 1;
        else if (find_mem(body, dlen, "/DeviceCMYK")) comps = 4;
        else if (find_mem(body, dlen, "/DeviceRGB")) comps = 3;

        /* 處理 PNG Predictor (10..15) */
        size_t stride = (size_t)iw * (size_t)comps;
        uint8_t *pix = raw;
        uint8_t *unfilt = NULL;
        if (predictor >= 10 && slen >= (size_t)ih * (stride + 1)) {
            unfilt = (uint8_t *)calloc((size_t)ih, stride);
            if (unfilt) {
                for (int y = 0; y < ih; y++) {
                    const uint8_t *src = raw + (size_t)y * (stride + 1);
                    uint8_t ftype = src[0];
                    src++;
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
                pix = unfilt;
                slen = (size_t)ih * stride;
            }
        }

        if (slen >= (size_t)ih * stride) {
            rgb = (uint8_t *)malloc((size_t)iw * (size_t)ih * 3u);
            if (rgb) {
                size_t total = (size_t)iw * (size_t)ih;
                if (comps == 3) {
                    memcpy(rgb, pix, total * 3u);
                } else if (comps == 1) {
                    for (size_t k = 0; k < total; k++) {
                        rgb[k * 3] = rgb[k * 3 + 1] = rgb[k * 3 + 2] = pix[k];
                    }
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
        free(unfilt);
        free(raw);
    }

    if (!rgb) return NULL;

    /* 存入 LRU 快取 */
    int slot = 0;
    for (int i = 0; i < PDF_IMG_CACHE_SLOTS; i++) {
        if (!doc->img_cache[i].rgb) { slot = i; break; }
        if (doc->img_cache[i].stamp < doc->img_cache[slot].stamp) slot = i;
    }
    free(doc->img_cache[slot].rgb);
    doc->img_cache[slot].obj_num = obj_num;
    doc->img_cache[slot].w = iw;
    doc->img_cache[slot].h = ih;
    doc->img_cache[slot].rgb = rgb;
    doc->img_cache[slot].stamp = ++doc->cache_clock;

    *out_w = iw; *out_h = ih;
    return rgb;
}

/* --- 收集本頁 /Resources /XObject 映射表 --- */
#define MAX_XOBJS 48
typedef struct {
    char name[32];
    int  obj_num;
} XObjMap;

static void parse_xobject_dict(const PdfDoc *doc, const char *res, size_t rlen, XObjMap *maps, int *cnt) {
    const char *xo = find_mem(res, rlen, "/XObject");
    if (!xo) return;
    const char *p = xo + 8;
    const char *end = res + rlen;
    while (p < end && isspace((unsigned char)*p)) p++;

    /* 支援間接 /XObject 15 0 R 或內嵌 /XObject << ... >> */
    int ref_obj = 0;
    if (parse_ref_after(p, end, &ref_obj)) {
        const PdfObjRec *o = find_obj(doc, ref_obj);
        if (!o) return;
        p = (const char *)(doc->data + o->body_off);
        end = p + o->body_len;
    }

    const char *op = find_mem(p, (size_t)(end - p), "<<");
    if (!op) return;
    p = op + 2;
    int depth = 1;
    while (p < end && depth > 0 && *cnt < MAX_XOBJS) {
        if (p + 1 < end && p[0] == '<' && p[1] == '<') { depth++; p += 2; continue; }
        if (p + 1 < end && p[0] == '>' && p[1] == '>') { depth--; p += 2; continue; }
        if (depth == 1 && *p == '/') {
            p++;
            char nm[32]; size_t ni = 0;
            while (p < end && ni + 1 < sizeof(nm) && !isspace((unsigned char)*p) && *p != '/' && *p != '<' && *p != '>') {
                nm[ni++] = *p++;
            }
            nm[ni] = '\0';
            int tobj = 0;
            if (nm[0] && parse_ref_after(p, end, &tobj)) {
                snprintf(maps[*cnt].name, sizeof(maps[*cnt].name), "%s", nm);
                maps[*cnt].obj_num = tobj;
                (*cnt)++;
            }
        } else {
            p++;
        }
    }
}

static void collect_page_xobjects(const PdfDoc *doc, const PdfObjRec *page_obj, XObjMap *maps, int *cnt) {
    *cnt = 0;
    const char *body = (const char *)(doc->data + page_obj->body_off);
    size_t blen = page_obj->body_len;
    const char *st = find_mem(body, blen, "stream");
    size_t dlen = st ? (size_t)(st - body) : blen;

    const char *res = find_mem(body, dlen, "/Resources");
    if (res) {
        int res_obj = 0;
        if (dict_get_ref(res, (size_t)(body + dlen - res), "/Resources", &res_obj)) {
            const PdfObjRec *ro = find_obj(doc, res_obj);
            if (ro) parse_xobject_dict(doc, (const char *)(doc->data + ro->body_off), ro->body_len, maps, cnt);
        } else {
            parse_xobject_dict(doc, res, (size_t)(body + dlen - res), maps, cnt);
        }
    }
    /* 若該頁繼承父 /Pages 的 /Resources */
    if (*cnt == 0) {
        int parent_obj = 0;
        if (dict_get_ref(body, dlen, "/Parent", &parent_obj)) {
            const PdfObjRec *po = find_obj(doc, parent_obj);
            if (po) {
                const char *pb = (const char *)(doc->data + po->body_off);
                parse_xobject_dict(doc, pb, po->body_len, maps, cnt);
            }
        }
    }
}

/* 合併取得本頁完整的解壓後內容串流 */
static uint8_t *get_page_content_stream(const PdfDoc *doc, const PdfObjRec *page_obj, size_t *out_len) {
    *out_len = 0;
    const char *body = (const char *)(doc->data + page_obj->body_off);
    size_t blen = page_obj->body_len;
    const char *st = find_mem(body, blen, "stream");
    size_t dlen = st ? (size_t)(st - body) : blen;

    const char *cptr = find_mem(body, dlen, "/Contents");
    if (!cptr) return NULL;
    const char *p = cptr + 9;
    const char *end = body + dlen;
    while (p < end && isspace((unsigned char)*p)) p++;

    int single_obj = 0;
    if (parse_ref_after(p, end, &single_obj)) {
        const PdfObjRec *co = find_obj(doc, single_obj);
        int dct = 0;
        return extract_obj_stream(doc, co, out_len, &dct);
    }

    if (p < end && *p == '[') {
        p++;
        uint8_t *merged = NULL;
        size_t mlen = 0;
        while (p < end && *p != ']') {
            int co_num = 0;
            if (parse_ref_after(p, end, &co_num)) {
                const PdfObjRec *co = find_obj(doc, co_num);
                size_t clen = 0; int dct = 0;
                uint8_t *part = extract_obj_stream(doc, co, &clen, &dct);
                if (part && clen > 0 && mlen + clen + 2 < PDF_MAX_STREAM) {
                    uint8_t *nm = (uint8_t *)realloc(merged, mlen + clen + 2);
                    if (nm) {
                        merged = nm;
                        memcpy(merged + mlen, part, clen);
                        mlen += clen;
                        merged[mlen++] = '\n';
                        merged[mlen] = 0;
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

/* ============================================================
 * 真實 PDF 2D 轉換矩陣與內容串流渲染直譯器
 * ============================================================ */
typedef struct {
    float a, b, c, d, e, f;
} Mat2D;

static Mat2D mat_ident(void) { Mat2D m = {1, 0, 0, 1, 0, 0}; return m; }
static Mat2D mat_mul(Mat2D m1, Mat2D m2) {
    /* 先套用 m1 再套用 m2 (PDF cm: CTM' = M * CTM) */
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

typedef struct {
    Mat2D ctm;
    uint32_t fill_rgb;
    uint32_t stroke_rgb;
} GState;

typedef struct {
    PdfDoc *doc;
    const XObjMap *xobjs;
    int xobj_cnt;

    /* 頁面座標 -> 螢幕座標轉換參數 */
    float media_x0, media_y0, media_w, media_h;
    int page_sx, page_sy, page_pw, page_ph;
    float scale;
    int vx, vy, vw, vh;
    int night_invert;
    uint32_t paper_rgb;

    /* 若 text_out != NULL 則只做文字抽取，不繪圖 */
    char  *text_out;
    size_t text_len;
    size_t text_cap;

    GState gstack[16];
    int gtop;
    int images_drawn;
    int text_runs_drawn;
} PdfRenderCtx;

static void pdf_to_screen(const PdfRenderCtx *rc, float px, float py, int *sx, int *sy) {
    *sx = rc->page_sx + (int)((px - rc->media_x0) * rc->scale);
    *sy = rc->page_sy + (int)((rc->media_h - (py - rc->media_y0)) * rc->scale);
}

static uint32_t maybe_invert(uint32_t rgb, int inv) {
    if (!inv) return rgb;
    uint32_t r = 255u - ((rgb >> 16) & 0xFFu);
    uint32_t g = 255u - ((rgb >> 8) & 0xFFu);
    uint32_t b = 255u - (rgb & 0xFFu);
    return (r << 16) | (g << 8) | b;
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

static void append_utf8_buf(char *dst, size_t cap, size_t *len, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFFu) return;
    if (cp < 0x80u) {
        if (*len + 1 < cap) dst[(*len)++] = (char)cp;
    } else if (cp < 0x800u) {
        if (*len + 2 < cap) {
            dst[(*len)++] = (char)(0xC0u | (cp >> 6));
            dst[(*len)++] = (char)(0x80u | (cp & 0x3Fu));
        }
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

static void decode_hex_into(const char *s, size_t slen, const PdfCMap *cm, char *out, size_t cap, size_t *olen) {
    uint8_t bytes[512]; size_t blen = 0;
    int hi = -1;
    for (size_t i = 0; i < slen && blen < sizeof(bytes); i++) {
        int hv = hex_val(s[i]);
        if (hv < 0) continue;
        if (hi < 0) hi = hv;
        else { bytes[blen++] = (uint8_t)((hi << 4) | hv); hi = -1; }
    }
    if (hi >= 0 && blen < sizeof(bytes)) bytes[blen++] = (uint8_t)(hi << 4);
    if (blen == 0) return;

    if (blen >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        for (size_t i = 2; i + 1 < blen; i += 2)
            append_utf8_buf(out, cap, olen, (uint32_t)((bytes[i] << 8) | bytes[i + 1]));
        return;
    }
    if (cm->count > 0 && (blen % 2) == 0) {
        int hits = 0;
        for (size_t i = 0; i + 1 < blen; i += 2)
            if (cmap_get(cm, (uint16_t)((bytes[i] << 8) | bytes[i + 1])) != 0) hits++;
        if (hits > 0) {
            for (size_t i = 0; i + 1 < blen; i += 2) {
                uint16_t code = (uint16_t)((bytes[i] << 8) | bytes[i + 1]);
                uint16_t u16 = cmap_get(cm, code);
                if (u16 > 0) append_utf8_buf(out, cap, olen, u16);
                else if (code >= 0x20 && code < 0x7F) append_utf8_buf(out, cap, olen, code);
            }
            return;
        }
    }
    for (size_t i = 0; i < blen; i++) {
        uint16_t u16 = (cm->count > 0) ? cmap_get(cm, bytes[i]) : 0;
        if (u16 > 0) append_utf8_buf(out, cap, olen, u16);
        else if (bytes[i] >= 0x20 && bytes[i] < 0x7F) append_utf8_buf(out, cap, olen, bytes[i]);
    }
}

static size_t decode_paren_into(const char *s, size_t rem, const PdfCMap *cm, char *out, size_t cap, size_t *olen) {
    size_t i = 1;
    int depth = 1;
    uint8_t raw[512]; size_t rlen = 0;
    while (i < rem && depth > 0) {
        char c = s[i++];
        if (c == '\\' && i < rem) {
            char nx = s[i++];
            uint8_t oc = (uint8_t)nx;
            if (nx == 'n') oc = '\n';
            else if (nx == 'r') oc = '\r';
            else if (nx == 't') oc = '\t';
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
    if (rlen >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        for (size_t k = 2; k + 1 < rlen; k += 2)
            append_utf8_buf(out, cap, olen, (uint32_t)((raw[k] << 8) | raw[k + 1]));
    } else {
        for (size_t k = 0; k < rlen; k++) {
            uint16_t u16 = (cm->count > 0) ? cmap_get(cm, raw[k]) : 0;
            if (u16 > 0) append_utf8_buf(out, cap, olen, u16);
            else if (raw[k] >= 0x20 && raw[k] < 0x7F) append_utf8_buf(out, cap, olen, raw[k]);
            else if (raw[k] == '\t') append_utf8_buf(out, cap, olen, ' ');
        }
    }
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
        rc->text_out = nb;
        rc->text_cap = ncap;
    }
    memcpy(rc->text_out + rc->text_len, s, n);
    rc->text_len += n;
    rc->text_out[rc->text_len] = '\0';
}

/* 裁切後的實心矩形與線條 (不超出頁框與視窗) */
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

static void execute_pdf_stream(PdfRenderCtx *rc, const char *s, size_t len, int form_depth) {
    if (!s || len == 0 || form_depth > 2) return;

    float nums[16];
    int ncnt = 0;
    char last_name[32] = {0};

    /* 簡易路徑狀態 (支援 re, m, l) */
    int has_rect = 0;
    float rx0 = 0, ry0 = 0, rw = 0, rh = 0;
    int has_line = 0;
    float lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;

    /* 文字狀態 */
    int in_bt = 0;
    Mat2D tm = mat_ident();
    Mat2D tlm = mat_ident();
    float t_leading = 14.0f;
    float font_size = 12.0f;
    char pending_str[1024];
    size_t pending_len = 0;
    pending_str[0] = '\0';

    size_t i = 0;
    while (i < len) {
        /* 跳過空白與註解 */
        while (i < len && (isspace((unsigned char)s[i]) || s[i] == '%')) {
            if (s[i] == '%') { while (i < len && s[i] != '\n' && s[i] != '\r') i++; }
            else i++;
        }
        if (i >= len) break;

        char c = s[i];

        /* 1. 字串 (...) */
        if (c == '(') {
            pending_len = 0; pending_str[0] = '\0';
            i += decode_paren_into(s + i, len - i, &rc->doc->cmap, pending_str, sizeof(pending_str), &pending_len);
            continue;
        }
        /* 2. 十六進位字串 <...> (排除字典 <<) */
        if (c == '<') {
            if (i + 1 < len && s[i + 1] == '<') {
                i += 2;
                int d = 1;
                while (i + 1 < len && d > 0) {
                    if (s[i] == '<' && s[i + 1] == '<') { d++; i += 2; }
                    else if (s[i] == '>' && s[i + 1] == '>') { d--; i += 2; }
                    else i++;
                }
            } else {
                size_t j = i + 1;
                while (j < len && s[j] != '>') j++;
                pending_len = 0; pending_str[0] = '\0';
                decode_hex_into(s + i + 1, j - (i + 1), &rc->doc->cmap, pending_str, sizeof(pending_str), &pending_len);
                i = (j < len) ? (j + 1) : len;
            }
            continue;
        }
        /* 3. 陣列 [...] (主要為 TJ 文字陣列) */
        if (c == '[') {
            pending_len = 0; pending_str[0] = '\0';
            i++;
            while (i < len && s[i] != ']') {
                if (s[i] == '(') {
                    i += decode_paren_into(s + i, len - i, &rc->doc->cmap, pending_str, sizeof(pending_str), &pending_len);
                } else if (s[i] == '<' && (i + 1 >= len || s[i + 1] != '<')) {
                    size_t j = i + 1;
                    while (j < len && s[j] != '>') j++;
                    decode_hex_into(s + i + 1, j - (i + 1), &rc->doc->cmap, pending_str, sizeof(pending_str), &pending_len);
                    i = (j < len) ? (j + 1) : len;
                } else if (s[i] == '-' || isdigit((unsigned char)s[i])) {
                    long kern = strtol(s + i, NULL, 10);
                    if (kern <= -180 && pending_len > 0 && pending_str[pending_len - 1] != ' ') {
                        append_utf8_buf(pending_str, sizeof(pending_str), &pending_len, ' ');
                    }
                    while (i < len && (s[i] == '-' || s[i] == '.' || isdigit((unsigned char)s[i]))) i++;
                } else i++;
            }
            if (i < len && s[i] == ']') i++;
            continue;
        }
        /* 4. 名稱 /Name */
        if (c == '/') {
            i++;
            size_t ni = 0;
            while (i < len && ni + 1 < sizeof(last_name) &&
                   !isspace((unsigned char)s[i]) && s[i] != '/' && s[i] != '[' && s[i] != ']' && s[i] != '(' && s[i] != '<') {
                last_name[ni++] = s[i++];
            }
            last_name[ni] = '\0';
            continue;
        }
        /* 5. 數字 */
        if (c == '-' || c == '+' || c == '.' || isdigit((unsigned char)c)) {
            char *ep = NULL;
            float v = strtof(s + i, &ep);
            if (ep && ep > s + i) {
                if (ncnt < 16) nums[ncnt++] = v;
                i = (size_t)(ep - s);
                continue;
            }
        }
        /* 6. 操作元 (Operator) */
        char op[24]; size_t oi = 0;
        while (i < len && oi + 1 < sizeof(op) &&
               !isspace((unsigned char)s[i]) && s[i] != '/' && s[i] != '[' && s[i] != ']' && s[i] != '(' && s[i] != '<') {
            op[oi++] = s[i++];
        }
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
        } else if (strcmp(op, "re") == 0 && ncnt >= 4) {
            rx0 = nums[ncnt-4]; ry0 = nums[ncnt-3]; rw = nums[ncnt-2]; rh = nums[ncnt-1];
            has_rect = 1;
        } else if (strcmp(op, "m") == 0 && ncnt >= 2) {
            lx0 = nums[ncnt-2]; ly0 = nums[ncnt-1];
            has_line = 1;
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
                    if (do_fill) draw_clipped_rect(rc, sx0, sy0, sx1 - sx0, sy1 - sy0, maybe_invert(gs->fill_rgb, rc->night_invert), 1);
                    if (do_stroke) draw_clipped_rect(rc, sx0, sy0, sx1 - sx0, sy1 - sy0, maybe_invert(gs->stroke_rgb, rc->night_invert), 0);
                }
                if (has_line == 2 && do_stroke) {
                    float px0, py0, px1, py1;
                    mat_pt(gs->ctm, lx0, ly0, &px0, &py0);
                    mat_pt(gs->ctm, lx1, ly1, &px1, &py1);
                    int sx0, sy0, sx1, sy1;
                    pdf_to_screen(rc, px0, py0, &sx0, &sy0);
                    pdf_to_screen(rc, px1, py1, &sx1, &sy1);
                    /* 簡易水平/垂直線段裁切繪製 */
                    if (abs(sy1 - sy0) <= 2) {
                        draw_clipped_rect(rc, sx0 < sx1 ? sx0 : sx1, sy0, abs(sx1 - sx0) + 1, 1, maybe_invert(gs->stroke_rgb, rc->night_invert), 1);
                    } else if (abs(sx1 - sx0) <= 2) {
                        draw_clipped_rect(rc, sx0, sy0 < sy1 ? sy0 : sy1, 1, abs(sy1 - sy0) + 1, maybe_invert(gs->stroke_rgb, rc->night_invert), 1);
                    } else if (sx0 >= rc->vx && sx0 < rc->vx + rc->vw && sy0 >= rc->vy && sy0 < rc->vy + rc->vh &&
                               sx1 >= rc->vx && sx1 < rc->vx + rc->vw && sy1 >= rc->vy && sy1 < rc->vy + rc->vh) {
                        hal_draw_line(sx0, sy0, sx1, sy1, maybe_invert(gs->stroke_rgb, rc->night_invert));
                    }
                }
            }
            has_rect = 0; has_line = 0;
        } else if (strcmp(op, "n") == 0) {
            has_rect = 0; has_line = 0;
        } else if (strcmp(op, "Do") == 0 && last_name[0]) {
            /* 渲染內嵌圖片或 Form XObject! */
            int xobj_num = 0;
            for (int k = 0; k < rc->xobj_cnt; k++) {
                if (strcmp(rc->xobjs[k].name, last_name) == 0) { xobj_num = rc->xobjs[k].obj_num; break; }
            }
            if (xobj_num > 0) {
                const PdfObjRec *xo = find_obj(rc->doc, xobj_num);
                if (xo) {
                    const char *xb = (const char *)(rc->doc->data + xo->body_off);
                    const char *xst = find_mem(xb, xo->body_len, "stream");
                    size_t xdl = xst ? (size_t)(xst - xb) : xo->body_len;
                    if (find_mem(xb, xdl, "/Form")) {
                        size_t flen = 0; int dct = 0;
                        uint8_t *fd = extract_obj_stream(rc->doc, xo, &flen, &dct);
                        if (fd && !dct) execute_pdf_stream(rc, (const char *)fd, flen, form_depth + 1);
                        free(fd);
                    } else if (!rc->text_out && find_mem(xb, xdl, "/Image")) {
                        int iw = 0, ih = 0;
                        uint8_t *rgb = decode_pdf_image_obj(rc->doc, xobj_num, &iw, &ih);
                        if (rgb && iw > 0 && ih > 0) {
                            /* PDF 圖片預設畫在單位正方形 [0,0]..[1,1]，經 CTM 轉換成頁面上真實矩形 */
                            float p0x, p0y, p1x, p1y;
                            mat_pt(gs->ctm, 0.0f, 1.0f, &p0x, &p0y); /* 左上 */
                            mat_pt(gs->ctm, 1.0f, 0.0f, &p1x, &p1y); /* 右下 */
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
            in_bt = 1;
            tm = mat_ident();
            tlm = mat_ident();
        } else if (strcmp(op, "ET") == 0) {
            in_bt = 0;
            if (rc->text_out && rc->text_len > 0 && rc->text_out[rc->text_len - 1] != '\n') {
                text_out_append(rc, "\n", 1);
            }
        } else if (in_bt && strcmp(op, "Tf") == 0 && ncnt >= 1) {
            font_size = f_abs(nums[ncnt - 1]);
            if (font_size < 6.0f) font_size = 12.0f;
        } else if (in_bt && strcmp(op, "TL") == 0 && ncnt >= 1) {
            t_leading = nums[ncnt - 1];
        } else if (in_bt && strcmp(op, "Tm") == 0 && ncnt >= 6) {
            Mat2D m = { nums[ncnt-6], nums[ncnt-5], nums[ncnt-4], nums[ncnt-3], nums[ncnt-2], nums[ncnt-1] };
            tm = m; tlm = m;
        } else if (in_bt && (strcmp(op, "Td") == 0 || strcmp(op, "TD") == 0) && ncnt >= 2) {
            float tx = nums[ncnt - 2], ty = nums[ncnt - 1];
            if (op[1] == 'D') t_leading = -ty;
            Mat2D tr = { 1, 0, 0, 1, tx, ty };
            tlm = mat_mul(tr, tlm);
            tm = tlm;
            if (rc->text_out && f_abs(ty) > 1.0f && rc->text_len > 0 && rc->text_out[rc->text_len - 1] != '\n') {
                text_out_append(rc, "\n", 1);
            }
        } else if (in_bt && strcmp(op, "T*") == 0) {
            Mat2D tr = { 1, 0, 0, 1, 0, -t_leading };
            tlm = mat_mul(tr, tlm);
            tm = tlm;
            if (rc->text_out && rc->text_len > 0 && rc->text_out[rc->text_len - 1] != '\n') {
                text_out_append(rc, "\n", 1);
            }
        } else if (in_bt && (strcmp(op, "Tj") == 0 || strcmp(op, "TJ") == 0 || strcmp(op, "'") == 0 || strcmp(op, "\"") == 0)) {
            if (op[0] == '\'' || op[0] == '"') {
                Mat2D tr = { 1, 0, 0, 1, 0, -t_leading };
                tlm = mat_mul(tr, tlm);
                tm = tlm;
            }
            if (pending_len > 0) {
                if (rc->text_out) {
                    text_out_append(rc, pending_str, pending_len);
                } else {
                    /* 在 PDF 頁面真實 (x, y) 座標上繪製定位文字！ */
                    Mat2D full = mat_mul(tm, gs->ctm);
                    int sx, sy;
                    pdf_to_screen(rc, full.e, full.f, &sx, &sy);
                    /* PDF 文字基線在下方，稍微往上移字高對齊 */
                    sy -= (int)(font_size * rc->scale * 0.85f);
                    if (sy < rc->page_sy + 2) sy = rc->page_sy + 2;

                    int cx0 = rc->vx > rc->page_sx ? rc->vx : rc->page_sx;
                    int cy0 = rc->vy > rc->page_sy ? rc->vy : rc->page_sy;
                    int cx1 = (rc->vx + rc->vw) < (rc->page_sx + rc->page_pw) ? (rc->vx + rc->vw) : (rc->page_sx + rc->page_pw);
                    int cy1 = (rc->vy + rc->vh) < (rc->page_sy + rc->page_ph) ? (rc->vy + rc->vh) : (rc->page_sy + rc->page_ph);

                    if (sy >= cy0 - 2 && sy + 14 <= cy1 && sx < cx1 - 8 && sx + 400 > cx0) {
                        int draw_x = sx < cx0 ? cx0 : sx;
                        int max_w = cx1 - draw_x - 4;
                        if (max_w > 12) {
                            uint32_t tcol = maybe_invert(gs->fill_rgb, rc->night_invert);
                            /* 避免淺背景淺字看不見 */
                            if (tcol == rc->paper_rgb) tcol = rc->night_invert ? 0xE2E8F0 : 0x1A1A1A;
                            hal_set_text_bg(rc->paper_rgb);
                            hal_draw_line_utf8(draw_x, sy, pending_str, (int)pending_len, max_w, tcol);
                            rc->text_runs_drawn++;
                        }
                    }
                    /* 推進文字游標粗略寬度，避免同列多個連續 Tj 重疊 */
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

    /* 1. 計算縮放比例 */
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

    /* 2. 繪製閱讀背景與真實立體紙張陰影 */
    hal_fill_rect(vx, vy, vw, vh, backdrop_rgb);

    uint32_t paper = night_invert ? 0x1A1E26 : 0xFFFFFF;
    uint32_t shadow = night_invert ? 0x090B0E : 0xB8B2A6;
    uint32_t border = night_invert ? 0x333C4A : 0xC9C2B6;

    /* 裁切紙張繪製範圍在視窗內 */
    int sx0 = px < vx ? vx : px;
    int sy0 = py < vy ? vy : py;
    int sx1 = (px + pw) > (vx + vw) ? (vx + vw) : (px + pw);
    int sy1 = (py + ph) > (vy + vh) ? (vy + vh) : (py + ph);

    if (sx1 > sx0 && sy1 > sy0) {
        int shx1 = (sx1 + 5 > vx + vw) ? (vx + vw) : (sx1 + 5);
        int shy1 = (sy1 + 5 > vy + vh) ? (vy + vh) : (sy1 + 5);
        hal_fill_rect(sx0 + 4, sy0 + 4, shx1 - (sx0 + 4), shy1 - (sy0 + 4), shadow);
        hal_fill_rect(sx0, sy0, sx1 - sx0, sy1 - sy0, paper);
        hal_draw_rect(sx0, sy0, sx1 - sx0, sy1 - sy0, border);
    }

    /* 3. 解析本頁資源與內容串流並渲染 */
    XObjMap xobjs[MAX_XOBJS];
    int xcnt = 0;
    collect_page_xobjects(doc, po, xobjs, &xcnt);

    PdfRenderCtx rc;
    memset(&rc, 0, sizeof(rc));
    rc.doc = doc;
    rc.xobjs = xobjs;
    rc.xobj_cnt = xcnt;
    rc.media_x0 = pr->media_x0; rc.media_y0 = pr->media_y0;
    rc.media_w = pr->media_w;   rc.media_h = pr->media_h;
    rc.page_sx = px; rc.page_sy = py;
    rc.page_pw = pw; rc.page_ph = ph;
    rc.scale = scale;
    rc.vx = vx; rc.vy = vy; rc.vw = vw; rc.vh = vh;
    rc.night_invert = night_invert;
    rc.paper_rgb = paper;
    rc.gstack[0].ctm = mat_ident();
    rc.gstack[0].fill_rgb = 0x111111;
    rc.gstack[0].stroke_rgb = 0x111111;

    size_t clen = 0;
    uint8_t *content = get_page_content_stream(doc, po, &clen);
    if (content && clen > 0) {
        execute_pdf_stream(&rc, (const char *)content, clen, 0);
    }
    free(content);

    /* 4. 強韌回退：若該頁有圖片 XObject 但串流未經標準 Do 觸發 (部分掃描器產出的極簡 PDF)，自動滿頁繪製該圖片！ */
    if (rc.images_drawn == 0 && rc.text_runs_drawn == 0 && xcnt > 0) {
        for (int k = 0; k < xcnt; k++) {
            int iw = 0, ih = 0;
            uint8_t *rgb = decode_pdf_image_obj(doc, xobjs[k].obj_num, &iw, &ih);
            if (rgb && iw > 0 && ih > 0) {
                hal_blit_rgb888_scaled(rgb, iw, ih, px, py, pw, ph, sx0, sy0, sx1 - sx0, sy1 - sy0, night_invert);
                break;
            }
        }
    }
}

char *pdf_extract_page_text(PdfDoc *doc, int page_idx, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!doc || page_idx < 0 || page_idx >= doc->page_count) return NULL;
    const PdfObjRec *po = find_obj(doc, doc->pages[page_idx].obj_num);
    if (!po) return NULL;

    XObjMap xobjs[MAX_XOBJS];
    int xcnt = 0;
    collect_page_xobjects(doc, po, xobjs, &xcnt);

    PdfRenderCtx rc;
    memset(&rc, 0, sizeof(rc));
    rc.doc = doc;
    rc.xobjs = xobjs;
    rc.xobj_cnt = xcnt;
    rc.text_cap = 2048;
    rc.text_out = (char *)calloc(1, rc.text_cap);
    if (!rc.text_out) return NULL;
    rc.gstack[0].ctm = mat_ident();

    size_t clen = 0;
    uint8_t *content = get_page_content_stream(doc, po, &clen);
    if (content && clen > 0) {
        execute_pdf_stream(&rc, (const char *)content, clen, 0);
    }
    free(content);
    if (out_len) *out_len = rc.text_len;
    return rc.text_out;
}
