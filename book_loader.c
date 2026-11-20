#include "book_loader.h"
#include "inflate_tiny.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BOOK_TEXT_BYTES (6u * 1024u * 1024u)
#define MAX_STREAM_BYTES    (2u * 1024u * 1024u)

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

static void add_chapter(BookDoc *doc, size_t offset, const char *title) {
    if (doc->chapter_count >= BOOK_MAX_CHAPTERS) return;
    if (doc->chapter_count > 0 && doc->chapters[doc->chapter_count - 1].offset == offset) return;
    BookChapter *ch = &doc->chapters[doc->chapter_count++];
    ch->offset = offset;
    trim_copy(ch->title, sizeof(ch->title), title, strlen(title));
    if (ch->title[0] == '\0') snprintf(ch->title, sizeof(ch->title), "章節 %d", doc->chapter_count);
}

BookFormat book_detect_format(const char *filepath) {
    if (!filepath) return BOOK_FMT_UNKNOWN;
    if (ends_with_ci(filepath, ".epub")) return BOOK_FMT_EPUB;
    if (ends_with_ci(filepath, ".pdf"))  return BOOK_FMT_PDF;
    if (ends_with_ci(filepath, ".bmp"))  return BOOK_FMT_BMP;
    return BOOK_FMT_TXT;
}

const char *book_format_tag(BookFormat fmt) {
    switch (fmt) {
        case BOOK_FMT_EPUB: return "EPUB";
        case BOOK_FMT_PDF:  return "PDF";
        case BOOK_FMT_BMP:  return "BMP";
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

/* --- 原生 EPUB (ZIP + OPF + Spine + XHTML) 解析器 --- */
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

static void xhtml_to_text(const char *html, size_t len, StrBuf *sb, char *out_heading, size_t heading_sz) {
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

static int extract_xml_attr(const char *tag, size_t tlen, const char *attr, char *out, size_t out_sz) {
    const char *p = find_ci(tag, tlen, attr);
    if (!p) return 0;
    p += strlen(attr);
    const char *end = tag + tlen;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '=')) p++;
    if (p >= end || (*p != '"' && *p != '\'')) return 0;
    char quote = *p++;
    const char *val_start = p;
    while (p < end && *p != quote) p++;
    size_t vlen = (size_t)(p - val_start);
    if (vlen >= out_sz) vlen = out_sz - 1;
    memcpy(out, val_start, vlen);
    out[vlen] = '\0';
    return 1;
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

typedef struct { char id[64]; char href[160]; } OpfItem;

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

    const ZipEntry *oe = opf_path[0] ? zip_find(entries, entry_cnt, opf_path) : NULL;
    if (oe) {
        size_t olen = 0; uint8_t *odata = zip_extract_entry(zip, zlen, oe, &olen);
        if (odata) {
            const char *ox = (const char *)odata;
            extract_xml_tag_text(ox, olen, "<dc:title", "</dc:title>", doc->title, sizeof(doc->title));
            extract_xml_tag_text(ox, olen, "<dc:creator", "</dc:creator>", doc->author, sizeof(doc->author));

            manifest = (OpfItem *)calloc(512, sizeof(OpfItem));
            spine_paths = (char (*)[192])calloc(512, sizeof(*spine_paths));
            if (manifest && spine_paths) {
                const char *p = ox, *end = ox + olen;
                while (p < end && man_cnt < 512) {
                    const char *tag = find_ci(p, (size_t)(end - p), "<item ");
                    if (!tag) break;
                    const char *gt = (const char *)memchr(tag, '>', (size_t)(end - tag));
                    if (!gt) break;
                    char id[64] = {0}, href[160] = {0};
                    if (extract_xml_attr(tag, (size_t)(gt - tag), "id", id, sizeof(id)) &&
                        extract_xml_attr(tag, (size_t)(gt - tag), "href", href, sizeof(href))) {
                        snprintf(manifest[man_cnt].id, sizeof(manifest[man_cnt].id), "%s", id);
                        snprintf(manifest[man_cnt].href, sizeof(manifest[man_cnt].href), "%s", href);
                        man_cnt++;
                    }
                    p = gt + 1;
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

    StrBuf sb = {0};
    if (spine_cnt > 0 && spine_paths) {
        for (int s = 0; s < spine_cnt; s++) {
            const ZipEntry *ze = zip_find(entries, entry_cnt, spine_paths[s]);
            if (!ze) continue;
            size_t xlen = 0; uint8_t *xdata = zip_extract_entry(zip, zlen, ze, &xlen);
            if (!xdata) continue;
            size_t ch_off = sb.len; char heading[88] = {0};
            xhtml_to_text((const char *)xdata, xlen, &sb, heading, sizeof(heading));
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
                xhtml_to_text((const char *)xdata, xlen, &sb, heading, sizeof(heading));
                if (sb.len > ch_off + 2) {
                    if (!heading[0]) snprintf(heading, sizeof(heading), "第 %d 節", doc->chapter_count + 1);
                    add_chapter(doc, ch_off, heading);
                }
                free(xdata);
            }
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

    if (out_doc->format == BOOK_FMT_BMP || out_doc->format == BOOK_FMT_PDF) return 1;
    if (out_doc->format == BOOK_FMT_EPUB) return load_epub(filepath, out_doc);
    return load_txt(filepath, out_doc);
}
