#include "book_loader.h"
#include "inflate_tiny.h"
#include "jpeg_tiny.h"
#include "png_tiny.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BOOK_TEXT_BYTES (6u * 1024u * 1024u)
#define MAX_STREAM_BYTES    (4u * 1024u * 1024u)
#define COVER_THUMB_W       96
#define COVER_THUMB_H       128

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static int sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 > MAX_BOOK_TEXT_BYTES) return 0;
    if (sb->len + extra + 1 <= sb->cap) return 1;
    size_t ncap = sb->cap ? sb->cap * 2 : 16384;
    while (ncap < sb->len + extra + 1) ncap *= 2;
    if (ncap > MAX_BOOK_TEXT_BYTES) ncap = MAX_BOOK_TEXT_BYTES;
    char *nd = (char *)realloc(sb->data, ncap);
    if (!nd) return 0;
    sb->data = nd; sb->cap = ncap;
    return 1;
}

static void sb_append_mem(StrBuf *sb, const char *p, size_t n) {
    if (n == 0 || !sb_reserve(sb, n)) return;
    memcpy(sb->data + sb->len, p, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append_char(StrBuf *sb, char c) {
    if (!sb_reserve(sb, 1)) return;
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

static void sb_append_str(StrBuf *sb, const char *s) {
    if (s) sb_append_mem(sb, s, strlen(s));
}

static void sb_append_utf8_cp(StrBuf *sb, uint32_t cp) {
    char buf[4]; size_t n = 0;
    if (cp == 0 || cp > 0x10FFFFu) return;
    if (cp < 0x80u) { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800u) { buf[0] = (char)(0xC0u | (cp >> 6)); buf[1] = (char)(0x80u | (cp & 0x3Fu)); n = 2; }
    else if (cp < 0x10000u) {
        buf[0] = (char)(0xE0u | (cp >> 12));
        buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        buf[0] = (char)(0xF0u | (cp >> 18));
        buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[3] = (char)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    sb_append_mem(sb, buf, n);
}

static int ends_with_ci(const char *str, const char *suffix) {
    size_t slen = strlen(str), xlen = strlen(suffix);
    if (slen < xlen) return 0;
    const char *p = str + slen - xlen;
    for (size_t i = 0; i < xlen; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}

static int str_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
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

static void trim_copy(char *dst, size_t dst_sz, const char *src, size_t src_len) {
    size_t s = 0;
    while (s < src_len && (src[s] == ' ' || src[s] == '\t' || src[s] == '\r' || src[s] == '\n')) s++;
    size_t e = src_len;
    while (e > s && (src[e - 1] == ' ' || src[e - 1] == '\t' || src[e - 1] == '\r' || src[e - 1] == '\n')) e--;
    size_t n = e - s;
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src + s, n);
    dst[n] = '\0';
}

static void get_base_title(const char *filepath, char *out, size_t out_sz) {
    const char *p1 = strrchr(filepath, '\\');
    const char *p2 = strrchr(filepath, '/');
    const char *name = (p1 > p2) ? (p1 + 1) : (p2 ? (p2 + 1) : filepath);
    const char *dot = strrchr(name, '.');
    size_t len = dot ? (size_t)(dot - name) : strlen(name);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, name, len);
    out[len] = '\0';
}

/* 解析 ZIP 內相對路徑 (正確處理 ../ 與 ./) */
static void resolve_zip_relpath(const char *cur_file, const char *rel, char *out, size_t out_sz) {
    if (!rel || !rel[0]) { out[0] = '\0'; return; }
    char combined[256];
    if (rel[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", rel + 1);
    } else {
        const char *sl = cur_file ? strrchr(cur_file, '/') : NULL;
        if (sl) {
            size_t dlen = (size_t)(sl - cur_file) + 1;
            if (dlen > 120) dlen = 120;
            memcpy(combined, cur_file, dlen);
            snprintf(combined + dlen, sizeof(combined) - dlen, "%s", rel);
        } else {
            snprintf(combined, sizeof(combined), "%s", rel);
        }
    }
    /* 移除 #fragment 與 ?query */
    char *q = strchr(combined, '#'); if (q) *q = '\0';
    q = strchr(combined, '?'); if (q) *q = '\0';

    /* 正規化 path segments */
    const char *segs[32];
    int seg_cnt = 0;
    char *p = combined;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p == '/') *p++ = '\0';
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) {
            if (seg_cnt > 0) seg_cnt--;
            continue;
        }
        if (seg_cnt < 32) segs[seg_cnt++] = start;
    }
    out[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < seg_cnt; i++) {
        size_t slen = strlen(segs[i]);
        if (pos + slen + 2 >= out_sz) break;
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, segs[i], slen);
        pos += slen;
        out[pos] = '\0';
    }
}

static void add_chapter(BookDoc *doc, size_t offset, const char *title) {
    if (doc->chapter_count >= BOOK_MAX_CHAPTERS) return;
    if (doc->chapter_count > 0 && doc->chapters[doc->chapter_count - 1].offset == offset) return;
    BookChapter *ch = &doc->chapters[doc->chapter_count++];
    ch->offset = offset;
    trim_copy(ch->title, sizeof(ch->title), title, strlen(title));
    if (ch->title[0] == '\0') snprintf(ch->title, sizeof(ch->title), "章節 %d", doc->chapter_count);
}

static int add_epub_image(BookDoc *doc, const char *zip_path) {
    if (!zip_path || !zip_path[0]) return -1;
    if (!ends_with_ci(zip_path, ".jpg") && !ends_with_ci(zip_path, ".jpeg") && !ends_with_ci(zip_path, ".png"))
        return -1;
    if (doc->image_count >= BOOK_MAX_IMAGES) return -1;
    int idx = doc->image_count++;
    snprintf(doc->images[idx].zip_path, sizeof(doc->images[idx].zip_path), "%s", zip_path);
    return idx;
}

BookFormat book_detect_format(const char *filepath) {
    if (!filepath) return BOOK_FMT_UNKNOWN;
    if (ends_with_ci(filepath, ".epub")) return BOOK_FMT_EPUB;
    if (ends_with_ci(filepath, ".pdf"))  return BOOK_FMT_PDF;
    if (ends_with_ci(filepath, ".bmp"))  return BOOK_FMT_BMP;
    if (ends_with_ci(filepath, ".png"))  return BOOK_FMT_PNG;
    return BOOK_FMT_TXT;
}

const char *book_format_tag(BookFormat fmt) {
    switch (fmt) {
        case BOOK_FMT_EPUB: return "EPUB";
        case BOOK_FMT_PDF:  return "PDF";
        case BOOK_FMT_BMP:  return "BMP";
        case BOOK_FMT_PNG:  return "PNG";
        default:            return "TXT";
    }
}

void book_doc_free(BookDoc *doc) {
    if (!doc) return;
    free(doc->text);
    memset(doc, 0, sizeof(*doc));
}

static uint8_t *read_entire_file(const char *filepath, size_t max_bytes, size_t *out_len) {
    if (out_len) *out_len = 0;
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > max_bytes) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

static int is_txt_chapter_line(const char *s, size_t len) {
    if (len == 0 || len > 96) return 0;
    size_t i = 0;
    while (i < len) {
        if (s[i] == ' ' || s[i] == '\t') i++;
        else if (i + 2 < len && (unsigned char)s[i] == 0xE3 && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x80) i += 3;
        else break;
    }
    if (i >= len) return 0;
    const char *p = s + i; size_t rem = len - i;
    if (rem >= 7 && (strncmp(p, "Chapter", 7) == 0 || strncmp(p, "CHAPTER", 7) == 0 || strncmp(p, "chapter", 7) == 0)) return 1;
    if (rem >= 6 && (unsigned char)p[0] == 0xE7 && (unsigned char)p[1] == 0xAC && (unsigned char)p[2] == 0xAC) {
        size_t lim = rem < 30 ? rem : 30;
        for (size_t k = 3; k + 2 < lim; k++) {
            unsigned char b0 = (unsigned char)p[k], b1 = (unsigned char)p[k + 1], b2 = (unsigned char)p[k + 2];
            if ((b0 == 0xE7 && b1 == 0xAB && b2 == 0xA0) ||
                (b0 == 0xE5 && b1 == 0x9B && b2 == 0x9E) ||
                (b0 == 0xE7 && b1 == 0xAF && b2 == 0x80) ||
                (b0 == 0xE8 && b1 == 0x8A && b2 == 0x82) ||
                (b0 == 0xE5 && b1 == 0x8D && b2 == 0xB7) ||
                (b0 == 0xE9 && b1 == 0x9B && b2 == 0x86) ||
                (b0 == 0xE9 && b1 == 0x83 && b2 == 0xA8)) return 1;
        }
    }
    const char *specials[] = { "序章", "前言", "楔子", "尾聲", "尾声", "番外", "後記", "后记" };
    for (int j = 0; j < (int)(sizeof(specials) / sizeof(specials[0])); j++) {
        size_t slen = strlen(specials[j]);
        if (rem >= slen && strncmp(p, specials[j], slen) == 0 && rem <= 36) return 1;
    }
    return 0;
}

static void scan_txt_chapters(BookDoc *doc) {
    if (!doc->text || doc->text_len == 0) return;
    size_t pos = 0;
    while (pos < doc->text_len && doc->chapter_count < BOOK_MAX_CHAPTERS) {
        size_t start = pos;
        while (pos < doc->text_len && doc->text[pos] != '\n' && doc->text[pos] != '\r') pos++;
        size_t len = pos - start;
        if (is_txt_chapter_line(doc->text + start, len)) {
            char title[88];
            trim_copy(title, sizeof(title), doc->text + start, len);
            add_chapter(doc, start, title);
        }
        if (pos < doc->text_len && doc->text[pos] == '\r') pos++;
        if (pos < doc->text_len && doc->text[pos] == '\n') pos++;
    }
}

static int load_txt(const char *filepath, BookDoc *doc) {
    size_t raw_len = 0;
    uint8_t *raw = read_entire_file(filepath, MAX_BOOK_TEXT_BYTES, &raw_len);
    if (!raw) return 0;

    StrBuf sb = {0};
    if (raw_len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        for (size_t i = 2; i + 1 < raw_len; i += 2)
            sb_append_utf8_cp(&sb, (uint32_t)raw[i] | ((uint32_t)raw[i + 1] << 8));
        free(raw);
        doc->text = sb.data; doc->text_len = sb.len;
    } else if (raw_len >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        for (size_t i = 2; i + 1 < raw_len; i += 2)
            sb_append_utf8_cp(&sb, ((uint32_t)raw[i] << 8) | (uint32_t)raw[i + 1]);
        free(raw);
        doc->text = sb.data; doc->text_len = sb.len;
    } else {
        size_t off = (raw_len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        if (off > 0) { memmove(raw, raw + off, raw_len - off + 1); raw_len -= off; }
        doc->text = (char *)raw; doc->text_len = raw_len;
    }

    scan_txt_chapters(doc);
    return (doc->text != NULL);
}

/* --- 原生 EPUB (ZIP + OPF + Cover + Spine + XHTML + Inline Images) --- */
typedef struct {
    char name[192];
    uint16_t method;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_off;
} ZipEntry;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse_zip_directory(const uint8_t *zip, size_t zlen, ZipEntry **out_entries, int *out_count) {
    *out_entries = NULL; *out_count = 0;
    if (zlen < 22) return 0;

    size_t search_start = (zlen > 65560) ? (zlen - 65560) : 0;
    size_t eocd = 0; int found = 0;
    for (size_t i = zlen - 22; ; i--) {
        if (zip[i] == 0x50 && zip[i + 1] == 0x4B && zip[i + 2] == 0x05 && zip[i + 3] == 0x06) {
            eocd = i; found = 1; break;
        }
        if (i == search_start) break;
    }
    if (!found) return 0;

    uint16_t total = rd16(zip + eocd + 10);
    uint32_t cd_size = rd32(zip + eocd + 12);
    uint32_t cd_off = rd32(zip + eocd + 16);
    if (total == 0 || cd_off >= zlen || cd_off + cd_size > zlen) return 0;

    int cap = (total > 1024) ? 1024 : (int)total;
    ZipEntry *arr = (ZipEntry *)calloc((size_t)cap, sizeof(ZipEntry));
    if (!arr) return 0;

    size_t pos = cd_off; int cnt = 0;
    while (cnt < cap && pos + 46 <= zlen) {
        if (zip[pos] != 0x50 || zip[pos + 1] != 0x4B || zip[pos + 2] != 0x01 || zip[pos + 3] != 0x02) break;
        uint16_t method = rd16(zip + pos + 10);
        uint32_t csz = rd32(zip + pos + 20), usz = rd32(zip + pos + 24);
        uint16_t nlen = rd16(zip + pos + 28), xlen = rd16(zip + pos + 30), clen = rd16(zip + pos + 32);
        uint32_t loff = rd32(zip + pos + 42);
        if (pos + 46 + nlen + xlen + clen > zlen) break;

        size_t copy_n = (nlen < sizeof(arr[cnt].name) - 1) ? nlen : (sizeof(arr[cnt].name) - 1);
        memcpy(arr[cnt].name, zip + pos + 46, copy_n);
        arr[cnt].name[copy_n] = '\0';
        arr[cnt].method = method; arr[cnt].comp_size = csz;
        arr[cnt].uncomp_size = usz; arr[cnt].local_off = loff;
        cnt++;
        pos += 46u + nlen + xlen + clen;
    }
    *out_entries = arr; *out_count = cnt;
    return (cnt > 0);
}

static uint8_t *zip_extract_entry(const uint8_t *zip, size_t zlen, const ZipEntry *e, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!e || e->local_off + 30 > zlen) return NULL;
    const uint8_t *lh = zip + e->local_off;
    if (lh[0] != 0x50 || lh[1] != 0x4B || lh[2] != 0x03 || lh[3] != 0x04) return NULL;
    uint16_t nlen = rd16(lh + 26), xlen = rd16(lh + 28);
    size_t data_off = (size_t)e->local_off + 30u + nlen + xlen;
    if (data_off + e->comp_size > zlen) return NULL;

    if (e->method == 0) {
        uint8_t *buf = (uint8_t *)malloc(e->comp_size + 1);
        if (!buf) return NULL;
        memcpy(buf, zip + data_off, e->comp_size);
        buf[e->comp_size] = 0;
        if (out_len) *out_len = e->comp_size;
        return buf;
    }
    if (e->method == 8) {
        return tiny_inflate_raw_alloc(zip + data_off, e->comp_size, e->uncomp_size, MAX_STREAM_BYTES, out_len);
    }
    return NULL;
}

static const ZipEntry *zip_find(const ZipEntry *entries, int count, const char *path) {
    for (int i = 0; i < count; i++) if (str_eq_ci(entries[i].name, path)) return &entries[i];
    char dec[192]; size_t di = 0;
    for (size_t i = 0; path[i] && di + 1 < sizeof(dec); i++) {
        if (path[i] == '%' && isxdigit((unsigned char)path[i + 1]) && isxdigit((unsigned char)path[i + 2])) {
            char hx[3] = { path[i + 1], path[i + 2], 0 };
            dec[di++] = (char)strtol(hx, NULL, 16);
            i += 2;
        } else dec[di++] = path[i];
    }
    dec[di] = '\0';
    for (int i = 0; i < count; i++) if (str_eq_ci(entries[i].name, dec)) return &entries[i];
    return NULL;
}

static uint8_t *decode_raw_image_mem(const uint8_t *raw, size_t len, int *out_w, int *out_h) {
    if (!raw || len < 16) return NULL;
    static const uint8_t PNG_SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (memcmp(raw, PNG_SIG, 8) == 0) return tiny_png_decode(raw, len, out_w, out_h);
    if (raw[0] == 0xFF && raw[1] == 0xD8) return tiny_jpeg_decode(raw, len, out_w, out_h);
    return NULL;
}

/* 將 EPUB 真封面縮放為 96x128 並快取至 <filepath>.cov，讓書架瞬間載入零卡頓 */
static void save_cover_thumb_from_rgb(const char *filepath, const uint8_t *rgb, int iw, int ih) {
    if (!filepath || !rgb || iw <= 0 || ih <= 0) return;
    int tw = COVER_THUMB_W, th = COVER_THUMB_H;
    uint8_t *thumb = (uint8_t *)malloc((size_t)tw * (size_t)th * 3u);
    if (!thumb) return;

    for (int y = 0; y < th; y++) {
        int sy = (y * ih) / th;
        if (sy >= ih) sy = ih - 1;
        for (int x = 0; x < tw; x++) {
            int sx = (x * iw) / tw;
            if (sx >= iw) sx = iw - 1;
            const uint8_t *sp = rgb + ((size_t)sy * (size_t)iw + (size_t)sx) * 3u;
            uint8_t *dp = thumb + ((size_t)y * (size_t)tw + (size_t)x) * 3u;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
        }
    }

    char cpath[420];
    snprintf(cpath, sizeof(cpath), "%s.cov", filepath);
    FILE *f = fopen(cpath, "wb");
    if (f) {
        uint8_t hdr[8] = { 'C', 'O', 'V', '1',
                           (uint8_t)(tw & 0xFF), (uint8_t)((tw >> 8) & 0xFF),
                           (uint8_t)(th & 0xFF), (uint8_t)((th >> 8) & 0xFF) };
        fwrite(hdr, 1, 8, f);
        fwrite(thumb, 1, (size_t)tw * (size_t)th * 3u, f);
        fclose(f);
    }
    free(thumb);
}

uint8_t *book_load_cover_thumb(const char *filepath, int *out_w, int *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!filepath || !filepath[0]) return NULL;
    char cpath[420];
    snprintf(cpath, sizeof(cpath), "%s.cov", filepath);
    FILE *f = fopen(cpath, "rb");
    if (!f) return NULL;
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "COV1", 4) != 0) { fclose(f); return NULL; }
    int w = (int)hdr[4] | ((int)hdr[5] << 8);
    int h = (int)hdr[6] | ((int)hdr[7] << 8);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) { fclose(f); return NULL; }
    size_t need = (size_t)w * (size_t)h * 3u;
    uint8_t *rgb = (uint8_t *)malloc(need);
    if (!rgb || fread(rgb, 1, need, f) != need) { free(rgb); fclose(f); return NULL; }
    fclose(f);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return rgb;
}

uint8_t *book_extract_epub_image(const char *epub_path, const char *zip_path, int *out_w, int *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!epub_path || !zip_path || !zip_path[0]) return NULL;

    size_t zlen = 0;
    uint8_t *zip = read_entire_file(epub_path, 24u * 1024u * 1024u, &zlen);
    if (!zip) return NULL;

    ZipEntry *entries = NULL; int entry_cnt = 0;
    uint8_t *rgb = NULL;
    if (parse_zip_directory(zip, zlen, &entries, &entry_cnt)) {
        const ZipEntry *ze = zip_find(entries, entry_cnt, zip_path);
        if (ze) {
            size_t ilen = 0;
            uint8_t *raw = zip_extract_entry(zip, zlen, ze, &ilen);
            if (raw && ilen > 0) rgb = decode_raw_image_mem(raw, ilen, out_w, out_h);
            free(raw);
        }
        free(entries);
    }
    free(zip);
    return rgb;
}

static int extract_xml_attr(const char *tag, size_t tlen, const char *attr, char *out, size_t out_sz) {
    const char *p = find_ci(tag, tlen, attr);
    while (p) {
        if (p == tag || isspace((unsigned char)p[-1]) || p[-1] == ':') {
            const char *q = p + strlen(attr);
            const char *end = tag + tlen;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q < end && *q == '=') {
                q++;
                while (q < end && (*q == ' ' || *q == '\t')) q++;
                if (q < end && (*q == '"' || *q == '\'')) {
                    char quote = *q++;
                    const char *vs = q;
                    while (q < end && *q != quote) q++;
                    size_t vl = (size_t)(q - vs);
                    if (vl >= out_sz) vl = out_sz - 1;
                    memcpy(out, vs, vl);
                    out[vl] = '\0';
                    return 1;
                }
            }
        }
        p = find_ci(p + 1, (size_t)(tag + tlen - (p + 1)), attr);
    }
    return 0;
}

static size_t decode_html_entity(const char *s, size_t rem, StrBuf *sb) {
    size_t end = 1;
    while (end < rem && end < 12 && s[end] != ';') end++;
    if (end >= rem || s[end] != ';') { sb_append_char(sb, '&'); return 1; }
    char ent[16]; size_t elen = end - 1;
    if (elen >= sizeof(ent)) { sb_append_char(sb, '&'); return 1; }
    memcpy(ent, s + 1, elen); ent[elen] = '\0';

    if (ent[0] == '#') {
        uint32_t cp = (ent[1] == 'x' || ent[1] == 'X') ? (uint32_t)strtoul(ent + 2, NULL, 16) : (uint32_t)strtoul(ent + 1, NULL, 10);
        if (cp > 0) sb_append_utf8_cp(sb, cp);
    } else if (strcmp(ent, "nbsp") == 0)  sb_append_char(sb, ' ');
    else if (strcmp(ent, "amp") == 0)     sb_append_char(sb, '&');
    else if (strcmp(ent, "lt") == 0)      sb_append_char(sb, '<');
    else if (strcmp(ent, "gt") == 0)      sb_append_char(sb, '>');
    else if (strcmp(ent, "quot") == 0)    sb_append_char(sb, '"');
    else if (strcmp(ent, "apos") == 0)    sb_append_char(sb, '\'');
    else if (strcmp(ent, "mdash") == 0)   sb_append_utf8_cp(sb, 0x2014);
    else if (strcmp(ent, "ndash") == 0)   sb_append_utf8_cp(sb, 0x2013);
    else if (strcmp(ent, "hellip") == 0)  sb_append_utf8_cp(sb, 0x2026);
    else if (strcmp(ent, "ldquo") == 0)   sb_append_utf8_cp(sb, 0x201C);
    else if (strcmp(ent, "rdquo") == 0)   sb_append_utf8_cp(sb, 0x201D);
    else if (strcmp(ent, "lsquo") == 0)   sb_append_utf8_cp(sb, 0x2018);
    else if (strcmp(ent, "rsquo") == 0)   sb_append_utf8_cp(sb, 0x2019);
    else sb_append_char(sb, ' ');
    return end + 1;
}

/* 解析 XHTML 文字 + 自動擷取 <img> / <image> 內嵌插圖標記 (\x01IMG:idx) */
static void xhtml_to_text(const char *html, size_t len, const char *xhtml_zip_path,
                          BookDoc *doc, StrBuf *sb, char *out_heading, size_t heading_sz) {
    out_heading[0] = '\0';
    int in_skip = 0, in_heading = 0, last_was_space = 1, nl_count = 0;
    StrBuf hbuf = {0};

    size_t i = 0;
    while (i < len) {
        if (html[i] == '<') {
            size_t j = i + 1;
            while (j < len && html[j] != '>') j++;
            if (j >= len) break;
            size_t tlen = j - (i + 1);
            const char *ts = html + i + 1;
            int closing = 0;
            if (tlen > 0 && ts[0] == '/') { closing = 1; ts++; tlen--; }
            char tname[24]; size_t tn = 0;
            while (tn < tlen && tn + 1 < sizeof(tname) &&
                   ts[tn] != ' ' && ts[tn] != '\t' && ts[tn] != '\r' && ts[tn] != '\n' && ts[tn] != '/') {
                tname[tn] = (char)tolower((unsigned char)ts[tn]); tn++;
            }
            tname[tn] = '\0';

            if (strcmp(tname, "style") == 0 || strcmp(tname, "script") == 0 || strcmp(tname, "head") == 0) {
                in_skip = closing ? 0 : 1;
            } else if (!in_skip && !closing && (strcmp(tname, "img") == 0 || strcmp(tname, "image") == 0)) {
                char src[192] = {0};
                int ok_src = extract_xml_attr(ts, tlen, "src", src, sizeof(src));
                if (!ok_src) ok_src = extract_xml_attr(ts, tlen, "href", src, sizeof(src));
                if (ok_src && src[0]) {
                    char resolved[192];
                    resolve_zip_relpath(xhtml_zip_path, src, resolved, sizeof(resolved));
                    int img_id = add_epub_image(doc, resolved);
                    if (img_id >= 0) {
                        if (sb->len > 0 && sb->data[sb->len - 1] != '\n') sb_append_char(sb, '\n');
                        char marker[32];
                        snprintf(marker, sizeof(marker), "\x01IMG:%d\n", img_id);
                        sb_append_str(sb, marker);
                        nl_count = 1; last_was_space = 1;
                    }
                }
            } else if (strcmp(tname, "h1") == 0 || strcmp(tname, "h2") == 0 || strcmp(tname, "h3") == 0) {
                if (!closing && out_heading[0] == '\0') { in_heading = 1; hbuf.len = 0; }
                else if (closing && in_heading) {
                    in_heading = 0;
                    if (hbuf.data && hbuf.len > 0) trim_copy(out_heading, heading_sz, hbuf.data, hbuf.len);
                }
                if (nl_count < 2 && sb->len > 0) { sb_append_char(sb, '\n'); nl_count++; last_was_space = 1; }
            } else if (strcmp(tname, "p") == 0 || strcmp(tname, "div") == 0 ||
                       strcmp(tname, "br") == 0 || strcmp(tname, "li") == 0 ||
                       strcmp(tname, "tr") == 0 || strcmp(tname, "blockquote") == 0 ||
                       strcmp(tname, "h4") == 0 || strcmp(tname, "h5") == 0) {
                if ((closing || strcmp(tname, "br") == 0) && nl_count < 2 && sb->len > 0) {
                    sb_append_char(sb, '\n'); nl_count++; last_was_space = 1;
                }
            }
            i = j + 1;
            continue;
        }
        if (in_skip) { i++; continue; }
        if (html[i] == '&') {
            if (in_heading) {
                StrBuf tmp = {0};
                size_t step = decode_html_entity(html + i, len - i, &tmp);
                if (tmp.data) { sb_append_mem(&hbuf, tmp.data, tmp.len); sb_append_mem(sb, tmp.data, tmp.len); free(tmp.data); }
                i += step;
            } else i += decode_html_entity(html + i, len - i, sb);
            last_was_space = 0; nl_count = 0;
            continue;
        }
        char ch = html[i++];
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            if (!last_was_space) {
                sb_append_char(sb, ' ');
                if (in_heading) sb_append_char(&hbuf, ' ');
                last_was_space = 1;
            }
        } else {
            sb_append_char(sb, ch);
            if (in_heading) sb_append_char(&hbuf, ch);
            last_was_space = 0; nl_count = 0;
        }
    }
    free(hbuf.data);
    if (sb->len > 0 && sb->data[sb->len - 1] != '\n') sb_append_char(sb, '\n');
    sb_append_char(sb, '\n');
}

static void extract_xml_tag_text(const char *xml, size_t xlen, const char *open_tag, const char *close_tag, char *out, size_t out_sz) {
    const char *p = find_ci(xml, xlen, open_tag);
    if (!p) return;
    p += strlen(open_tag);
    const char *end = xml + xlen;
    while (p < end && *p != '>') p++;
    if (p >= end) return;
    p++;
    const char *c = find_ci(p, (size_t)(end - p), close_tag);
    if (!c) return;
    trim_copy(out, out_sz, p, (size_t)(c - p));
}

typedef struct { char id[64]; char href[160]; int is_cover_prop; } OpfItem;

static int load_epub(const char *filepath, BookDoc *doc) {
    size_t zlen = 0;
    uint8_t *zip = read_entire_file(filepath, 24u * 1024u * 1024u, &zlen);
    if (!zip) return 0;

    ZipEntry *entries = NULL; int entry_cnt = 0;
    if (!parse_zip_directory(zip, zlen, &entries, &entry_cnt)) { free(zip); return 0; }

    char opf_path[192] = {0};
    const ZipEntry *ce = zip_find(entries, entry_cnt, "META-INF/container.xml");
    if (ce) {
        size_t clen = 0; uint8_t *cdata = zip_extract_entry(zip, zlen, ce, &clen);
        if (cdata) { extract_xml_attr((const char *)cdata, clen, "full-path", opf_path, sizeof(opf_path)); free(cdata); }
    }
    if (!opf_path[0]) {
        for (int i = 0; i < entry_cnt; i++) {
            if (ends_with_ci(entries[i].name, ".opf")) { snprintf(opf_path, sizeof(opf_path), "%s", entries[i].name); break; }
        }
    }

    char base_dir[160] = {0};
    const char *slash = strrchr(opf_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - opf_path) + 1;
        if (dlen < sizeof(base_dir)) { memcpy(base_dir, opf_path, dlen); base_dir[dlen] = '\0'; }
    }

    OpfItem *manifest = NULL; int man_cnt = 0;
    char (*spine_paths)[192] = NULL; int spine_cnt = 0;
    char cover_meta_id[64] = {0};

    const ZipEntry *oe = opf_path[0] ? zip_find(entries, entry_cnt, opf_path) : NULL;
    if (oe) {
        size_t olen = 0; uint8_t *odata = zip_extract_entry(zip, zlen, oe, &olen);
        if (odata) {
            const char *ox = (const char *)odata;
            extract_xml_tag_text(ox, olen, "<dc:title", "</dc:title>", doc->title, sizeof(doc->title));
            extract_xml_tag_text(ox, olen, "<dc:creator", "</dc:creator>", doc->author, sizeof(doc->author));

            /* 找 <meta name="cover" content="..."/> */
            const char *mp = ox, *end = ox + olen;
            while (mp < end) {
                const char *tag = find_ci(mp, (size_t)(end - mp), "<meta ");
                if (!tag) break;
                const char *gt = (const char *)memchr(tag, '>', (size_t)(end - tag));
                if (!gt) break;
                char mname[32] = {0}, mcontent[64] = {0};
                if (extract_xml_attr(tag, (size_t)(gt - tag), "name", mname, sizeof(mname)) &&
                    str_eq_ci(mname, "cover") &&
                    extract_xml_attr(tag, (size_t)(gt - tag), "content", mcontent, sizeof(mcontent))) {
                    snprintf(cover_meta_id, sizeof(cover_meta_id), "%s", mcontent);
                    break;
                }
                mp = gt + 1;
            }

            manifest = (OpfItem *)calloc(512, sizeof(OpfItem));
            spine_paths = (char (*)[192])calloc(512, sizeof(*spine_paths));
            if (manifest && spine_paths) {
                const char *p = ox;
                while (p < end && man_cnt < 512) {
                    const char *tag = find_ci(p, (size_t)(end - p), "<item ");
                    if (!tag) break;
                    const char *gt = (const char *)memchr(tag, '>', (size_t)(end - tag));
                    if (!gt) break;
                    size_t tl = (size_t)(gt - tag);
                    char id[64] = {0}, href[160] = {0}, props[64] = {0};
                    if (extract_xml_attr(tag, tl, "id", id, sizeof(id)) &&
                        extract_xml_attr(tag, tl, "href", href, sizeof(href))) {
                        snprintf(manifest[man_cnt].id, sizeof(manifest[man_cnt].id), "%s", id);
                        snprintf(manifest[man_cnt].href, sizeof(manifest[man_cnt].href), "%s", href);
                        if (extract_xml_attr(tag, tl, "properties", props, sizeof(props)) &&
                            find_ci(props, strlen(props), "cover-image")) {
                            manifest[man_cnt].is_cover_prop = 1;
                        }
                        man_cnt++;
                    }
                    p = gt + 1;
                }

                /* 優先順序尋找 EPUB 真封面圖片路徑 */
                for (int m = 0; m < man_cnt && !doc->cover_zip_path[0]; m++) {
                    if (manifest[m].is_cover_prop || (cover_meta_id[0] && strcmp(manifest[m].id, cover_meta_id) == 0)) {
                        if (ends_with_ci(manifest[m].href, ".jpg") || ends_with_ci(manifest[m].href, ".jpeg") || ends_with_ci(manifest[m].href, ".png")) {
                            snprintf(doc->cover_zip_path, sizeof(doc->cover_zip_path), "%s%s", base_dir, manifest[m].href);
                        }
                    }
                }

                p = ox;
                while (p < end && spine_cnt < 512) {
                    const char *tag = find_ci(p, (size_t)(end - p), "<itemref");
                    if (!tag) break;
                    const char *gt = (const char *)memchr(tag, '>', (size_t)(end - tag));
                    if (!gt) break;
                    char idref[64] = {0};
                    if (extract_xml_attr(tag, (size_t)(gt - tag), "idref", idref, sizeof(idref))) {
                        for (int m = 0; m < man_cnt; m++) {
                            if (strcmp(manifest[m].id, idref) == 0) {
                                snprintf(spine_paths[spine_cnt], 192, "%s%s", base_dir, manifest[m].href);
                                spine_cnt++; break;
                            }
                        }
                    }
                    p = gt + 1;
                }
            }
            free(odata);
        }
    }

    /* 若 OPF 未指定 cover，掃描 ZIP 中檔名含 cover 的圖片 */
    if (!doc->cover_zip_path[0]) {
        for (int i = 0; i < entry_cnt; i++) {
            if ((ends_with_ci(entries[i].name, ".jpg") || ends_with_ci(entries[i].name, ".jpeg") || ends_with_ci(entries[i].name, ".png")) &&
                find_ci(entries[i].name, strlen(entries[i].name), "cover")) {
                snprintf(doc->cover_zip_path, sizeof(doc->cover_zip_path), "%s", entries[i].name);
                break;
            }
        }
    }

    /* 解析所有章節與內嵌插圖 */
    StrBuf sb = {0};
    if (spine_cnt > 0 && spine_paths) {
        for (int s = 0; s < spine_cnt; s++) {
            const ZipEntry *ze = zip_find(entries, entry_cnt, spine_paths[s]);
            if (!ze) continue;
            size_t xlen = 0; uint8_t *xdata = zip_extract_entry(zip, zlen, ze, &xlen);
            if (!xdata) continue;
            size_t ch_off = sb.len; char heading[88] = {0};
            xhtml_to_text((const char *)xdata, xlen, spine_paths[s], doc, &sb, heading, sizeof(heading));
            if (sb.len > ch_off + 2) {
                if (!heading[0]) snprintf(heading, sizeof(heading), "第 %d 節", doc->chapter_count + 1);
                add_chapter(doc, ch_off, heading);
            }
            free(xdata);
        }
    } else {
        for (int i = 0; i < entry_cnt; i++) {
            if (ends_with_ci(entries[i].name, ".xhtml") || ends_with_ci(entries[i].name, ".html") || ends_with_ci(entries[i].name, ".htm")) {
                size_t xlen = 0; uint8_t *xdata = zip_extract_entry(zip, zlen, &entries[i], &xlen);
                if (!xdata) continue;
                size_t ch_off = sb.len; char heading[88] = {0};
                xhtml_to_text((const char *)xdata, xlen, entries[i].name, doc, &sb, heading, sizeof(heading));
                if (sb.len > ch_off + 2) {
                    if (!heading[0]) snprintf(heading, sizeof(heading), "第 %d 節", doc->chapter_count + 1);
                    add_chapter(doc, ch_off, heading);
                }
                free(xdata);
            }
        }
    }

    /* 若仍沒找到封面，取第一張插圖當封面；接著直接存出 96x128 封面快取檔 (.cov) */
    if (!doc->cover_zip_path[0] && doc->image_count > 0) {
        snprintf(doc->cover_zip_path, sizeof(doc->cover_zip_path), "%s", doc->images[0].zip_path);
    }
    if (doc->cover_zip_path[0]) {
        const ZipEntry *cov_e = zip_find(entries, entry_cnt, doc->cover_zip_path);
        if (cov_e) {
            size_t clen = 0;
            uint8_t *craw = zip_extract_entry(zip, zlen, cov_e, &clen);
            if (craw && clen > 0) {
                int cw = 0, ch = 0;
                uint8_t *crgb = decode_raw_image_mem(craw, clen, &cw, &ch);
                if (crgb && cw > 0 && ch > 0) save_cover_thumb_from_rgb(filepath, crgb, cw, ch);
                free(crgb);
            }
            free(craw);
        }
    }

    free(manifest); free(spine_paths); free(entries); free(zip);
    if (!sb.data || sb.len == 0) { free(sb.data); return 0; }
    doc->text = sb.data; doc->text_len = sb.len;
    return 1;
}

int book_load_document(const char *filepath, BookDoc *out_doc) {
    if (!filepath || !out_doc) return 0;
    memset(out_doc, 0, sizeof(*out_doc));
    out_doc->format = book_detect_format(filepath);
    get_base_title(filepath, out_doc->title, sizeof(out_doc->title));

    if (out_doc->format == BOOK_FMT_BMP || out_doc->format == BOOK_FMT_PNG || out_doc->format == BOOK_FMT_PDF) return 1;
    if (out_doc->format == BOOK_FMT_EPUB) return load_epub(filepath, out_doc);
    return load_txt(filepath, out_doc);
}
