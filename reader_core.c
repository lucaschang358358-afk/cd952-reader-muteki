#include "reader_core.h"
#include "book_loader.h"
#include "pdf_engine.h"
#include "ui_gfx.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * CD-952 現代化全功能電子書閱讀器 v4
 *  - 真視覺化 PDF 閱讀引擎 (紙張/陰影/內嵌JPEG與Flate圖片/向量框線/定位中英文 + 縮放/平移/夜間反白 + 可選單頁重排)
 *  - 原生 EPUB (ZIP + OPF + Spine + XHTML + 章節目錄) / TXT (UTF-8/UTF-16) / 編號 BMP 圖片書
 *  - 現代 App 介面 (左側導覽列 + 繼續閱讀主卡片 + 4欄立體書封 + 4套現代主題色票)
 *  - 全套進階功能：多組手動書籤 (最多16筆)、全文關鍵字搜尋、章節目錄、視覺跳頁滑桿、ESC 閱讀控制面板
 * ============================================================ */

typedef enum {
    SCR_HOME = 0,
    SCR_READ_TEXT,
    SCR_READ_PDF,
    SCR_READ_BMP,
    SCR_READ_MENU,
    SCR_TOC,
    SCR_JUMP,
    SCR_BOOKMARKS,
    SCR_SEARCH
} ScreenMode;

typedef enum {
    TAB_SHELF = 0,
    TAB_MANAGE,
    TAB_APPEARANCE
} HomeTab;

typedef struct {
    const char *name;
    const char *eng;
    int is_dark;
    uint32_t bg;
    uint32_t rail_bg;
    uint32_t card_bg;
    uint32_t card_border;
    uint32_t shadow;
    uint32_t bar_bg;
    uint32_t text;
    uint32_t muted;
    uint32_t accent;
    uint32_t track;
} AppTheme;

static const AppTheme THEMES[4] = {
    /* 0: 象牙晨光 */
    { "象牙晨光", "IVORY", 0, 0xF4F1EA, 0xEAE5DB, 0xFFFFFF, 0xDDD6C8, 0xD3CCC0, 0xEFECE4, 0x1F242B, 0x6B7280, 0x2563EB, 0xE2DDD2 },
    /* 1: 護眼森綠 */
    { "護眼森綠", "SAGE",  0, 0xE3EFE5, 0xD4E5D7, 0xF3F9F4, 0xBBD2BF, 0xB0C7B4, 0xDBEADB, 0x1A2E20, 0x52725C, 0x059669, 0xC4DAC7 },
    /* 2: 柔和暖茶 */
    { "柔和暖茶", "SEPIA", 0, 0xF2E7D5, 0xE7D8C0, 0xFAF4E8, 0xD9C7AB, 0xCCBA9D, 0xECE0CB, 0x3A291C, 0x7A624E, 0xC2410C, 0xDDD0BC },
    /* 3: 暗夜極黑 */
    { "暗夜極黑", "NIGHT", 1, 0x111419, 0x171B22, 0x1C212A, 0x2B3340, 0x090B0E, 0x161A21, 0xE2E8F0, 0x8A94A6, 0x3B82F6, 0x2A313D }
};

#define MAX_BOOKS       32
#define MAX_BOOKMARKS   16
#define MAX_SEARCH_HITS 64

typedef struct {
    char path[384];
    char title[96];
    char author[64];
    BookFormat format;
    size_t byte_offset;
    size_t total_bytes;
    int page_idx;
    int page_count;
} BookEntry;

typedef struct {
    size_t pos_val; /* TXT/EPUB: byte offset; PDF: page_idx */
    int page_num;   /* 1-based display page */
    int pct;
    char label[72];
} ManualBookmark;

typedef struct {
    size_t pos_val; /* TXT/EPUB: byte offset; PDF: page_idx */
    int page_num;   /* 1-based */
    char snippet[84];
} SearchHit;

static ScreenMode g_screen = SCR_HOME;
static HomeTab g_home_tab = TAB_SHELF;
static int g_theme_idx = 0;
static int g_spacing_idx = 1;
static const int SPACING_PX[3] = { 5, 9, 14 };
static const char *SPACING_NAMES[3] = { "緊湊", "標準", "寬鬆" };
static const char *PDF_ZOOM_NAMES[5] = { "適應整頁", "適應寬度", "135%", "175%", "225%" };
static const int FONT_H = 18;

/* 書庫 */
static BookEntry g_books[MAX_BOOKS];
static int g_book_count = 0;
static int g_shelf_sel = 0;
static int g_manage_scroll = 0;
static int g_appear_row = 0;

/* 當前開啟書籍 (EPUB/TXT 或 真 PDF 或 BMP) */
static char g_filepath[384] = {0};
static BookDoc g_doc;
static PdfDoc *g_pdf = NULL;

/* TXT/EPUB 分頁 */
static size_t *g_page_offsets = NULL;
static int g_page_count = 0;
static int g_current_page = 0;

/* 真 PDF 檢視狀態 */
static int g_pdf_page = 0;
static int g_pdf_zoom = 0; /* 0=FitPage, 1=FitWidth, 2=135%, 3=175%, 4=225% */
static int g_pdf_pan_x = 0;
static int g_pdf_pan_y = 0;
static int g_pdf_last_pw = 800;
static int g_pdf_last_ph = 400;
static int g_pdf_reflow = 0; /* 0=真原版面PDF(預設), 1=單頁文字重排放大檢視 */
static char *g_pdf_reflow_text = NULL;
static size_t g_pdf_reflow_len = 0;
static int g_pdf_reflow_page_cached = -1;

/* 多組手動書籤 */
static ManualBookmark g_bmarks[MAX_BOOKMARKS];
static int g_bmark_count = 0;
static int g_bmark_sel = 0;

/* 全文搜尋 */
static char g_search_query[64] = {0};
static SearchHit g_search_hits[MAX_SEARCH_HITS];
static int g_search_cnt = 0;
static int g_search_sel = 0;
static int g_search_scroll = 0;
static int g_search_ran = 0;

/* 控制選單 / 目錄 / 跳頁 / 提示 */
static int g_read_menu_sel = 0;
static int g_toc_sel = 0;
static int g_toc_scroll = 0;
static int g_jump_target = 0;
static char g_toast_msg[128] = {0};

/* --- UTF-8 與基礎輔助 --- */
static int utf8_char_info(const unsigned char *s, size_t remaining, int *out_units) {
    if (remaining == 0) { *out_units = 0; return 0; }
    unsigned char c = s[0];
    if ((c & 0x80) == 0) { *out_units = 1; return 1; }
    if ((c & 0xE0) == 0xC0 && remaining >= 2) { *out_units = 2; return 2; }
    if ((c & 0xF0) == 0xE0 && remaining >= 3) { *out_units = 2; return 3; }
    if ((c & 0xF8) == 0xF0 && remaining >= 4) { *out_units = 2; return 4; }
    *out_units = 1;
    return 1;
}

static void utf8_ellipsis(const char *src, int max_units, char *dst, size_t dst_sz) {
    if (!src || dst_sz == 0) { if (dst_sz) dst[0] = '\0'; return; }
    size_t slen = strlen(src), pos = 0, out = 0;
    int units = 0;
    while (pos < slen) {
        int u = 0;
        int clen = utf8_char_info((const unsigned char *)(src + pos), slen - pos, &u);
        if (units + u > max_units) {
            if (out + 3 < dst_sz) { dst[out++] = '.'; dst[out++] = '.'; }
            break;
        }
        if (out + (size_t)clen >= dst_sz) break;
        memcpy(dst + out, src + pos, (size_t)clen);
        out += (size_t)clen; pos += (size_t)clen; units += u;
    }
    dst[out] = '\0';
}

static const char *get_filename_only(const char *path) {
    const char *p1 = strrchr(path, '\\');
    const char *p2 = strrchr(path, '/');
    const char *p = (p1 > p2) ? p1 : p2;
    return p ? (p + 1) : path;
}

static void draw_str(int x, int y, const char *s, int max_w, uint32_t rgb) {
    if (!s || !s[0]) return;
    hal_draw_line_utf8(x, y, s, (int)strlen(s), max_w, rgb);
}

static void sanitize_field(char *dst, size_t sz, const char *src) {
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 1 < sz; i++) {
        char c = src[i];
        if (c == '|' || c == '\r' || c == '\n') c = ' ';
        dst[o++] = c;
    }
    dst[o] = '\0';
}

static ScreenMode active_read_screen(void) {
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) return SCR_READ_PDF;
    if (g_doc.format == BOOK_FMT_BMP) return SCR_READ_BMP;
    return SCR_READ_TEXT;
}

/* --- 書庫索引讀寫 --- */
static void save_library(void) {
    const char *lib_path = hal_get_library_path();
    if (!lib_path || !lib_path[0]) return;
    FILE *f = fopen(lib_path, "wb");
    if (!f) return;
    fprintf(f, "#CFG4|%d|%d\n", g_theme_idx, g_spacing_idx);
    for (int i = 0; i < g_book_count; i++) {
        char t[96], a[64];
        sanitize_field(t, sizeof(t), g_books[i].title);
        sanitize_field(a, sizeof(a), g_books[i].author);
        fprintf(f, "%d|%zu|%zu|%d|%d|%s|%s|%s\n",
                (int)g_books[i].format, g_books[i].byte_offset, g_books[i].total_bytes,
                g_books[i].page_idx, g_books[i].page_count, t, a, g_books[i].path);
    }
    fclose(f);
}

static void load_library(void) {
    g_book_count = 0;
    const char *lib_path = hal_get_library_path();
    if (!lib_path || !lib_path[0]) return;
    FILE *f = fopen(lib_path, "rb");
    if (!f) return;

    char line[768];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        if (strncmp(line, "#CFG4|", 6) == 0 || strncmp(line, "#CFG3|", 6) == 0) {
            int th = 0, sp = 1;
            if (sscanf(line + 6, "%d|%d", &th, &sp) == 2) {
                if (th >= 0 && th < 4) g_theme_idx = th;
                if (sp >= 0 && sp < 3) g_spacing_idx = sp;
            }
            continue;
        }
        if (line[0] == '#' || g_book_count >= MAX_BOOKS) continue;

        char *p1 = strchr(line, '|'); if (!p1) continue;
        char *p2 = strchr(p1 + 1, '|'); if (!p2) continue;
        char *p3 = strchr(p2 + 1, '|'); if (!p3) continue;
        char *p4 = strchr(p3 + 1, '|'); if (!p4) continue;
        char *p5 = strchr(p4 + 1, '|'); if (!p5) continue;
        char *p6 = strchr(p5 + 1, '|'); if (!p6) continue;
        char *p7 = strchr(p6 + 1, '|'); if (!p7) continue;

        BookEntry *be = &g_books[g_book_count++];
        memset(be, 0, sizeof(*be));
        be->format = (BookFormat)atoi(line);
        be->byte_offset = (size_t)strtoull(p1 + 1, NULL, 10);
        be->total_bytes = (size_t)strtoull(p2 + 1, NULL, 10);
        be->page_idx = atoi(p3 + 1);
        be->page_count = atoi(p4 + 1);

        size_t tlen = (size_t)(p6 - (p5 + 1));
        if (tlen >= sizeof(be->title)) tlen = sizeof(be->title) - 1;
        memcpy(be->title, p5 + 1, tlen); be->title[tlen] = '\0';

        size_t alen = (size_t)(p7 - (p6 + 1));
        if (alen >= sizeof(be->author)) alen = sizeof(be->author) - 1;
        memcpy(be->author, p6 + 1, alen); be->author[alen] = '\0';

        snprintf(be->path, sizeof(be->path), "%s", p7 + 1);
        if (be->title[0] == '\0') utf8_ellipsis(get_filename_only(be->path), 40, be->title, sizeof(be->title));
    }
    fclose(f);
}

static void update_book_in_library(const char *path, BookFormat fmt, const char *title, const char *author,
                                   size_t boff, size_t btot, int pidx, int pcnt) {
    if (!path || !path[0]) return;
    int found = -1;
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].path, path) == 0) { found = i; break; }
    }

    BookEntry cur;
    memset(&cur, 0, sizeof(cur));
    snprintf(cur.path, sizeof(cur.path), "%s", path);
    const char *use_title = (title && title[0]) ? title : get_filename_only(path);
    utf8_ellipsis(use_title, 44, cur.title, sizeof(cur.title));
    sanitize_field(cur.author, sizeof(cur.author), author ? author : "");
    cur.format = fmt;
    cur.byte_offset = boff; cur.total_bytes = btot;
    cur.page_idx = pidx; cur.page_count = pcnt;

    if (found >= 0) {
        for (int i = found; i > 0; i--) g_books[i] = g_books[i - 1];
        g_books[0] = cur;
    } else {
        int move_n = (g_book_count < MAX_BOOKS) ? g_book_count : (MAX_BOOKS - 1);
        for (int i = move_n; i > 0; i--) g_books[i] = g_books[i - 1];
        g_books[0] = cur;
        if (g_book_count < MAX_BOOKS) g_book_count++;
    }
    g_shelf_sel = 0; g_manage_scroll = 0;
    save_library();
}

static void remove_selected_book(void) {
    if (g_book_count <= 0 || g_shelf_sel < 0 || g_shelf_sel >= g_book_count) return;
    for (int i = g_shelf_sel; i + 1 < g_book_count; i++) g_books[i] = g_books[i + 1];
    g_book_count--;
    if (g_shelf_sel >= g_book_count && g_book_count > 0) g_shelf_sel = g_book_count - 1;
    if (g_book_count == 0) { g_shelf_sel = 0; g_manage_scroll = 0; }
    save_library();
}

/* --- 自動進度書籤 + 多組手動書籤 (<file>.bmk2) --- */
static void save_manual_bookmarks(void) {
    if (!g_filepath[0]) return;
    char p[420];
    snprintf(p, sizeof(p), "%s.bmk2", g_filepath);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    for (int i = 0; i < g_bmark_count; i++) {
        char lbl[72];
        sanitize_field(lbl, sizeof(lbl), g_bmarks[i].label);
        fprintf(f, "%zu|%d|%d|%s\n", g_bmarks[i].pos_val, g_bmarks[i].page_num, g_bmarks[i].pct, lbl);
    }
    fclose(f);
}

static void load_manual_bookmarks(const char *filepath) {
    g_bmark_count = 0;
    g_bmark_sel = 0;
    if (!filepath || !filepath[0]) return;
    char p[420];
    snprintf(p, sizeof(p), "%s.bmk2", filepath);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_bmark_count < MAX_BOOKMARKS) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        char *p1 = strchr(line, '|'); if (!p1) continue;
        char *p2 = strchr(p1 + 1, '|'); if (!p2) continue;
        char *p3 = strchr(p2 + 1, '|'); if (!p3) continue;
        ManualBookmark *bm = &g_bmarks[g_bmark_count++];
        bm->pos_val = (size_t)strtoull(line, NULL, 10);
        bm->page_num = atoi(p1 + 1);
        bm->pct = atoi(p2 + 1);
        snprintf(bm->label, sizeof(bm->label), "%s", p3 + 1);
    }
    fclose(f);
}

static void add_current_manual_bookmark(void) {
    if (g_bmark_count >= MAX_BOOKMARKS) return;
    ManualBookmark *bm = &g_bmarks[g_bmark_count];
    memset(bm, 0, sizeof(*bm));

    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf);
        bm->pos_val = (size_t)g_pdf_page;
        bm->page_num = g_pdf_page + 1;
        bm->pct = tot > 0 ? ((g_pdf_page + 1) * 100) / tot : 0;
        size_t tlen = 0;
        char *pt = pdf_extract_page_text(g_pdf, g_pdf_page, &tlen);
        if (pt && tlen > 2) {
            char clean[72]; sanitize_field(clean, sizeof(clean), pt);
            utf8_ellipsis(clean, 28, bm->label, sizeof(bm->label));
        } else {
            snprintf(bm->label, sizeof(bm->label), "PDF 圖像頁 #%d", g_pdf_page + 1);
        }
        free(pt);
    } else if (g_doc.text && g_page_offsets && g_page_count > 0) {
        size_t off = g_page_offsets[g_current_page];
        bm->pos_val = off;
        bm->page_num = g_current_page + 1;
        bm->pct = ((g_current_page + 1) * 100) / g_page_count;
        char raw[80];
        size_t rem = g_doc.text_len - off;
        if (rem > 70) rem = 70;
        memcpy(raw, g_doc.text + off, rem); raw[rem] = '\0';
        char clean[80]; sanitize_field(clean, sizeof(clean), raw);
        utf8_ellipsis(clean, 28, bm->label, sizeof(bm->label));
    } else {
        return;
    }

    g_bmark_count++;
    g_bmark_sel = g_bmark_count - 1;
    save_manual_bookmarks();
}

static void save_bookmark(void) {
    if (!g_filepath[0]) return;
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf);
        char bmk_path[420];
        snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", g_filepath);
        FILE *f = fopen(bmk_path, "wb");
        if (f) { fprintf(f, "%d", g_pdf_page); fclose(f); }
        update_book_in_library(g_filepath, BOOK_FMT_PDF, g_doc.title, g_doc.author,
                               (size_t)g_pdf_page, (size_t)tot, g_pdf_page, tot);
    } else if (g_doc.format == BOOK_FMT_BMP) {
        update_book_in_library(g_filepath, BOOK_FMT_BMP, g_doc.title, "", 0, 0, 0, 0);
    } else if (g_page_count > 0 && g_page_offsets) {
        size_t off = g_page_offsets[g_current_page];
        char bmk_path[420];
        snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", g_filepath);
        FILE *f = fopen(bmk_path, "wb");
        if (f) { fprintf(f, "%zu", off); fclose(f); }
        update_book_in_library(g_filepath, g_doc.format, g_doc.title, g_doc.author,
                               off, g_doc.text_len, g_current_page, g_page_count);
    }
}

static size_t load_bookmark(const char *path) {
    if (!path || !path[0]) return 0;
    char bmk_path[420];
    snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", path);
    FILE *f = fopen(bmk_path, "rb");
    if (f) {
        size_t off = 0;
        if (fscanf(f, "%zu", &off) == 1) { fclose(f); return off; }
        fclose(f);
    }
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].path, path) == 0) return g_books[i].byte_offset;
    }
    return 0;
}

/* --- TXT/EPUB 排版分頁 --- */
static size_t scan_one_line_buf(const char *buf, size_t len, size_t start_off, int max_units, size_t *out_draw_len) {
    size_t pos = start_off;
    int units = 0;
    while (pos < len) {
        char ch = buf[pos];
        if (ch == '\r') {
            *out_draw_len = pos - start_off;
            pos++;
            if (pos < len && buf[pos] == '\n') pos++;
            return pos - start_off;
        }
        if (ch == '\n') {
            *out_draw_len = pos - start_off;
            return (pos - start_off) + 1;
        }
        int u = 0;
        int clen = utf8_char_info((const unsigned char *)(buf + pos), len - pos, &u);
        if (units + u > max_units && pos > start_off) {
            *out_draw_len = pos - start_off;
            return pos - start_off;
        }
        units += u;
        pos += (size_t)clen;
    }
    *out_draw_len = pos - start_off;
    return pos - start_off;
}

static void get_layout_params(int *out_max_units, int *out_lines_per_page, int *out_line_h) {
    int w = hal_get_width(), h = hal_get_height();
    int pad_x = 28, top_bar = 36, bot_bar = 38;
    int line_h = FONT_H + SPACING_PX[g_spacing_idx];
    int usable_w = w - pad_x * 2;
    if (usable_w < 160) usable_w = 160;
    int max_units = usable_w / 9;
    if (max_units < 14) max_units = 14;
    int lines = (h - top_bar - bot_bar - 18) / line_h;
    if (lines < 1) lines = 1;
    *out_max_units = max_units; *out_lines_per_page = lines; *out_line_h = line_h;
}

static void jump_to_offset(size_t target_offset) {
    g_current_page = 0;
    for (int i = 0; i < g_page_count; i++) {
        if (g_page_offsets[i] <= target_offset) g_current_page = i;
        else break;
    }
}

static int find_page_for_offset(size_t off) {
    int pg = 0;
    for (int i = 0; i < g_page_count; i++) {
        if (g_page_offsets[i] <= off) pg = i;
        else break;
    }
    return pg;
}

static void repaginate(size_t target_offset) {
    if (!g_doc.text) return;
    int max_units, lines_per_page, line_h;
    get_layout_params(&max_units, &lines_per_page, &line_h);

    free(g_page_offsets);
    g_page_offsets = NULL; g_page_count = 0;
    int cap = 128;
    g_page_offsets = (size_t *)malloc((size_t)cap * sizeof(size_t));
    if (!g_page_offsets) return;

    size_t pos = 0;
    while (pos < g_doc.text_len || g_page_count == 0) {
        if (g_page_count >= cap) {
            cap *= 2;
            size_t *tmp = (size_t *)realloc(g_page_offsets, (size_t)cap * sizeof(size_t));
            if (!tmp) break;
            g_page_offsets = tmp;
        }
        g_page_offsets[g_page_count++] = pos;
        for (int line = 0; line < lines_per_page && pos < g_doc.text_len; line++) {
            size_t draw_len = 0;
            size_t step = scan_one_line_buf(g_doc.text, g_doc.text_len, pos, max_units, &draw_len);
            if (step == 0) break;
            pos += step;
        }
        if (pos >= g_doc.text_len) break;
    }
    jump_to_offset(target_offset);
}

static const char *get_current_chapter_name(void) {
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) return "";
    if (g_doc.chapter_count <= 0 || !g_page_offsets || g_page_count <= 0) return "";
    size_t cur_off = g_page_offsets[g_current_page];
    const char *best = g_doc.chapters[0].title;
    for (int i = 0; i < g_doc.chapter_count; i++) {
        if (g_doc.chapters[i].offset <= cur_off) best = g_doc.chapters[i].title;
        else break;
    }
    return best;
}

/* --- 全文搜尋引擎 (同時支援 EPUB / TXT / PDF) --- */
static const char *find_ci_str(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) j++;
        if (j == nlen) return hay + i;
    }
    return NULL;
}

static void run_fulltext_search(void) {
    g_search_cnt = 0;
    g_search_sel = 0;
    g_search_scroll = 0;
    g_search_ran = 1;
    if (!g_search_query[0]) return;

    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf);
        for (int p = 0; p < tot && g_search_cnt < MAX_SEARCH_HITS; p++) {
            size_t tlen = 0;
            char *pt = pdf_extract_page_text(g_pdf, p, &tlen);
            if (pt && tlen > 0) {
                const char *hit = find_ci_str(pt, tlen, g_search_query);
                if (hit) {
                    SearchHit *sh = &g_search_hits[g_search_cnt++];
                    sh->pos_val = (size_t)p;
                    sh->page_num = p + 1;
                    size_t off = (size_t)(hit - pt);
                    size_t ctx_s = (off > 12) ? (off - 12) : 0;
                    while (ctx_s < off && ((unsigned char)pt[ctx_s] & 0xC0) == 0x80) ctx_s++;
                    char raw[96]; size_t rlen = tlen - ctx_s; if (rlen > 78) rlen = 78;
                    memcpy(raw, pt + ctx_s, rlen); raw[rlen] = '\0';
                    char clean[96]; sanitize_field(clean, sizeof(clean), raw);
                    utf8_ellipsis(clean, 36, sh->snippet, sizeof(sh->snippet));
                }
            }
            free(pt);
        }
        return;
    }

    if (g_doc.text && g_doc.text_len > 0) {
        size_t pos = 0;
        size_t qlen = strlen(g_search_query);
        while (pos < g_doc.text_len && g_search_cnt < MAX_SEARCH_HITS) {
            const char *hit = find_ci_str(g_doc.text + pos, g_doc.text_len - pos, g_search_query);
            if (!hit) break;
            size_t off = (size_t)(hit - g_doc.text);
            SearchHit *sh = &g_search_hits[g_search_cnt++];
            sh->pos_val = off;
            sh->page_num = find_page_for_offset(off) + 1;

            size_t ctx_s = (off > 12) ? (off - 12) : 0;
            while (ctx_s < off && ((unsigned char)g_doc.text[ctx_s] & 0xC0) == 0x80) ctx_s++;
            char raw[96]; size_t rlen = g_doc.text_len - ctx_s; if (rlen > 78) rlen = 78;
            memcpy(raw, g_doc.text + ctx_s, rlen); raw[rlen] = '\0';
            char clean[96]; sanitize_field(clean, sizeof(clean), raw);
            utf8_ellipsis(clean, 36, sh->snippet, sizeof(sh->snippet));

            pos = off + qlen;
        }
    }
}

static void show_loading_card(const char *filepath) {
    const AppTheme *th = &THEMES[g_theme_idx];
    int w = hal_get_width(), h = hal_get_height();
    BookFormat fmt = book_detect_format(filepath);
    int cw = 390, ch = 126, cx = (w - cw) / 2, cy = (h - ch) / 2;

    ui_card(cx, cy, cw, ch, 12, th->card_bg, th->card_border, th->shadow, 1, ui_format_color(fmt));
    ui_draw_badge(cx + 20, cy + 20, book_format_tag(fmt), ui_format_color(fmt), 0xFFFFFF);
    hal_set_text_bg(th->card_bg);
    char msg[128], fn[80];
    utf8_ellipsis(get_filename_only(filepath), 28, fn, sizeof(fn));
    snprintf(msg, sizeof(msg), "正在開啟 %s 引擎...", book_format_tag(fmt));
    draw_str(cx + 76, cy + 18, msg, cw - 92, th->text);
    draw_str(cx + 20, cy + 54, fn, cw - 40, th->muted);
    ui_progress_bar(cx + 20, cy + 92, cw - 40, 8, 65, th->track, ui_format_color(fmt));
    hal_present();
}

static void clamp_pdf_pan(void) {
    int vw = hal_get_width(), vh = hal_get_height() - 74;
    int max_x = g_pdf_last_pw - vw; if (max_x < 0) max_x = 0;
    int max_y = g_pdf_last_ph - vh; if (max_y < 0) max_y = 0;
    if (g_pdf_pan_x < 0) g_pdf_pan_x = 0;
    if (g_pdf_pan_x > max_x) g_pdf_pan_x = max_x;
    if (g_pdf_pan_y < 0) g_pdf_pan_y = 0;
    if (g_pdf_pan_y > max_y) g_pdf_pan_y = max_y;
}

/* --- 公開介面 --- */
void reader_init(void) {
    memset(&g_doc, 0, sizeof(g_doc));
    g_pdf = NULL;
    load_library();
    g_screen = SCR_HOME;
    g_home_tab = TAB_SHELF;
    g_toast_msg[0] = '\0';
}

void reader_shutdown(void) {
    save_bookmark();
    save_manual_bookmarks();
    save_library();
    pdf_close(g_pdf); g_pdf = NULL;
    free(g_pdf_reflow_text); g_pdf_reflow_text = NULL;
    book_doc_free(&g_doc);
    free(g_page_offsets); g_page_offsets = NULL;
}

int reader_open_file(const char *filepath) {
    if (!filepath || !filepath[0]) return 0;

    if (g_screen != SCR_HOME) {
        save_bookmark();
        save_manual_bookmarks();
    }

    show_loading_card(filepath);

    BookFormat fmt = book_detect_format(filepath);
    if (fmt == BOOK_FMT_PDF) {
        PdfDoc *nd = pdf_open(filepath);
        if (!nd) {
            snprintf(g_toast_msg, sizeof(g_toast_msg), "無法開啟 PDF：%s", get_filename_only(filepath));
            return 0;
        }
        pdf_close(g_pdf);
        g_pdf = nd;
        free(g_pdf_reflow_text); g_pdf_reflow_text = NULL; g_pdf_reflow_page_cached = -1;
        book_doc_free(&g_doc);
        g_doc.format = BOOK_FMT_PDF;
        const char *pt = pdf_get_title(g_pdf);
        utf8_ellipsis((pt && pt[0]) ? pt : get_filename_only(filepath), 44, g_doc.title, sizeof(g_doc.title));
        sanitize_field(g_doc.author, sizeof(g_doc.author), pdf_get_author(g_pdf));
        /* 建立每頁 PDF 目錄項目 */
        int pcnt = pdf_page_count(g_pdf);
        int cap = pcnt > BOOK_MAX_CHAPTERS ? BOOK_MAX_CHAPTERS : pcnt;
        for (int i = 0; i < cap; i++) {
            g_doc.chapters[i].offset = (size_t)i;
            snprintf(g_doc.chapters[i].title, sizeof(g_doc.chapters[i].title), "PDF 第 %d 頁", i + 1);
        }
        g_doc.chapter_count = cap;

        snprintf(g_filepath, sizeof(g_filepath), "%s", filepath);
        g_toast_msg[0] = '\0';
        g_pdf_zoom = 0; g_pdf_pan_x = 0; g_pdf_pan_y = 0; g_pdf_reflow = 0;
        size_t saved_pg = load_bookmark(g_filepath);
        g_pdf_page = ((int)saved_pg >= 0 && (int)saved_pg < pcnt) ? (int)saved_pg : 0;
        load_manual_bookmarks(g_filepath);
        g_screen = SCR_READ_PDF;
        save_bookmark();
        return 1;
    }

    BookDoc next_doc;
    if (!book_load_document(filepath, &next_doc)) {
        snprintf(g_toast_msg, sizeof(g_toast_msg), "無法開啟檔案：%s", get_filename_only(filepath));
        return 0;
    }

    pdf_close(g_pdf); g_pdf = NULL;
    free(g_pdf_reflow_text); g_pdf_reflow_text = NULL; g_pdf_reflow_page_cached = -1;
    book_doc_free(&g_doc);
    g_doc = next_doc;
    snprintf(g_filepath, sizeof(g_filepath), "%s", filepath);
    g_toast_msg[0] = '\0';
    load_manual_bookmarks(g_filepath);

    if (g_doc.format == BOOK_FMT_BMP) {
        free(g_page_offsets); g_page_offsets = NULL; g_page_count = 0;
        g_screen = SCR_READ_BMP;
        save_bookmark();
        return 1;
    }

    g_screen = SCR_READ_TEXT;
    size_t bmk = load_bookmark(g_filepath);
    repaginate(bmk);
    save_bookmark();
    return 1;
}

void reader_on_resize(void) {
    if (g_doc.text && g_page_offsets && g_page_count > 0) {
        repaginate(g_page_offsets[g_current_page]);
    }
}

static void open_new_book_dialog(void) {
    char path[384] = {0};
    if (hal_open_file_dialog(path, sizeof(path))) reader_open_file(path);
}

static void open_toc_screen(void) {
    g_toc_sel = 0;
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        g_toc_sel = (g_pdf_page < g_doc.chapter_count) ? g_pdf_page : 0;
    } else if (g_page_offsets && g_page_count > 0) {
        size_t cur_off = g_page_offsets[g_current_page];
        for (int i = 0; i < g_doc.chapter_count; i++) {
            if (g_doc.chapters[i].offset <= cur_off) g_toc_sel = i;
            else break;
        }
    }
    g_toc_scroll = 0;
    g_screen = SCR_TOC;
}

/* ============================================================
 * 按鍵與字元事件處理
 * ============================================================ */
void reader_on_key(ReaderKey key, char raw_char) {
    int is_ok = (key == RKEY_OK || key == RKEY_SPACE);

    /* 0. 全文搜尋畫面 (優先接收實體鍵盤打字!) */
    if (g_screen == SCR_SEARCH) {
        if (key == RKEY_BACK) { g_screen = active_read_screen(); return; }
        if (key == RKEY_BACKSPACE) {
            size_t n = strlen(g_search_query);
            if (n > 0) {
                /* 安全刪除一個 UTF-8 字元 */
                while (n > 0 && ((unsigned char)g_search_query[n - 1] & 0xC0) == 0x80) n--;
                if (n > 0) n--;
                g_search_query[n] = '\0';
                g_search_ran = 0;
            }
            return;
        }
        /* 快速中文常用關鍵字 (僅當搜尋欄為空時按 1~4 快速填入) */
        if (g_search_query[0] == '\0' && (key == RKEY_TAB1 || key == RKEY_TAB2 || key == RKEY_TAB3 || key == RKEY_TAB4)) {
            const char *presets[4] = { "第一章", "序章", "結論", "重點" };
            snprintf(g_search_query, sizeof(g_search_query), "%s", presets[key - RKEY_TAB1]);
            run_fulltext_search();
            return;
        }
        if (key == RKEY_UP) { if (g_search_sel > 0) g_search_sel--; return; }
        if (key == RKEY_DOWN) { if (g_search_sel + 1 < g_search_cnt) g_search_sel++; return; }
        if (key == RKEY_OK) {
            if (!g_search_ran) {
                run_fulltext_search();
            } else if (g_search_cnt > 0) {
                SearchHit *sh = &g_search_hits[g_search_sel];
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
                    g_pdf_page = (int)sh->pos_val; g_pdf_pan_x = 0; g_pdf_pan_y = 0;
                } else {
                    jump_to_offset(sh->pos_val);
                }
                save_bookmark();
                g_screen = active_read_screen();
            } else {
                run_fulltext_search();
            }
            return;
        }
        /* 直接接收鍵盤字元輸入 (A-Z, a-z, 0-9, Space, - _ .) */
        if (raw_char >= 0x20 && raw_char < 0x7F) {
            size_t n = strlen(g_search_query);
            if (n + 1 < sizeof(g_search_query)) {
                g_search_query[n] = raw_char;
                g_search_query[n + 1] = '\0';
                g_search_ran = 0;
            }
        }
        return;
    }

    /* 1. 多組手動書籤管理畫面 */
    if (g_screen == SCR_BOOKMARKS) {
        if (key == RKEY_BACK || key == RKEY_BOOKMARK) { g_screen = active_read_screen(); return; }
        if (key == RKEY_UP || key == RKEY_LEFT) { if (g_bmark_sel > 0) g_bmark_sel--; return; }
        if (key == RKEY_DOWN || key == RKEY_RIGHT) { if (g_bmark_sel + 1 < g_bmark_count) g_bmark_sel++; return; }
        if (key == RKEY_SPACE || key == RKEY_FONT_UP) {
            add_current_manual_bookmark();
            return;
        }
        if (key == RKEY_DELETE && g_bmark_count > 0) {
            for (int i = g_bmark_sel; i + 1 < g_bmark_count; i++) g_bmarks[i] = g_bmarks[i + 1];
            g_bmark_count--;
            if (g_bmark_sel >= g_bmark_count && g_bmark_count > 0) g_bmark_sel = g_bmark_count - 1;
            save_manual_bookmarks();
            return;
        }
        if (key == RKEY_OK) {
            if (g_bmark_count == 0) {
                add_current_manual_bookmark();
            } else {
                ManualBookmark *bm = &g_bmarks[g_bmark_sel];
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
                    int p = (int)bm->pos_val;
                    if (p >= 0 && p < pdf_page_count(g_pdf)) { g_pdf_page = p; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                } else {
                    jump_to_offset(bm->pos_val);
                }
                save_bookmark();
                g_screen = active_read_screen();
            }
        }
        return;
    }

    /* 2. 現代化首頁 */
    if (g_screen == SCR_HOME) {
        if (key == RKEY_TAB1) { g_home_tab = TAB_SHELF; return; }
        if (key == RKEY_TAB2) { g_home_tab = TAB_MANAGE; return; }
        if (key == RKEY_TAB3) { g_home_tab = TAB_APPEARANCE; return; }
        if (key == RKEY_TAB4 || key == RKEY_OPEN) { open_new_book_dialog(); return; }
        if (key == RKEY_THEME) { g_theme_idx = (g_theme_idx + 1) % 4; save_library(); return; }
        if (key == RKEY_SPACING) { g_spacing_idx = (g_spacing_idx + 1) % 3; save_library(); return; }
        if (key == RKEY_BACK) { hal_request_quit(); return; }

        if (g_home_tab == TAB_APPEARANCE) {
            if (key == RKEY_UP || key == RKEY_DOWN) g_appear_row = 1 - g_appear_row;
            else if (key == RKEY_LEFT) {
                if (g_appear_row == 0) g_theme_idx = (g_theme_idx + 3) % 4;
                else g_spacing_idx = (g_spacing_idx + 2) % 3;
                save_library();
            } else if (key == RKEY_RIGHT || is_ok) {
                if (g_appear_row == 0) g_theme_idx = (g_theme_idx + 1) % 4;
                else g_spacing_idx = (g_spacing_idx + 1) % 3;
                save_library();
            }
            return;
        }

        if (g_book_count == 0) { if (is_ok) open_new_book_dialog(); return; }

        if (g_home_tab == TAB_SHELF) {
            if (key == RKEY_LEFT) { if (g_shelf_sel > 0) g_shelf_sel--; }
            else if (key == RKEY_RIGHT) { if (g_shelf_sel + 1 < g_book_count) g_shelf_sel++; }
            else if (key == RKEY_UP) {
                if (g_shelf_sel >= 5) g_shelf_sel -= 4;
                else if (g_shelf_sel >= 1) g_shelf_sel = 0;
            } else if (key == RKEY_DOWN) {
                if (g_shelf_sel == 0 && g_book_count > 1) g_shelf_sel = 1;
                else if (g_shelf_sel + 4 < g_book_count) g_shelf_sel += 4;
                else if (g_shelf_sel + 1 < g_book_count) g_shelf_sel = g_book_count - 1;
            } else if (key == RKEY_PGUP) { g_shelf_sel = (g_shelf_sel >= 4) ? (g_shelf_sel - 4) : 0; }
            else if (key == RKEY_PGDN) { g_shelf_sel = (g_shelf_sel + 4 < g_book_count) ? (g_shelf_sel + 4) : (g_book_count - 1); }
            else if (key == RKEY_DELETE) remove_selected_book();
            else if (is_ok) {
                char target[384]; snprintf(target, sizeof(target), "%s", g_books[g_shelf_sel].path);
                reader_open_file(target);
            }
            return;
        }

        if (g_home_tab == TAB_MANAGE) {
            if (key == RKEY_UP || key == RKEY_LEFT || key == RKEY_PGUP) { if (g_shelf_sel > 0) g_shelf_sel--; }
            else if (key == RKEY_DOWN || key == RKEY_RIGHT || key == RKEY_PGDN) { if (g_shelf_sel + 1 < g_book_count) g_shelf_sel++; }
            else if (key == RKEY_DELETE) remove_selected_book();
            else if (is_ok) {
                char target[384]; snprintf(target, sizeof(target), "%s", g_books[g_shelf_sel].path);
                reader_open_file(target);
            }
            return;
        }
        return;
    }

    /* 3. 閱讀控制面板 (ESC / M) */
    if (g_screen == SCR_READ_MENU) {
        if (key == RKEY_BACK || key == RKEY_MENU) { g_screen = active_read_screen(); return; }
        if (key == RKEY_UP) { g_read_menu_sel = (g_read_menu_sel + 7) % 8; return; }
        if (key == RKEY_DOWN) { g_read_menu_sel = (g_read_menu_sel + 1) % 8; return; }
        if (key == RKEY_LEFT || key == RKEY_RIGHT) {
            int d = (key == RKEY_RIGHT) ? 1 : -1;
            if (g_read_menu_sel == 4) { g_theme_idx = (g_theme_idx + 4 + d) % 4; save_library(); }
            else if (g_read_menu_sel == 5) {
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
                    g_pdf_zoom = (g_pdf_zoom + 5 + d) % 5; g_pdf_pan_x = 0; g_pdf_pan_y = 0;
                } else {
                    g_spacing_idx = (g_spacing_idx + 3 + d) % 3; save_library(); reader_on_resize();
                }
            }
            return;
        }
        if (is_ok) {
            switch (g_read_menu_sel) {
                case 0: g_screen = active_read_screen(); break;
                case 1: open_toc_screen(); break;
                case 2: g_screen = SCR_BOOKMARKS; break;
                case 3: g_search_ran = 0; g_screen = SCR_SEARCH; break;
                case 4: g_theme_idx = (g_theme_idx + 1) % 4; save_library(); break;
                case 5:
                    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
                        g_pdf_zoom = (g_pdf_zoom + 1) % 5; g_pdf_pan_x = 0; g_pdf_pan_y = 0;
                    } else {
                        g_spacing_idx = (g_spacing_idx + 1) % 3; save_library(); reader_on_resize();
                    }
                    break;
                case 6:
                    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
                        g_jump_target = g_pdf_page; g_screen = SCR_JUMP;
                    } else if (g_page_count > 0) {
                        g_jump_target = g_current_page; g_screen = SCR_JUMP;
                    }
                    break;
                case 7:
                    save_bookmark(); save_manual_bookmarks();
                    g_screen = SCR_HOME;
                    break;
            }
        }
        return;
    }

    /* 4. 目錄 */
    if (g_screen == SCR_TOC) {
        if (key == RKEY_BACK || key == RKEY_TOC) { g_screen = active_read_screen(); return; }
        if (key == RKEY_UP || key == RKEY_LEFT) { if (g_toc_sel > 0) g_toc_sel--; return; }
        if (key == RKEY_DOWN || key == RKEY_RIGHT) { if (g_toc_sel + 1 < g_doc.chapter_count) g_toc_sel++; return; }
        if (key == RKEY_PGUP) { g_toc_sel = (g_toc_sel >= 8) ? (g_toc_sel - 8) : 0; return; }
        if (key == RKEY_PGDN) {
            g_toc_sel += 8;
            if (g_toc_sel >= g_doc.chapter_count) g_toc_sel = g_doc.chapter_count > 0 ? g_doc.chapter_count - 1 : 0;
            return;
        }
        if (is_ok) {
            if (g_doc.chapter_count > 0) {
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
                    g_pdf_page = (int)g_doc.chapters[g_toc_sel].offset;
                    g_pdf_pan_x = 0; g_pdf_pan_y = 0;
                } else {
                    jump_to_offset(g_doc.chapters[g_toc_sel].offset);
                }
                save_bookmark();
            }
            g_screen = active_read_screen();
        }
        return;
    }

    /* 5. 視覺跳頁滑桿 */
    if (g_screen == SCR_JUMP) {
        int tot = (g_doc.format == BOOK_FMT_PDF && g_pdf) ? pdf_page_count(g_pdf) : g_page_count;
        if (key == RKEY_BACK || key == RKEY_JUMP) { g_screen = active_read_screen(); return; }
        if (key == RKEY_LEFT)  g_jump_target -= 10;
        else if (key == RKEY_RIGHT) g_jump_target += 10;
        else if (key == RKEY_UP)    g_jump_target -= 50;
        else if (key == RKEY_DOWN)  g_jump_target += 50;
        else if (key == RKEY_PGUP)  g_jump_target -= 1;
        else if (key == RKEY_PGDN)  g_jump_target += 1;
        else if (is_ok) {
            if (g_jump_target >= 0 && g_jump_target < tot) {
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) { g_pdf_page = g_jump_target; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                else g_current_page = g_jump_target;
                save_bookmark();
            }
            g_screen = active_read_screen();
            return;
        }
        if (g_jump_target < 0) g_jump_target = 0;
        if (g_jump_target >= tot) g_jump_target = tot > 0 ? tot - 1 : 0;
        return;
    }

    /* 6. 真 PDF 視覺閱讀模式 (原版面渲染 + 縮放 + 平移 + 夜間反白 + 可選單頁重排) */
    if (g_screen == SCR_READ_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf);
        int vw = hal_get_width(), vh = hal_get_height() - 74;
        int can_pan_x = (!g_pdf_reflow && g_pdf_last_pw > vw);
        int can_pan_y = (!g_pdf_reflow && g_pdf_last_ph > vh);

        switch (key) {
            case RKEY_PGUP:
                if (g_pdf_page > 0) { g_pdf_page--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; save_bookmark(); }
                break;
            case RKEY_PGDN:
            case RKEY_SPACE:
                if (g_pdf_page + 1 < tot) { g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; save_bookmark(); }
                break;
            case RKEY_LEFT:
                if (can_pan_x && g_pdf_pan_x > 0) { g_pdf_pan_x -= 90; clamp_pdf_pan(); }
                else if (g_pdf_page > 0) { g_pdf_page--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; save_bookmark(); }
                break;
            case RKEY_RIGHT:
                if (can_pan_x && g_pdf_pan_x < g_pdf_last_pw - vw) { g_pdf_pan_x += 90; clamp_pdf_pan(); }
                else if (g_pdf_page + 1 < tot) { g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; save_bookmark(); }
                break;
            case RKEY_UP:
                if (can_pan_y && g_pdf_pan_y > 0) { g_pdf_pan_y -= 90; clamp_pdf_pan(); }
                else if (g_pdf_page > 0) { g_pdf_page--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; save_bookmark(); }
                break;
            case RKEY_DOWN:
                if (can_pan_y && g_pdf_pan_y < g_pdf_last_ph - vh) { g_pdf_pan_y += 90; clamp_pdf_pan(); }
                else if (g_pdf_page + 1 < tot) { g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; save_bookmark(); }
                break;
            case RKEY_FONT_UP:
                if (g_pdf_zoom < 4) { g_pdf_zoom++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                break;
            case RKEY_FONT_DOWN:
                if (g_pdf_zoom > 0) { g_pdf_zoom--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                break;
            case RKEY_REFLOW:
                g_pdf_reflow = !g_pdf_reflow;
                break;
            case RKEY_THEME:
                g_theme_idx = (g_theme_idx + 1) % 4; save_library();
                break;
            case RKEY_TOC: open_toc_screen(); break;
            case RKEY_JUMP: g_jump_target = g_pdf_page; g_screen = SCR_JUMP; break;
            case RKEY_BOOKMARK: g_screen = SCR_BOOKMARKS; break;
            case RKEY_SEARCH: g_search_ran = 0; g_screen = SCR_SEARCH; break;
            case RKEY_OPEN: open_new_book_dialog(); break;
            case RKEY_SHELF:
            case RKEY_BACKSPACE: save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; break;
            case RKEY_BACK:
            case RKEY_MENU: g_read_menu_sel = 0; g_screen = SCR_READ_MENU; break;
            default: break;
        }
        return;
    }

    /* 7. BMP 圖片書模式 */
    if (g_screen == SCR_READ_BMP) {
        if (key == RKEY_LEFT || key == RKEY_UP || key == RKEY_PGUP) {
            char np[384] = {0};
            if (hal_find_sibling_bmp(g_filepath, -1, np, sizeof(np))) {
                snprintf(g_filepath, sizeof(g_filepath), "%s", np);
                utf8_ellipsis(get_filename_only(np), 44, g_doc.title, sizeof(g_doc.title));
            }
        } else if (key == RKEY_RIGHT || key == RKEY_DOWN || key == RKEY_PGDN || key == RKEY_SPACE) {
            char np[384] = {0};
            if (hal_find_sibling_bmp(g_filepath, 1, np, sizeof(np))) {
                snprintf(g_filepath, sizeof(g_filepath), "%s", np);
                utf8_ellipsis(get_filename_only(np), 44, g_doc.title, sizeof(g_doc.title));
            }
        } else if (key == RKEY_THEME) { g_theme_idx = (g_theme_idx + 1) % 4; save_library(); }
        else if (key == RKEY_OPEN) open_new_book_dialog();
        else if (key == RKEY_SHELF || key == RKEY_BACKSPACE) { save_bookmark(); g_screen = SCR_HOME; }
        else if (key == RKEY_BACK || key == RKEY_MENU) { g_read_menu_sel = 0; g_screen = SCR_READ_MENU; }
        return;
    }

    /* 8. EPUB / TXT 閱讀模式 */
    switch (key) {
        case RKEY_LEFT:
        case RKEY_UP:
        case RKEY_PGUP:
            if (g_current_page > 0) { g_current_page--; save_bookmark(); }
            break;
        case RKEY_RIGHT:
        case RKEY_DOWN:
        case RKEY_PGDN:
        case RKEY_SPACE:
            if (g_current_page + 1 < g_page_count) { g_current_page++; save_bookmark(); }
            break;
        case RKEY_TOC: open_toc_screen(); break;
        case RKEY_JUMP: if (g_page_count > 0) { g_jump_target = g_current_page; g_screen = SCR_JUMP; } break;
        case RKEY_BOOKMARK: g_screen = SCR_BOOKMARKS; break;
        case RKEY_SEARCH: g_search_ran = 0; g_screen = SCR_SEARCH; break;
        case RKEY_THEME: g_theme_idx = (g_theme_idx + 1) % 4; save_library(); break;
        case RKEY_SPACING: g_spacing_idx = (g_spacing_idx + 1) % 3; save_library(); reader_on_resize(); break;
        case RKEY_OPEN: open_new_book_dialog(); break;
        case RKEY_SHELF:
        case RKEY_BACKSPACE: save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; break;
        case RKEY_BACK:
        case RKEY_MENU: g_read_menu_sel = 0; g_screen = SCR_READ_MENU; break;
        default: break;
    }
}

/* ============================================================
 * 現代化 App 畫面繪製
 * ============================================================ */
static int calc_book_pct(const BookEntry *be) {
    if (be->page_count > 0) return ((be->page_idx + 1) * 100) / be->page_count;
    if (be->total_bytes > 0) return (int)((be->byte_offset * 100) / be->total_bytes);
    return 0;
}

static void render_left_rail(const AppTheme *th, int h) {
    int rw = 76;
    hal_fill_rect(0, 0, rw, h, th->rail_bg);
    hal_fill_rect(rw, 0, 1, h, th->card_border);

    ui_fill_round_rect(18, 16, 40, 40, 10, th->accent);
    ui_draw_ascii(32, 27, "R", 2, 0xFFFFFF);

    const char *labels[4] = { "書架", "管理", "外觀", "匯入" };
    for (int i = 0; i < 4; i++) {
        int ty = 78 + i * 74;
        int active = (i < 3 && (int)g_home_tab == i);
        uint32_t bg = active ? th->card_bg : th->rail_bg;
        uint32_t fg = active ? th->accent : th->muted;
        if (active) {
            ui_fill_round_rect(8, ty, rw - 16, 62, 10, bg);
            hal_fill_rect(0, ty + 14, 4, 34, th->accent);
        }
        if (i == 0) ui_icon_shelf(rw / 2, ty + 20, fg);
        else if (i == 1) ui_icon_list(rw / 2, ty + 20, fg);
        else if (i == 2) ui_icon_palette(rw / 2, ty + 20, fg);
        else ui_icon_import(rw / 2, ty + 20, fg);

        hal_set_text_bg(bg);
        draw_str(20, ty + 38, labels[i], 44, active ? th->text : th->muted);
        char kb[4]; snprintf(kb, sizeof(kb), "%d", i + 1);
        ui_draw_ascii(rw - 16, ty + 5, kb, 1, th->muted);
    }
    ui_draw_ascii(18, h - 24, "V4.0", 1, th->muted);
}

static void render_home_shelf(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    hal_set_text_bg(th->bg);
    char hdr[96]; snprintf(hdr, sizeof(hdr), "我的現代書房  (%d 本)", g_book_count);
    draw_str(mx, 18, hdr, 260, th->text);

    int bx = w - 184;
    ui_fill_round_rect(bx, 12, 166, 32, 16, th->accent);
    hal_set_text_bg(th->accent);
    draw_str(bx + 18, 19, "[O] 匯入 EPUB/PDF", 140, 0xFFFFFF);

    if (g_toast_msg[0]) {
        hal_set_text_bg(th->bg);
        draw_str(mx + 220, 18, g_toast_msg, 240, 0xEF4444);
    }

    if (g_book_count == 0) {
        int cw = mw - 40, ch = h - 108, cx = mx + 20, cy = 62;
        ui_card(cx, cy, cw, ch, 16, th->card_bg, th->card_border, th->shadow, 0, th->accent);
        ui_icon_open_book(cx + cw / 2, cy + 80, th->accent, th->bg, th->card_border);

        int px = cx + cw / 2 - 118;
        ui_draw_badge(px,       cy + 128, "EPUB", ui_format_color(BOOK_FMT_EPUB), 0xFFFFFF);
        ui_draw_badge(px + 60,  cy + 128, "PDF",  ui_format_color(BOOK_FMT_PDF),  0xFFFFFF);
        ui_draw_badge(px + 114, cy + 128, "TXT",  ui_format_color(BOOK_FMT_TXT),  0xFFFFFF);
        ui_draw_badge(px + 168, cy + 128, "BMP",  ui_format_color(BOOK_FMT_BMP),  0xFFFFFF);

        hal_set_text_bg(th->card_bg);
        draw_str(cx + cw / 2 - 130, cy + 162, "真正原生閱讀 EPUB 與 PDF 原版面", 320, th->text);
        draw_str(cx + cw / 2 - 200, cy + 192, "支援 PDF 圖片/排版縮放平移、書籤、全文搜尋、4種護眼主題", 420, th->muted);

        int btn_w = 240, btn_h = 42, btn_x = cx + (cw - btn_w) / 2, btn_y = cy + 242;
        ui_fill_round_rect(btn_x, btn_y, btn_w, btn_h, 21, th->accent);
        hal_set_text_bg(th->accent);
        draw_str(btn_x + 30, btn_y + 12, "[Enter] 匯入第一本電子書", 200, 0xFFFFFF);
        return;
    }

    /* 上方 Hero Card */
    BookEntry *hero = &g_books[0];
    int hero_y = 56, hero_h = 114;
    ui_card(mx, hero_y, mw, hero_h, 14, th->card_bg, th->card_border, th->shadow, (g_shelf_sel == 0), th->accent);
    ui_draw_book_cover(mx + 14, hero_y + 12, 72, 90, hero->title, hero->format, th->is_dark);

    int tx = mx + 102, tw = mw - 240;
    hal_set_text_bg(th->card_bg);
    char htitle[96]; utf8_ellipsis(hero->title, tw / 9, htitle, sizeof(htitle));
    draw_str(tx, hero_y + 16, htitle, tw, th->text);

    char sub[128];
    if (hero->author[0]) snprintf(sub, sizeof(sub), "上次閱讀 · %s · %s", hero->author, book_format_tag(hero->format));
    else snprintf(sub, sizeof(sub), "上次閱讀 · %s %s", book_format_tag(hero->format), hero->format == BOOK_FMT_PDF ? "原版面文件" : "電子書");
    draw_str(tx, hero_y + 42, sub, tw, th->muted);

    int hpct = calc_book_pct(hero);
    ui_progress_bar(tx, hero_y + 72, tw - 12, 8, hpct, th->track, th->accent);

    char pinfo[96];
    if (hero->page_count > 0) snprintf(pinfo, sizeof(pinfo), "已讀 %d%%  (第 %d / %d 頁)", hpct, hero->page_idx + 1, hero->page_count);
    else snprintf(pinfo, sizeof(pinfo), "圖片書模式 / 準備就緒");
    draw_str(tx, hero_y + 86, pinfo, tw, th->muted);

    int cta_x = mx + mw - 126, cta_y = hero_y + 39;
    uint32_t cta_bg = (g_shelf_sel == 0) ? th->accent : th->track;
    uint32_t cta_fg = (g_shelf_sel == 0) ? 0xFFFFFF : th->text;
    ui_fill_round_rect(cta_x, cta_y, 110, 36, 18, cta_bg);
    hal_set_text_bg(cta_bg);
    draw_str(cta_x + 18, cta_y + 9, "繼續閱讀", 80, cta_fg);

    /* 下方 4 欄視覺書封網格 */
    hal_set_text_bg(th->bg);
    draw_str(mx, 182, "全部書籍  (↑↓←→ 選書 · [Enter] 閱讀 · [D] 移出)", mw, th->muted);

    int grid_y = 208, cols = 4, gap = 14;
    int card_w = (mw - gap * (cols - 1)) / cols;
    int card_h = h - grid_y - 34;
    if (card_h > 236) card_h = 236;

    int start_idx = (g_book_count == 1) ? 0 : ((g_shelf_sel >= 5) ? (1 + ((g_shelf_sel - 1) / 4) * 4) : 1);
    for (int col = 0; col < cols; col++) {
        int idx = start_idx + col;
        if (idx >= g_book_count) break;
        BookEntry *be = &g_books[idx];
        int cx = mx + col * (card_w + gap);
        ui_card(cx, grid_y, card_w, card_h, 12, th->card_bg, th->card_border, th->shadow, (idx == g_shelf_sel), th->accent);

        int cov_h = card_h - 82; if (cov_h < 68) cov_h = 68;
        ui_draw_book_cover(cx + 10, grid_y + 10, card_w - 20, cov_h, be->title, be->format, th->is_dark);

        hal_set_text_bg(th->card_bg);
        char stitle[64]; utf8_ellipsis(be->title, (card_w - 20) / 9, stitle, sizeof(stitle));
        draw_str(cx + 10, grid_y + cov_h + 18, stitle, card_w - 20, th->text);

        int pct = calc_book_pct(be);
        ui_progress_bar(cx + 10, grid_y + card_h - 32, card_w - 20, 6, pct, th->track, ui_format_color(be->format));

        char ptxt[24]; snprintf(ptxt, sizeof(ptxt), "%d%%", pct);
        ui_draw_ascii(cx + 10, grid_y + card_h - 18, ptxt, 1, th->muted);
        if (be->page_count > 0) {
            char pg[24]; snprintf(pg, sizeof(pg), "%d/%d", be->page_idx + 1, be->page_count);
            ui_draw_ascii(cx + card_w - ui_ascii_width(pg, 1) - 10, grid_y + card_h - 18, pg, 1, th->muted);
        }
    }

    hal_set_text_bg(th->bg);
    draw_str(mx, h - 24, "[1]書架  [2]管理  [3]外觀主題  [4/O]匯入新書  [ESC]離開", mw, th->muted);
}

static void render_home_manage(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    hal_set_text_bg(th->bg);
    draw_str(mx, 18, "書庫管理  ([Enter] 閱讀 · [D] 移出紀錄 · [O] 匯入)", mw, th->text);
    if (g_book_count == 0) { draw_str(mx, 80, "目前沒有任何書籍紀錄，按 [O] 匯入新書。", mw, th->muted); return; }

    int row_h = 68, top_y = 54, visible = (h - top_y - 36) / row_h;
    if (visible < 1) visible = 1;
    if (g_shelf_sel < g_manage_scroll) g_manage_scroll = g_shelf_sel;
    if (g_shelf_sel >= g_manage_scroll + visible) g_manage_scroll = g_shelf_sel - visible + 1;

    for (int i = g_manage_scroll; i < g_book_count && i < g_manage_scroll + visible; i++) {
        BookEntry *be = &g_books[i];
        int ry = top_y + (i - g_manage_scroll) * row_h;
        ui_card(mx, ry, mw, row_h - 8, 10, th->card_bg, th->card_border, th->shadow, (i == g_shelf_sel), th->accent);
        ui_draw_badge(mx + 12, ry + 12, book_format_tag(be->format), ui_format_color(be->format), 0xFFFFFF);

        hal_set_text_bg(th->card_bg);
        char tbuf[96]; utf8_ellipsis(be->title, (mw - 180) / 9, tbuf, sizeof(tbuf));
        draw_str(mx + 70, ry + 10, tbuf, mw - 180, th->text);
        char pbuf[128]; utf8_ellipsis(be->path, (mw - 100) / 9, pbuf, sizeof(pbuf));
        draw_str(mx + 14, ry + 34, pbuf, mw - 110, th->muted);

        int pct = calc_book_pct(be);
        ui_progress_bar(mx + mw - 100, ry + 22, 84, 7, pct, th->track, th->accent);
        char pc[16]; snprintf(pc, sizeof(pc), "%d%%", pct);
        ui_draw_ascii(mx + mw - 42, ry + 36, pc, 1, th->muted);
    }
    hal_set_text_bg(th->bg);
    draw_str(mx, h - 24, "[1]書架  [2]管理  [3]外觀主題  [4/O]匯入新書  [ESC]離開", mw, th->muted);
}

static void render_home_appearance(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    hal_set_text_bg(th->bg);
    draw_str(mx, 18, "外觀與閱讀排版  (↑↓ 切項目 · ←→ 即時切換)", mw, th->text);
    draw_str(mx, 54, "選擇閱讀主題配色：", mw, (g_appear_row == 0) ? th->accent : th->muted);

    int gap = 14, cw = (mw - gap * 3) / 4, ch = 152, cy = 80;
    for (int i = 0; i < 4; i++) {
        const AppTheme *t = &THEMES[i];
        int cx = mx + i * (cw + gap);
        int is_cur = (i == g_theme_idx);
        ui_card(cx, cy, cw, ch, 12, t->bg, t->card_border, th->shadow, is_cur, t->accent);
        ui_fill_round_rect(cx + 12, cy + 14, cw - 24, 10, 4, t->bar_bg);
        for (int ln = 0; ln < 4; ln++) {
            int lw = (ln == 3) ? ((cw - 24) * 6 / 10) : (cw - 24);
            hal_fill_rect(cx + 12, cy + 34 + ln * 12, lw, 4, ui_blend_color(t->bg, t->text, 65));
        }
        ui_progress_bar(cx + 12, cy + 88, cw - 24, 5, 65, t->track, t->accent);
        hal_set_text_bg(t->bg);
        draw_str(cx + 12, cy + 106, t->name, cw - 24, t->text);
        if (is_cur) ui_draw_badge(cx + 12, cy + 128, "ACTIVE", t->accent, 0xFFFFFF);
        else ui_draw_ascii(cx + 12, cy + 131, t->eng, 1, t->muted);
    }

    hal_set_text_bg(th->bg);
    draw_str(mx, 250, "選擇正文行距：", mw, (g_appear_row == 1) ? th->accent : th->muted);
    for (int i = 0; i < 3; i++) {
        int px = mx + i * 140;
        int is_cur = (i == g_spacing_idx);
        uint32_t pbg = is_cur ? th->accent : th->card_bg;
        uint32_t pfg = is_cur ? 0xFFFFFF : th->text;
        ui_card(px, 276, 126, 38, 19, pbg, th->card_border, th->shadow, (g_appear_row == 1 && is_cur), th->accent);
        hal_set_text_bg(pbg);
        draw_str(px + 42, 286, SPACING_NAMES[i], 64, pfg);
    }

    int py = 330, ph = h - py - 34;
    ui_card(mx, py, mw, ph, 12, th->card_bg, th->card_border, th->shadow, 0, th->accent);
    hal_set_text_bg(th->card_bg);
    int lh = FONT_H + SPACING_PX[g_spacing_idx];
    draw_str(mx + 18, py + 14, "排版預覽：原生解析 EPUB 與真原版面 PDF，", mw - 36, th->text);
    if (14 + lh + FONT_H < ph) draw_str(mx + 18, py + 14 + lh, "支援書籤、全文搜尋、縮放平移與 4 種現代護眼主題。", mw - 36, th->text);

    hal_set_text_bg(th->bg);
    draw_str(mx, h - 24, "[1]書架  [2]管理  [3]外觀主題  [4/O]匯入新書  [ESC]離開", mw, th->muted);
}

/* --- 真 PDF 視覺頁面繪製 (原版面紙張 + 圖片 + 向量 + 定位字，或單頁重排) --- */
static void render_pdf_base(const AppTheme *th, int w, int h) {
    int top_bar = 36, bot_bar = 38;
    int tot = pdf_page_count(g_pdf);
    int pct = (tot > 0) ? (((g_pdf_page + 1) * 100) / tot) : 0;

    hal_clear(th->bg);

    /* 頂部 PDF 專用工具列 */
    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    hal_fill_rect(0, top_bar, w, 1, th->card_border);
    ui_draw_badge(14, 10, "PDF", ui_format_color(BOOK_FMT_PDF), 0xFFFFFF);

    hal_set_text_bg(th->bar_bg);
    char tbuf[72]; utf8_ellipsis(g_doc.title, 26, tbuf, sizeof(tbuf));
    draw_str(60, 9, tbuf, 260, th->text);

    char mode_tag[96];
    if (g_pdf_reflow) snprintf(mode_tag, sizeof(mode_tag), "單頁重排模式  ([R] 切回 PDF 原版面)");
    else snprintf(mode_tag, sizeof(mode_tag), "原版面視圖 · 縮放:%s  ([+/-]縮放 [R]重排)", PDF_ZOOM_NAMES[g_pdf_zoom]);
    draw_str(w - 390, 9, mode_tag, 376, th->accent);

    /* 底部進度與快捷列 */
    int by = h - bot_bar;
    hal_fill_rect(0, by, w, bot_bar, th->bar_bg);
    hal_fill_rect(0, by, w, 3, th->track);
    if (pct > 0) hal_fill_rect(0, by, (w * pct) / 100, 3, ui_format_color(BOOK_FMT_PDF));

    hal_set_text_bg(th->bar_bg);
    char foot_l[96];
    snprintf(foot_l, sizeof(foot_l), "PDF 第 %d / %d 頁 (%d%%)", g_pdf_page + 1, tot, pct);
    draw_str(18, by + 11, foot_l, 260, th->text);
    draw_str(w - 430, by + 11, "[PgUpDn]翻頁 [↑↓←→]平移 [K]書籤 [F]搜尋 [ESC]選單", 418, th->muted);

    /* 渲染主視區 */
    if (!g_pdf_reflow) {
        uint32_t backdrop = th->is_dark ? 0x0C0F14 : 0xDDD8CE;
        pdf_render_page(g_pdf, g_pdf_page, 0, top_bar + 1, w, h - top_bar - bot_bar - 1,
                        g_pdf_zoom, g_pdf_pan_x, g_pdf_pan_y, backdrop, th->is_dark,
                        &g_pdf_last_pw, &g_pdf_last_ph);
    } else {
        if (g_pdf_reflow_page_cached != g_pdf_page) {
            free(g_pdf_reflow_text);
            g_pdf_reflow_text = pdf_extract_page_text(g_pdf, g_pdf_page, &g_pdf_reflow_len);
            g_pdf_reflow_page_cached = g_pdf_page;
        }
        int max_units, lines_per_page, line_h;
        get_layout_params(&max_units, &lines_per_page, &line_h);
        hal_set_text_bg(th->bg);
        if (!g_pdf_reflow_text || g_pdf_reflow_len == 0) {
            draw_str(32, top_bar + 32, "此頁為純圖片/掃描 PDF 頁面，請按 [R] 切回「PDF 原版面視圖」直接看圖片！", w - 64, th->accent);
        } else {
            size_t pos = 0; int y = top_bar + 12;
            for (int line = 0; line < lines_per_page && pos < g_pdf_reflow_len; line++) {
                size_t draw_len = 0;
                size_t step = scan_one_line_buf(g_pdf_reflow_text, g_pdf_reflow_len, pos, max_units, &draw_len);
                if (draw_len > 0) hal_draw_line_utf8(28, y, g_pdf_reflow_text + pos, (int)draw_len, w - 56, th->text);
                y += line_h;
                if (step == 0) break;
                pos += step;
            }
        }
    }
}

static void render_reading_base(const AppTheme *th, int w, int h) {
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        render_pdf_base(th, w, h);
        return;
    }

    int top_bar = 36, bot_bar = 38;
    hal_clear(th->bg);

    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    hal_fill_rect(0, top_bar, w, 1, th->card_border);
    ui_draw_badge(14, 10, book_format_tag(g_doc.format), ui_format_color(g_doc.format), 0xFFFFFF);

    hal_set_text_bg(th->bar_bg);
    char tbuf[80]; utf8_ellipsis(g_doc.title[0] ? g_doc.title : get_filename_only(g_filepath), 30, tbuf, sizeof(tbuf));
    draw_str(68, 9, tbuf, 300, th->text);

    const char *ch_name = get_current_chapter_name();
    if (ch_name && ch_name[0]) {
        char cbuf[64]; utf8_ellipsis(ch_name, 26, cbuf, sizeof(cbuf));
        draw_str(w - 260, 9, cbuf, 246, th->muted);
    }

    int by = h - bot_bar;
    hal_fill_rect(0, by, w, bot_bar, th->bar_bg);
    int pct = (g_page_count > 0) ? (((g_current_page + 1) * 100) / g_page_count) : 0;
    hal_fill_rect(0, by, w, 3, th->track);
    if (pct > 0) hal_fill_rect(0, by, (w * pct) / 100, 3, th->accent);

    hal_set_text_bg(th->bar_bg);
    char foot_l[96];
    if (g_doc.format == BOOK_FMT_BMP) snprintf(foot_l, sizeof(foot_l), "圖片書 · [←→] 自動翻上下張");
    else if (g_page_count > 0) snprintf(foot_l, sizeof(foot_l), "第 %d / %d 頁 (%d%%)", g_current_page + 1, g_page_count, pct);
    else snprintf(foot_l, sizeof(foot_l), "準備就緒");
    draw_str(18, by + 11, foot_l, 260, th->text);
    draw_str(w - 430, by + 11, "[C]目錄  [K]書籤  [F]搜尋  [G]跳頁  [T]主題  [ESC]選單", 418, th->muted);

    if (g_doc.format == BOOK_FMT_BMP) {
        hal_draw_bmp_file(g_filepath, 0, top_bar + 1, w, h - top_bar - bot_bar - 1);
        return;
    }

    if (g_doc.text && g_page_offsets && g_page_count > 0) {
        int max_units, lines_per_page, line_h;
        get_layout_params(&max_units, &lines_per_page, &line_h);
        hal_set_text_bg(th->bg);
        size_t pos = g_page_offsets[g_current_page];
        int y = top_bar + 12;
        for (int line = 0; line < lines_per_page && pos < g_doc.text_len; line++) {
            size_t draw_len = 0;
            size_t step = scan_one_line_buf(g_doc.text, g_doc.text_len, pos, max_units, &draw_len);
            if (draw_len > 0) hal_draw_line_utf8(28, y, g_doc.text + pos, (int)draw_len, w - 56, th->text);
            y += line_h;
            if (step == 0) break;
            pos += step;
        }
    }
}

static void render_read_menu_overlay(const AppTheme *th, int w, int h) {
    int mw = 440, mh = 376, mx = (w - mw) / 2, my = (h - mh) / 2;
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 42, 12, th->bar_bg);
    hal_fill_rect(mx + 2, my + 42, mw - 4, 1, th->card_border);

    hal_set_text_bg(th->bar_bg);
    draw_str(mx + 20, my + 12, "閱讀控制面板  (←→ 可調主題與縮放/行距)", mw - 40, th->text);

    char items[8][64];
    snprintf(items[0], sizeof(items[0]), "繼續閱讀  (ESC)");
    snprintf(items[1], sizeof(items[1]), "章節 / 頁面目錄  (共 %d 項)", g_doc.chapter_count);
    snprintf(items[2], sizeof(items[2]), "書籤管理  (已存 %d 筆 · [K])", g_bmark_count);
    snprintf(items[3], sizeof(items[3]), "全文關鍵字搜尋  ([F])");
    snprintf(items[4], sizeof(items[4]), "主題配色：%s", th->name);
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        snprintf(items[5], sizeof(items[5]), "PDF 縮放比例：%s", PDF_ZOOM_NAMES[g_pdf_zoom]);
    } else {
        snprintf(items[5], sizeof(items[5]), "正文行距：%s", SPACING_NAMES[g_spacing_idx]);
    }
    snprintf(items[6], sizeof(items[6]), "快速跳頁滑桿  ([G])");
    snprintf(items[7], sizeof(items[7]), "存檔並返回現代書架");

    for (int i = 0; i < 8; i++) {
        int ry = my + 52 + i * 39;
        int is_sel = (i == g_read_menu_sel);
        uint32_t rbg = is_sel ? th->accent : th->card_bg;
        uint32_t rfg = is_sel ? 0xFFFFFF : th->text;
        if (is_sel) ui_fill_round_rect(mx + 14, ry, mw - 28, 34, 8, rbg);
        hal_set_text_bg(rbg);
        draw_str(mx + 28, ry + 8, items[i], mw - 120, rfg);

        if (i == 4) {
            for (int t = 0; t < 4; t++) {
                int sx = mx + mw - 102 + t * 22, sy = ry + 17;
                hal_fill_circle(sx, sy, 7, THEMES[t].bg);
                hal_draw_circle(sx, sy, 7, (t == g_theme_idx) ? (is_sel ? 0xFFFFFF : th->accent) : th->card_border);
            }
        }
    }
}

static void render_bookmarks_overlay(const AppTheme *th, int w, int h) {
    int mw = 540, mh = 360, mx = (w - mw) / 2, my = (h - mh) / 2;
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 44, 12, th->bar_bg);
    hal_fill_rect(mx + 2, my + 44, mw - 4, 1, th->card_border);

    hal_set_text_bg(th->bar_bg);
    char hdr[96]; snprintf(hdr, sizeof(hdr), "我的書籤 (%d / %d)  ·  [空白鍵/+] 新增本頁書籤", g_bmark_count, MAX_BOOKMARKS);
    draw_str(mx + 18, my + 13, hdr, mw - 36, th->text);

    if (g_bmark_count == 0) {
        hal_set_text_bg(th->card_bg);
        draw_str(mx + 32, my + 110, "這本書還沒有手動書籤。", mw - 64, th->text);
        draw_str(mx + 32, my + 144, "按 [Enter] 或 [空白鍵] 立即把目前這頁加入書籤！", mw - 64, th->accent);
    } else {
        int row_h = 38;
        int start = (g_bmark_sel >= 7) ? (g_bmark_sel - 6) : 0;
        for (int i = start; i < g_bmark_count && i < start + 7; i++) {
            int ry = my + 56 + (i - start) * row_h;
            int is_sel = (i == g_bmark_sel);
            uint32_t rbg = is_sel ? th->accent : th->bg;
            uint32_t rfg = is_sel ? 0xFFFFFF : th->text;
            ui_fill_round_rect(mx + 16, ry, mw - 32, 32, 8, rbg);
            hal_set_text_bg(rbg);
            char line[128];
            snprintf(line, sizeof(line), "第%d頁 (%d%%)  %s", g_bmarks[i].page_num, g_bmarks[i].pct, g_bmarks[i].label);
            draw_str(mx + 28, ry + 7, line, mw - 56, rfg);
        }
    }

    hal_set_text_bg(th->card_bg);
    draw_str(mx + 18, my + mh - 28, "[Enter]跳至書籤  [空白/+]新增本頁  [D]刪除  [ESC]返回", mw - 36, th->muted);
}

static void render_search_overlay(const AppTheme *th, int w, int h) {
    int mw = 580, mh = 380, mx = (w - mw) / 2, my = (h - mh) / 2;
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 44, 12, th->bar_bg);
    hal_fill_rect(mx + 2, my + 44, mw - 4, 1, th->card_border);

    hal_set_text_bg(th->bar_bg);
    draw_str(mx + 18, my + 13, "全文關鍵字搜尋  (直接鍵盤打字 + Enter 搜尋 · ESC 返回)", mw - 36, th->text);

    /* 現代圓角搜尋輸入框 */
    ui_fill_round_rect(mx + 18, my + 56, mw - 36, 36, 10, th->bg);
    ui_draw_round_rect(mx + 18, my + 56, mw - 36, 36, 10, th->accent);
    hal_set_text_bg(th->bg);
    char qdisp[96];
    if (g_search_query[0]) snprintf(qdisp, sizeof(qdisp), "搜尋：%s_", g_search_query);
    else snprintf(qdisp, sizeof(qdisp), "請直接輸入英文/數字，或按 [1]第一章 [2]序章 [3]結論 [4]重點");
    draw_str(mx + 30, my + 65, qdisp, mw - 60, g_search_query[0] ? th->text : th->muted);

    /* 搜尋結果清單 */
    int list_y = my + 104;
    if (!g_search_ran) {
        hal_set_text_bg(th->card_bg);
        draw_str(mx + 26, list_y + 20, "輸入關鍵字後按 [Enter] 開始全文檢索（支援 EPUB / PDF / TXT）。", mw - 52, th->muted);
    } else if (g_search_cnt == 0) {
        hal_set_text_bg(th->card_bg);
        draw_str(mx + 26, list_y + 20, "找不到符合的內容，請按 [Backspace] 清除後重試。", mw - 52, 0xEF4444);
    } else {
        int visible = 6, row_h = 38;
        if (g_search_sel < g_search_scroll) g_search_scroll = g_search_sel;
        if (g_search_sel >= g_search_scroll + visible) g_search_scroll = g_search_sel - visible + 1;

        for (int i = g_search_scroll; i < g_search_cnt && i < g_search_scroll + visible; i++) {
            int ry = list_y + (i - g_search_scroll) * row_h;
            int is_sel = (i == g_search_sel);
            uint32_t rbg = is_sel ? th->accent : th->bg;
            uint32_t rfg = is_sel ? 0xFFFFFF : th->text;
            ui_fill_round_rect(mx + 18, ry, mw - 36, 32, 8, rbg);
            hal_set_text_bg(rbg);
            char line[128];
            snprintf(line, sizeof(line), "[P.%d]  %s", g_search_hits[i].page_num, g_search_hits[i].snippet);
            draw_str(mx + 28, ry + 7, line, mw - 56, rfg);
        }
    }

    hal_set_text_bg(th->card_bg);
    char foot[96];
    if (g_search_ran) snprintf(foot, sizeof(foot), "共找到 %d 筆結果 · [↑↓]選擇  [Enter]立即跳轉  [Backspace]刪字", g_search_cnt);
    else snprintf(foot, sizeof(foot), "[Enter]開始搜尋  [Backspace]刪字  [ESC]關閉");
    draw_str(mx + 18, my + mh - 28, foot, mw - 36, th->muted);
}

static void render_toc_page(const AppTheme *th, int w, int h) {
    hal_clear(th->bg);
    int top_bar = 44, bot_bar = 34;
    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    hal_fill_rect(0, top_bar, w, 1, th->card_border);
    hal_set_text_bg(th->bar_bg);
    char hdr[128]; snprintf(hdr, sizeof(hdr), "目錄  ·  %s  (共 %d 項)", g_doc.title, g_doc.chapter_count);
    draw_str(22, 12, hdr, w - 44, th->text);

    hal_fill_rect(0, h - bot_bar, w, bot_bar, th->bar_bg);
    hal_set_text_bg(th->bar_bg);
    draw_str(22, h - bot_bar + 8, "[↑↓] 選擇   [PgUp/PgDn] 快捲   [Enter] 跳轉   [ESC] 返回", w - 44, th->muted);

    if (g_doc.chapter_count == 0) {
        ui_card(80, 110, w - 160, 140, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent);
        hal_set_text_bg(th->card_bg);
        draw_str(110, 148, "本書未偵測到章節標題，請按 [G] 使用跳頁滑桿。", w - 220, th->text);
        return;
    }

    int row_h = 38, visible = (h - top_bar - bot_bar - 20) / row_h;
    if (visible < 1) visible = 1;
    if (g_toc_sel < g_toc_scroll) g_toc_scroll = g_toc_sel;
    if (g_toc_sel >= g_toc_scroll + visible) g_toc_scroll = g_toc_sel - visible + 1;

    for (int i = g_toc_scroll; i < g_doc.chapter_count && i < g_toc_scroll + visible; i++) {
        int ry = top_bar + 12 + (i - g_toc_scroll) * row_h;
        int is_sel = (i == g_toc_sel);
        uint32_t rbg = is_sel ? th->accent : th->card_bg;
        uint32_t rfg = is_sel ? 0xFFFFFF : th->text;
        ui_card(24, ry, w - 48, row_h - 6, 8, rbg, th->card_border, th->shadow, is_sel, th->accent);
        hal_set_text_bg(rbg);
        char line[120], ctitle[80];
        utf8_ellipsis(g_doc.chapters[i].title, (w - 130) / 9, ctitle, sizeof(ctitle));
        snprintf(line, sizeof(line), "%02d.  %s", i + 1, ctitle);
        draw_str(40, ry + 7, line, w - 96, rfg);
    }
}

static void render_jump_overlay(const AppTheme *th, int w, int h) {
    int tot = (g_doc.format == BOOK_FMT_PDF && g_pdf) ? pdf_page_count(g_pdf) : g_page_count;
    int bw = 460, bh = 152, bx = (w - bw) / 2, by = (h - bh) / 2;
    ui_card(bx, by, bw, bh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    hal_set_text_bg(th->card_bg);

    int pct = (tot > 0) ? (((g_jump_target + 1) * 100) / tot) : 0;
    char t1[96]; snprintf(t1, sizeof(t1), "快速跳頁：第 %d / %d 頁  (%d%%)", g_jump_target + 1, tot, pct);
    draw_str(bx + 24, by + 20, t1, bw - 48, th->text);

    int sx = bx + 24, sy = by + 64, sw = bw - 48;
    ui_progress_bar(sx, sy, sw, 10, pct, th->track, th->accent);
    int kx = sx + (sw * pct) / 100;
    hal_fill_circle(kx, sy + 5, 10, th->accent);
    hal_fill_circle(kx, sy + 5, 5, 0xFFFFFF);

    draw_str(bx + 24, by + 98, "[←→] ±10頁   [↑↓] ±50頁   [PgUp/Dn] ±1頁", bw - 48, th->muted);
    draw_str(bx + 24, by + 122, "[Enter] 確認跳轉   [ESC] 取消", bw - 48, th->accent);
}

void reader_render(void) {
    const AppTheme *th = &THEMES[g_theme_idx];
    int w = hal_get_width(), h = hal_get_height();

    if (g_screen == SCR_HOME) {
        hal_clear(th->bg);
        render_left_rail(th, h);
        if (g_home_tab == TAB_SHELF) render_home_shelf(th, w, h);
        else if (g_home_tab == TAB_MANAGE) render_home_manage(th, w, h);
        else render_home_appearance(th, w, h);
        hal_present();
        return;
    }

    if (g_screen == SCR_TOC) {
        render_toc_page(th, w, h);
        hal_present();
        return;
    }

    render_reading_base(th, w, h);

    if (g_screen == SCR_READ_MENU) render_read_menu_overlay(th, w, h);
    else if (g_screen == SCR_JUMP) render_jump_overlay(th, w, h);
    else if (g_screen == SCR_BOOKMARKS) render_bookmarks_overlay(th, w, h);
    else if (g_screen == SCR_SEARCH) render_search_overlay(th, w, h);

    hal_present();
}
