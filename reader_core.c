#include "reader_core.h"
#include "book_loader.h"
#include "pdf_engine.h"
#include "stats.h"
#include "ui_gfx.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * CD-952 現代化觸控電子書閱讀器 v6
 *  - EPUB 真封面縮圖快取 + 內嵌插圖 (<img/image>) 原生顯示
 *  - 頂部即時系統時鐘 + 全年 365 天每日閱讀熱力圖 (53週×7天) + 連續閱讀天數 + 單書計時
 *  - 完整保留 v5 全部功能：真視覺 PDF、原生 PNG/BMP/TXT、觸控點擊、彈窗防重疊、多書籤、全文搜尋、4種現代主題
 * ============================================================ */

typedef enum {
    SCR_HOME = 0,
    SCR_READ_TEXT,
    SCR_READ_PDF,
    SCR_READ_IMG,
    SCR_READ_MENU,
    SCR_TOC,
    SCR_JUMP,
    SCR_BOOKMARKS,
    SCR_SEARCH,
    SCR_NEW_FOLDER,
    SCR_MOVE_FOLDER,
    SCR_NEXT_VOL
} ScreenMode;

typedef enum {
    TAB_SHELF = 0,
    TAB_MANAGE,
    TAB_STATS,
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
    { "象牙晨光", "IVORY", 0, 0xF4F1EA, 0xEAE5DB, 0xFFFFFF, 0xDDD6C8, 0xD3CCC0, 0xEFECE4, 0x1F242B, 0x6B7280, 0x2563EB, 0xE2DDD2 },
    { "護眼森綠", "SAGE",  0, 0xE3EFE5, 0xD4E5D7, 0xF3F9F4, 0xBBD2BF, 0xB0C7B4, 0xDBEADB, 0x1A2E20, 0x52725C, 0x059669, 0xC4DAC7 },
    { "柔和暖茶", "SEPIA", 0, 0xF2E7D5, 0xE7D8C0, 0xFAF4E8, 0xD9C7AB, 0xCCBA9D, 0xECE0CB, 0x3A291C, 0x7A624E, 0xC2410C, 0xDDD0BC },
    { "暗夜極黑", "NIGHT", 1, 0x111419, 0x171B22, 0x1C212A, 0x2B3340, 0x090B0E, 0x161A21, 0xE2E8F0, 0x8A94A6, 0x3B82F6, 0x2A313D }
};

#define MAX_BOOKS       32
#define MAX_FOLDERS     16
#define MAX_BOOKMARKS   16
#define MAX_SEARCH_HITS 64
#define COVER_CACHE_N   6
#define ILLUST_CACHE_N  2

typedef struct {
    char path[384];
    char title[96];
    char author[64];
    BookFormat format;
    size_t byte_offset;
    size_t total_bytes;
    int page_idx;
    int page_count;
    uint32_t read_secs;
    char folder[48];
} BookEntry;

typedef struct {
    size_t pos_val;
    int page_num;
    int pct;
    char label[72];
} ManualBookmark;

typedef struct {
    size_t pos_val;
    int page_num;
    char snippet[84];
} SearchHit;

typedef struct {
    char path[384];
    int tried;
    int w, h;
    uint8_t *rgb;
    unsigned stamp;
} CoverCacheSlot;

typedef struct {
    int img_id;
    int w, h;
    uint8_t *rgb;
    unsigned stamp;
} IllustCacheSlot;

static ScreenMode g_screen = SCR_HOME;
static HomeTab g_home_tab = TAB_SHELF;
static int g_theme_idx = 0;
static int g_spacing_idx = 1;
static const int SPACING_PX[3] = { 5, 9, 14 };
static const char *SPACING_NAMES[3] = { "緊湊", "標準", "寬鬆" };
static const char *PDF_ZOOM_NAMES[5] = { "適應整頁", "適應寬度", "135%", "175%", "225%" };
static const int FONT_H = 18;

static BookEntry g_books[MAX_BOOKS];
static int g_book_count = 0;
static int g_shelf_sel = 0;

/* 書架資料夾系統 */
static char g_folders[MAX_FOLDERS][48];
static int  g_folder_count = 0;
static char g_cur_folder[48] = {0};     /* 空字串 = 外層主書架 */
static char g_new_folder_buf[48] = {0};
static int  g_move_book_idx = -1;
static int  g_move_folder_sel = 0;
static int  g_next_vol_idx = -1;
static char g_pending_import_folder[48] = {0};
static int g_manage_scroll = 0;
static int g_appear_row = 0;

/* 365 天年度統計畫面游標 */
static int g_stats_year = 2026;
static int g_stats_doy = 0;
static int g_last_clock_min = -1;
static int g_unsaved_secs = 0;

/* EPUB 真封面與插圖 LRU 快取 */
static CoverCacheSlot  g_cov_cache[COVER_CACHE_N];
static unsigned        g_cov_clock = 0;
static IllustCacheSlot g_ill_cache[ILLUST_CACHE_N];
static unsigned        g_ill_clock = 0;

static char g_filepath[384] = {0};
static BookDoc g_doc;
static PdfDoc *g_pdf = NULL;

static size_t *g_page_offsets = NULL;
static int g_page_count = 0;
static int g_current_page = 0;

static int g_pdf_page = 0;
static int g_pdf_zoom = 0;
static int g_pdf_pan_x = 0;
static int g_pdf_pan_y = 0;
static int g_pdf_last_pw = 800;
static int g_pdf_last_ph = 400;
static int g_pdf_reflow = 0;
static char *g_pdf_reflow_text = NULL;
static size_t g_pdf_reflow_len = 0;
static int g_pdf_reflow_page_cached = -1;

static ManualBookmark g_bmarks[MAX_BOOKMARKS];
static int g_bmark_count = 0;
static int g_bmark_sel = 0;

static char g_search_query[64] = {0};
static SearchHit g_search_hits[MAX_SEARCH_HITS];
static int g_search_cnt = 0;
static int g_search_sel = 0;
static int g_search_scroll = 0;
static int g_search_ran = 0;

static int g_read_menu_sel = 0;
static int g_toc_sel = 0;
static int g_toc_scroll = 0;
static int g_jump_target = 0;
static char g_toast_msg[128] = {0};

static int is_image_fmt(BookFormat fmt) {
    return (fmt == BOOK_FMT_BMP || fmt == BOOK_FMT_PNG);
}

static void clear_illust_cache(void) {
    for (int i = 0; i < ILLUST_CACHE_N; i++) {
        free(g_ill_cache[i].rgb);
        g_ill_cache[i].rgb = NULL;
        g_ill_cache[i].img_id = -1;
    }
}

static void clear_cover_cache(void) {
    for (int i = 0; i < COVER_CACHE_N; i++) {
        free(g_cov_cache[i].rgb);
        memset(&g_cov_cache[i], 0, sizeof(g_cov_cache[i]));
    }
}

static void invalidate_cover_cache_for(const char *path) {
    for (int i = 0; i < COVER_CACHE_N; i++) {
        if (strcmp(g_cov_cache[i].path, path) == 0) {
            free(g_cov_cache[i].rgb);
            memset(&g_cov_cache[i], 0, sizeof(g_cov_cache[i]));
        }
    }
}

static const uint8_t *get_cached_cover(const char *path, int *out_w, int *out_h) {
    *out_w = 0; *out_h = 0;
    if (!path || !path[0]) return NULL;
    for (int i = 0; i < COVER_CACHE_N; i++) {
        if (g_cov_cache[i].tried && strcmp(g_cov_cache[i].path, path) == 0) {
            g_cov_cache[i].stamp = ++g_cov_clock;
            *out_w = g_cov_cache[i].w; *out_h = g_cov_cache[i].h;
            return g_cov_cache[i].rgb;
        }
    }
    int slot = 0;
    for (int i = 0; i < COVER_CACHE_N; i++) {
        if (!g_cov_cache[i].tried) { slot = i; break; }
        if (g_cov_cache[i].stamp < g_cov_cache[slot].stamp) slot = i;
    }
    free(g_cov_cache[slot].rgb);
    memset(&g_cov_cache[slot], 0, sizeof(g_cov_cache[slot]));
    snprintf(g_cov_cache[slot].path, sizeof(g_cov_cache[slot].path), "%s", path);
    g_cov_cache[slot].tried = 1;
    g_cov_cache[slot].stamp = ++g_cov_clock;
    g_cov_cache[slot].rgb = book_load_cover_thumb(path, &g_cov_cache[slot].w, &g_cov_cache[slot].h);
    *out_w = g_cov_cache[slot].w; *out_h = g_cov_cache[slot].h;
    return g_cov_cache[slot].rgb;
}

static const uint8_t *get_cached_illust(int img_id, int *out_w, int *out_h) {
    *out_w = 0; *out_h = 0;
    if (img_id < 0 || img_id >= g_doc.image_count) return NULL;
    for (int i = 0; i < ILLUST_CACHE_N; i++) {
        if (g_ill_cache[i].rgb && g_ill_cache[i].img_id == img_id) {
            g_ill_cache[i].stamp = ++g_ill_clock;
            *out_w = g_ill_cache[i].w; *out_h = g_ill_cache[i].h;
            return g_ill_cache[i].rgb;
        }
    }
    int slot = 0;
    for (int i = 0; i < ILLUST_CACHE_N; i++) {
        if (!g_ill_cache[i].rgb) { slot = i; break; }
        if (g_ill_cache[i].stamp < g_ill_cache[slot].stamp) slot = i;
    }
    free(g_ill_cache[slot].rgb);
    g_ill_cache[slot].rgb = book_extract_epub_image(g_filepath, g_doc.images[img_id].zip_path,
                                                    &g_ill_cache[slot].w, &g_ill_cache[slot].h);
    g_ill_cache[slot].img_id = g_ill_cache[slot].rgb ? img_id : -1;
    g_ill_cache[slot].stamp = ++g_ill_clock;
    *out_w = g_ill_cache[slot].w; *out_h = g_ill_cache[slot].h;
    return g_ill_cache[slot].rgb;
}

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
        if (c == '|' || c == '\r' || c == '\n' || c == '\x01') c = ' ';
        dst[o++] = c;
    }
    dst[o] = '\0';
}

static void format_duration_zh(uint32_t secs, char *out, size_t sz) {
    if (secs < 60u) {
        snprintf(out, sz, "%lu 秒", (unsigned long)secs);
    } else if (secs < 3600u) {
        snprintf(out, sz, "%lu 分鐘", (unsigned long)(secs / 60u));
    } else {
        unsigned long h = (unsigned long)(secs / 3600u);
        unsigned long m = (unsigned long)((secs % 3600u) / 60u);
        if (m > 0) snprintf(out, sz, "%lu 時 %lu 分", h, m);
        else snprintf(out, sz, "%lu 小時", h);
    }
}

static ScreenMode active_read_screen(void) {
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) return SCR_READ_PDF;
    if (is_image_fmt(g_doc.format)) return SCR_READ_IMG;
    return SCR_READ_TEXT;
}

static void record_page_turn_stat(void) {
    int y, m, d, hh, mm, ss;
    hal_get_local_time(&y, &m, &d, &hh, &mm, &ss);
    stats_add_reading(y, m, d, 0, 1);
}

/* --- 書庫索引讀寫 (v6 新增單書 read_secs，完全相容舊版) --- */
static void save_library(void) {
    const char *lib_path = hal_get_library_path();
    if (!lib_path || !lib_path[0]) return;
    FILE *f = fopen(lib_path, "wb");
    if (!f) return;
    fprintf(f, "#CFG7|%d|%d\n", g_theme_idx, g_spacing_idx);
    for (int i = 0; i < g_folder_count; i++) {
        char fn[48];
        sanitize_field(fn, sizeof(fn), g_folders[i]);
        if (fn[0]) fprintf(f, "#FLD|%s\n", fn);
    }
    for (int i = 0; i < g_book_count; i++) {
        char t[96], a[64], fld[48];
        sanitize_field(t, sizeof(t), g_books[i].title);
        sanitize_field(a, sizeof(a), g_books[i].author);
        sanitize_field(fld, sizeof(fld), g_books[i].folder);
        fprintf(f, "%d|%zu|%zu|%d|%d|%lu|%s|%s|%s|%s\n",
                (int)g_books[i].format, g_books[i].byte_offset, g_books[i].total_bytes,
                g_books[i].page_idx, g_books[i].page_count,
                (unsigned long)g_books[i].read_secs,
                t, a, fld, g_books[i].path);
    }
    fclose(f);
}

static void load_library(void) {
    g_book_count = 0;
    g_folder_count = 0;
    const char *lib_path = hal_get_library_path();
    if (!lib_path || !lib_path[0]) return;
    FILE *f = fopen(lib_path, "rb");
    if (!f) return;

    char line[768];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        if (strncmp(line, "#CFG", 4) == 0) {
            const char *bar = strchr(line, '|');
            int th = 0, sp = 1;
            if (bar && sscanf(bar + 1, "%d|%d", &th, &sp) == 2) {
                if (th >= 0 && th < 4) g_theme_idx = th;
                if (sp >= 0 && sp < 3) g_spacing_idx = sp;
            }
            continue;
        }
        if (strncmp(line, "#FLD|", 5) == 0) {
            if (g_folder_count < MAX_FOLDERS && line[5] != '\0') {
                utf8_ellipsis(line + 5, 22, g_folders[g_folder_count++], 48);
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
        char *p8 = strchr(p7 + 1, '|');
        char *p9 = p8 ? strchr(p8 + 1, '|') : NULL;

        BookEntry *be = &g_books[g_book_count++];
        memset(be, 0, sizeof(*be));
        be->format = (BookFormat)atoi(line);
        be->byte_offset = (size_t)strtoull(p1 + 1, NULL, 10);
        be->total_bytes = (size_t)strtoull(p2 + 1, NULL, 10);
        be->page_idx = atoi(p3 + 1);
        be->page_count = atoi(p4 + 1);

        char *t_start, *t_end, *a_start, *a_end, *path_start;
        if (p9) {
            be->read_secs = (uint32_t)strtoul(p5 + 1, NULL, 10);
            t_start = p6 + 1; t_end = p7;
            a_start = p7 + 1; a_end = p8;
            size_t flen = (size_t)(p9 - (p8 + 1));
            if (flen >= sizeof(be->folder)) flen = sizeof(be->folder) - 1;
            memcpy(be->folder, p8 + 1, flen); be->folder[flen] = '\0';
            path_start = p9 + 1;
        } else if (p8) {
            be->read_secs = (uint32_t)strtoul(p5 + 1, NULL, 10);
            t_start = p6 + 1; t_end = p7;
            a_start = p7 + 1; a_end = p8;
            path_start = p8 + 1;
        } else {
            t_start = p5 + 1; t_end = p6;
            a_start = p6 + 1; a_end = p7;
            path_start = p7 + 1;
        }

        size_t tlen = (size_t)(t_end - t_start);
        if (tlen >= sizeof(be->title)) tlen = sizeof(be->title) - 1;
        memcpy(be->title, t_start, tlen); be->title[tlen] = '\0';

        size_t alen = (size_t)(a_end - a_start);
        if (alen >= sizeof(be->author)) alen = sizeof(be->author) - 1;
        memcpy(be->author, a_start, alen); be->author[alen] = '\0';

        snprintf(be->path, sizeof(be->path), "%s", path_start);
        if (be->title[0] == '\0') utf8_ellipsis(get_filename_only(be->path), 40, be->title, sizeof(be->title));
    }
    fclose(f);
}

static void update_book_in_library(const char *path, BookFormat fmt, const char *title, const char *author,
                                   size_t boff, size_t btot, int pidx, int pcnt) {
    if (!path || !path[0]) return;
    int found = -1;
    uint32_t prev_secs = 0;
    char prev_folder[48] = {0};
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].path, path) == 0) {
            found = i;
            prev_secs = g_books[i].read_secs;
            snprintf(prev_folder, sizeof(prev_folder), "%s", g_books[i].folder);
            break;
        }
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
    cur.read_secs = prev_secs;
    if (prev_folder[0]) snprintf(cur.folder, sizeof(cur.folder), "%s", prev_folder);
    else if (g_pending_import_folder[0]) snprintf(cur.folder, sizeof(cur.folder), "%s", g_pending_import_folder);

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

static void remove_book_at(int idx) {
    if (g_book_count <= 0 || idx < 0 || idx >= g_book_count) return;
    invalidate_cover_cache_for(g_books[idx].path);
    for (int i = idx; i + 1 < g_book_count; i++) g_books[i] = g_books[i + 1];
    g_book_count--;
    if (g_shelf_sel >= g_book_count && g_book_count > 0) g_shelf_sel = g_book_count - 1;
    if (g_book_count == 0) { g_shelf_sel = 0; g_manage_scroll = 0; }
    save_library();
}

/* --- 書架資料夾管理與自動下一卷輔助 --- */
static int folder_find(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g_folder_count; i++) {
        if (strcmp(g_folders[i], name) == 0) return i;
    }
    return -1;
}

static int folder_create(const char *name) {
    if (!name || !name[0] || g_folder_count >= MAX_FOLDERS) return 0;
    char clean[48];
    sanitize_field(clean, sizeof(clean), name);
    if (!clean[0] || folder_find(clean) >= 0) return 0;
    snprintf(g_folders[g_folder_count++], 48, "%s", clean);
    save_library();
    return 1;
}

static void folder_delete(int fidx) {
    if (fidx < 0 || fidx >= g_folder_count) return;
    char target[48];
    snprintf(target, sizeof(target), "%s", g_folders[fidx]);
    /* 只解除分類，絕不刪除任何書籍檔案 */
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].folder, target) == 0) g_books[i].folder[0] = '\0';
    }
    for (int i = fidx; i + 1 < g_folder_count; i++) {
        snprintf(g_folders[i], 48, "%s", g_folders[i + 1]);
    }
    g_folder_count--;
    if (strcmp(g_cur_folder, target) == 0) g_cur_folder[0] = '\0';
    save_library();
}

static int folder_count_books(const char *name) {
    int c = 0;
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].folder, name) == 0) c++;
    }
    return c;
}

static int folder_first_book_idx(const char *name) {
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].folder, name) == 0) return i;
    }
    return -1;
}

/* 建立目前書架網格項目清單：type 1=資料夾, type 0=書籍 */
static int shelf_build_grid(int *out_types, int *out_indices, int max_n) {
    int cnt = 0;
    if (g_cur_folder[0] == '\0') {
        /* 外層主書架：先放全部資料夾，再放未分類的書 */
        for (int f = 0; f < g_folder_count && cnt < max_n; f++) {
            out_types[cnt] = 1;
            out_indices[cnt] = f;
            cnt++;
        }
        for (int b = 0; b < g_book_count && cnt < max_n; b++) {
            if (g_books[b].folder[0] == '\0') {
                out_types[cnt] = 0;
                out_indices[cnt] = b;
                cnt++;
            }
        }
    } else {
        /* 資料夾內部：列出此資料夾內的書，並依書名排序方便看系列(第1卷, 第2卷...) */
        for (int b = 0; b < g_book_count && cnt < max_n; b++) {
            if (strcmp(g_books[b].folder, g_cur_folder) == 0) {
                out_types[cnt] = 0;
                out_indices[cnt] = b;
                cnt++;
            }
        }
        for (int i = 0; i < cnt; i++) {
            for (int j = i + 1; j < cnt; j++) {
                if (strcmp(g_books[out_indices[i]].title, g_books[out_indices[j]].title) > 0) {
                    int tmp = out_indices[i];
                    out_indices[i] = out_indices[j];
                    out_indices[j] = tmp;
                }
            }
        }
    }
    return cnt;
}

/* 尋找同資料夾中的下一卷 (按書名排序) */
static int find_next_volume_in_folder(const char *cur_path) {
    int cur_bi = -1;
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].path, cur_path) == 0) { cur_bi = i; break; }
    }
    if (cur_bi < 0 || g_books[cur_bi].folder[0] == '\0') return -1;

    const char *fld = g_books[cur_bi].folder;
    int list[MAX_BOOKS], n = 0;
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].folder, fld) == 0) list[n++] = i;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(g_books[list[i]].title, g_books[list[j]].title) > 0) {
                int tmp = list[i]; list[i] = list[j]; list[j] = tmp;
            }
        }
    }
    for (int i = 0; i + 1 < n; i++) {
        if (list[i] == cur_bi) return list[i + 1];
    }
    return -1;
}

static void open_new_folder_modal(void) {
    g_new_folder_buf[0] = '\0';
    g_screen = SCR_NEW_FOLDER;
}

static void open_move_folder_modal(int book_idx) {
    if (book_idx < 0 || book_idx >= g_book_count) return;
    g_move_book_idx = book_idx;
    g_move_folder_sel = 0;
    if (g_books[book_idx].folder[0]) {
        int fi = folder_find(g_books[book_idx].folder);
        if (fi >= 0) g_move_folder_sel = fi + 1;
    }
    g_screen = SCR_MOVE_FOLDER;
}

static void save_manual_bookmarks(void) {
    if (!g_filepath[0]) return;
    char p[420]; snprintf(p, sizeof(p), "%s.bmk2", g_filepath);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    for (int i = 0; i < g_bmark_count; i++) {
        char lbl[72]; sanitize_field(lbl, sizeof(lbl), g_bmarks[i].label);
        fprintf(f, "%zu|%d|%d|%s\n", g_bmarks[i].pos_val, g_bmarks[i].page_num, g_bmarks[i].pct, lbl);
    }
    fclose(f);
}

static void load_manual_bookmarks(const char *filepath) {
    g_bmark_count = 0; g_bmark_sel = 0;
    if (!filepath || !filepath[0]) return;
    char p[420]; snprintf(p, sizeof(p), "%s.bmk2", filepath);
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
        } else snprintf(bm->label, sizeof(bm->label), "PDF 圖像頁 #%d", g_pdf_page + 1);
        free(pt);
    } else if (g_doc.text && g_page_offsets && g_page_count > 0) {
        size_t off = g_page_offsets[g_current_page];
        bm->pos_val = off;
        bm->page_num = g_current_page + 1;
        bm->pct = ((g_current_page + 1) * 100) / g_page_count;
        if (g_doc.text[off] == '\x01') {
            snprintf(bm->label, sizeof(bm->label), "【插圖頁 #%d】", g_current_page + 1);
        } else {
            char raw[80]; size_t rem = g_doc.text_len - off; if (rem > 70) rem = 70;
            memcpy(raw, g_doc.text + off, rem); raw[rem] = '\0';
            char clean[80]; sanitize_field(clean, sizeof(clean), raw);
            utf8_ellipsis(clean, 28, bm->label, sizeof(bm->label));
        }
    } else return;

    g_bmark_count++; g_bmark_sel = g_bmark_count - 1;
    save_manual_bookmarks();
}

static void save_bookmark(void) {
    if (!g_filepath[0]) return;
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf);
        char bmk_path[420]; snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", g_filepath);
        FILE *f = fopen(bmk_path, "wb");
        if (f) { fprintf(f, "%d", g_pdf_page); fclose(f); }
        update_book_in_library(g_filepath, BOOK_FMT_PDF, g_doc.title, g_doc.author,
                               (size_t)g_pdf_page, (size_t)tot, g_pdf_page, tot);
    } else if (is_image_fmt(g_doc.format)) {
        update_book_in_library(g_filepath, g_doc.format, g_doc.title, "", 0, 0, 0, 0);
    } else if (g_page_count > 0 && g_page_offsets) {
        size_t off = g_page_offsets[g_current_page];
        char bmk_path[420]; snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", g_filepath);
        FILE *f = fopen(bmk_path, "wb");
        if (f) { fprintf(f, "%zu", off); fclose(f); }
        update_book_in_library(g_filepath, g_doc.format, g_doc.title, g_doc.author,
                               off, g_doc.text_len, g_current_page, g_page_count);
    }
    stats_save(hal_get_stats_path());
}

static size_t load_bookmark(const char *path) {
    if (!path || !path[0]) return 0;
    char bmk_path[420]; snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", path);
    FILE *f = fopen(bmk_path, "rb");
    if (f) {
        size_t off = 0;
        if (fscanf(f, "%zu", &off) == 1) { fclose(f); return off; }
        fclose(f);
    }
    for (int i = 0; i < g_book_count; i++) if (strcmp(g_books[i].path, path) == 0) return g_books[i].byte_offset;
    return 0;
}

/* 檢查指定偏移是否為 EPUB 插圖標記 (\x01IMG:N) */
static int parse_img_marker(const char *buf, size_t len, size_t pos, int *out_img_id, size_t *out_step) {
    if (pos + 6 > len || buf[pos] != '\x01' || memcmp(buf + pos + 1, "IMG:", 4) != 0) return 0;
    int id = atoi(buf + pos + 5);
    size_t p = pos + 5;
    while (p < len && buf[p] != '\n' && buf[p] != '\r') p++;
    if (p < len && buf[p] == '\r') p++;
    if (p < len && buf[p] == '\n') p++;
    if (out_img_id) *out_img_id = id;
    if (out_step) *out_step = p - pos;
    return 1;
}

static size_t scan_one_line_buf(const char *buf, size_t len, size_t start_off, int max_units, size_t *out_draw_len) {
    size_t pos = start_off; int units = 0;
    while (pos < len) {
        char ch = buf[pos];
        if (ch == '\r') { *out_draw_len = pos - start_off; pos++; if (pos < len && buf[pos] == '\n') pos++; return pos - start_off; }
        if (ch == '\n') { *out_draw_len = pos - start_off; return (pos - start_off) + 1; }
        int u = 0;
        int clen = utf8_char_info((const unsigned char *)(buf + pos), len - pos, &u);
        if (units + u > max_units && pos > start_off) { *out_draw_len = pos - start_off; return pos - start_off; }
        units += u; pos += (size_t)clen;
    }
    *out_draw_len = pos - start_off;
    return pos - start_off;
}

static void get_layout_params(int *out_max_units, int *out_lines_per_page, int *out_line_h) {
    int w = hal_get_width(), h = hal_get_height();
    int pad_x = 28, top_bar = 36, bot_bar = 38;
    int line_h = FONT_H + SPACING_PX[g_spacing_idx];
    int usable_w = w - pad_x * 2; if (usable_w < 160) usable_w = 160;
    int max_units = usable_w / 9; if (max_units < 14) max_units = 14;
    int lines = (h - top_bar - bot_bar - 18) / line_h; if (lines < 1) lines = 1;
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
    for (int i = 0; i < g_page_count; i++) { if (g_page_offsets[i] <= off) pg = i; else break; }
    return pg;
}

/* 支援 EPUB 插圖專屬頁面的智慧分頁引擎 */
static void repaginate(size_t target_offset) {
    if (!g_doc.text) return;
    int max_units, lines_per_page, line_h;
    get_layout_params(&max_units, &lines_per_page, &line_h);

    free(g_page_offsets); g_page_offsets = NULL; g_page_count = 0;
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

        /* 若此頁起始即為插圖標記，獨立成為一張完整高清插圖頁！ */
        size_t img_step = 0;
        if (parse_img_marker(g_doc.text, g_doc.text_len, pos, NULL, &img_step)) {
            pos += img_step;
            if (pos >= g_doc.text_len) break;
            continue;
        }

        for (int line = 0; line < lines_per_page && pos < g_doc.text_len; line++) {
            /* 頁面中段遇到插圖標記 -> 提前換頁，讓下頁完整顯示插圖 */
            if (parse_img_marker(g_doc.text, g_doc.text_len, pos, NULL, NULL)) break;
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
    g_search_cnt = 0; g_search_sel = 0; g_search_scroll = 0; g_search_ran = 1;
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
                    sh->pos_val = (size_t)p; sh->page_num = p + 1;
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
        size_t pos = 0, qlen = strlen(g_search_query);
        while (pos < g_doc.text_len && g_search_cnt < MAX_SEARCH_HITS) {
            const char *hit = find_ci_str(g_doc.text + pos, g_doc.text_len - pos, g_search_query);
            if (!hit) break;
            size_t off = (size_t)(hit - g_doc.text);
            SearchHit *sh = &g_search_hits[g_search_cnt++];
            sh->pos_val = off; sh->page_num = find_page_for_offset(off) + 1;
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

    hal_clear_text_in_rect(cx - 8, cy - 6, cw + 16, ch + 12);
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

void reader_init(void) {
    memset(&g_doc, 0, sizeof(g_doc));
    g_pdf = NULL;
    load_library();
    stats_init(hal_get_stats_path());

    int y, m, d, hh, mm, ss;
    hal_get_local_time(&y, &m, &d, &hh, &mm, &ss);
    g_stats_year = y;
    g_stats_doy = stats_day_of_year(y, m, d);
    g_last_clock_min = mm;

    g_screen = SCR_HOME;
    g_home_tab = TAB_SHELF;
    g_toast_msg[0] = '\0';
}

void reader_shutdown(void) {
    save_bookmark();
    save_manual_bookmarks();
    save_library();
    stats_save(hal_get_stats_path());
    clear_illust_cache();
    clear_cover_cache();
    pdf_close(g_pdf); g_pdf = NULL;
    free(g_pdf_reflow_text); g_pdf_reflow_text = NULL;
    book_doc_free(&g_doc);
    free(g_page_offsets); g_page_offsets = NULL;
}

/* 每秒計時回呼：累計今日與單書閱讀時間；僅在分鐘變動時回傳 1 刷新頂部時鐘 */
int reader_tick_second(void) {
    int y, m, d, hh, mm, ss;
    hal_get_local_time(&y, &m, &d, &hh, &mm, &ss);

    if (g_screen == SCR_READ_TEXT || g_screen == SCR_READ_PDF || g_screen == SCR_READ_IMG) {
        stats_add_reading(y, m, d, 1, 0);
        if (g_book_count > 0 && strcmp(g_books[0].path, g_filepath) == 0) {
            g_books[0].read_secs++;
        }
        g_unsaved_secs++;
        if (g_unsaved_secs >= 60) {
            g_unsaved_secs = 0;
            stats_save(hal_get_stats_path());
            save_library();
        }
    }

    if (mm != g_last_clock_min) {
        g_last_clock_min = mm;
        return 1;
    }
    return 0;
}

int reader_open_file(const char *filepath) {
    if (!filepath || !filepath[0]) return 0;
    if (g_screen != SCR_HOME) { save_bookmark(); save_manual_bookmarks(); }

    show_loading_card(filepath);
    clear_illust_cache();

    BookFormat fmt = book_detect_format(filepath);
    if (fmt == BOOK_FMT_PDF) {
        PdfDoc *nd = pdf_open(filepath);
        if (!nd) {
            snprintf(g_toast_msg, sizeof(g_toast_msg), "無法開啟 PDF：%s", get_filename_only(filepath));
            return 0;
        }
        pdf_close(g_pdf); g_pdf = nd;
        free(g_pdf_reflow_text); g_pdf_reflow_text = NULL; g_pdf_reflow_page_cached = -1;
        book_doc_free(&g_doc);
        g_doc.format = BOOK_FMT_PDF;
        const char *pt = pdf_get_title(g_pdf);
        utf8_ellipsis((pt && pt[0]) ? pt : get_filename_only(filepath), 44, g_doc.title, sizeof(g_doc.title));
        sanitize_field(g_doc.author, sizeof(g_doc.author), pdf_get_author(g_pdf));

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
    invalidate_cover_cache_for(filepath); /* 重新載入剛擷取的 EPUB 真封面縮圖！ */
    g_toast_msg[0] = '\0';
    load_manual_bookmarks(g_filepath);

    if (is_image_fmt(g_doc.format)) {
        free(g_page_offsets); g_page_offsets = NULL; g_page_count = 0;
        g_screen = SCR_READ_IMG;
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
    if (g_doc.text && g_page_offsets && g_page_count > 0) repaginate(g_page_offsets[g_current_page]);
}

static void open_new_book_dialog(void) {
    char path[384] = {0};
    snprintf(g_pending_import_folder, sizeof(g_pending_import_folder), "%s", g_cur_folder);
    if (hal_open_file_dialog(path, sizeof(path))) reader_open_file(path);
    g_pending_import_folder[0] = '\0';
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

static void activate_read_menu_item(int idx) {
    switch (idx) {
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
            if (g_doc.format == BOOK_FMT_PDF && g_pdf) { g_jump_target = g_pdf_page; g_screen = SCR_JUMP; }
            else if (g_page_count > 0) { g_jump_target = g_current_page; g_screen = SCR_JUMP; }
            break;
        case 7:
            save_bookmark(); save_manual_bookmarks();
            g_screen = SCR_HOME;
            break;
    }
}

/* ============================================================
 * 按鍵事件處理
 * ============================================================ */
void reader_on_key(ReaderKey key, char raw_char) {
    int is_ok = (key == RKEY_OK || key == RKEY_SPACE);

    if (g_screen == SCR_NEW_FOLDER) {
        if (key == RKEY_BACK) { g_screen = SCR_HOME; return; }
        if (key == RKEY_BACKSPACE) {
            size_t n = strlen(g_new_folder_buf);
            if (n > 0) {
                while (n > 0 && ((unsigned char)g_new_folder_buf[n - 1] & 0xC0) == 0x80) n--;
                if (n > 0) n--;
                g_new_folder_buf[n] = '\0';
            }
            return;
        }
        /* 空白時按 1~4 快速建立常用資料夾 */
        if (g_new_folder_buf[0] == '\0' && (key == RKEY_TAB1 || key == RKEY_TAB2 || key == RKEY_TAB3 || key == RKEY_TAB4)) {
            const char *presets[4] = { "鬼滅之刃", "漫畫", "小說", "講義" };
            folder_create(presets[key - RKEY_TAB1]);
            g_screen = SCR_HOME;
            return;
        }
        if (key == RKEY_OK) {
            if (g_new_folder_buf[0]) folder_create(g_new_folder_buf);
            g_screen = SCR_HOME;
            return;
        }
        if (raw_char >= 0x20 && raw_char < 0x7F && raw_char != '|') {
            size_t n = strlen(g_new_folder_buf);
            if (n + 1 < sizeof(g_new_folder_buf)) {
                g_new_folder_buf[n] = raw_char;
                g_new_folder_buf[n + 1] = '\0';
            }
        }
        return;
    }

    if (g_screen == SCR_MOVE_FOLDER) {
        int total_opts = g_folder_count + 1; /* 0 = 移出到外層, 1..N = 各資料夾 */
        if (key == RKEY_BACK || key == RKEY_MENU) { g_screen = SCR_HOME; return; }
        if (key == RKEY_UP || key == RKEY_LEFT) {
            g_move_folder_sel = (g_move_folder_sel + total_opts - 1) % total_opts;
            return;
        }
        if (key == RKEY_DOWN || key == RKEY_RIGHT) {
            g_move_folder_sel = (g_move_folder_sel + 1) % total_opts;
            return;
        }
        if (is_ok) {
            if (g_move_book_idx >= 0 && g_move_book_idx < g_book_count) {
                if (g_move_folder_sel == 0) g_books[g_move_book_idx].folder[0] = '\0';
                else snprintf(g_books[g_move_book_idx].folder, sizeof(g_books[g_move_book_idx].folder), "%s", g_folders[g_move_folder_sel - 1]);
                save_library();
            }
            g_screen = SCR_HOME;
        }
        return;
    }

    if (g_screen == SCR_NEXT_VOL) {
        if (key == RKEY_BACK) { g_screen = active_read_screen(); return; }
        if (is_ok) {
            if (g_next_vol_idx >= 0 && g_next_vol_idx < g_book_count) {
                char target[384];
                snprintf(target, sizeof(target), "%s", g_books[g_next_vol_idx].path);
                reader_open_file(target);
            } else {
                g_screen = active_read_screen();
            }
        }
        return;
    }

    if (g_screen == SCR_SEARCH) {
        if (key == RKEY_BACK) { g_screen = active_read_screen(); return; }
        if (key == RKEY_BACKSPACE) {
            size_t n = strlen(g_search_query);
            if (n > 0) {
                while (n > 0 && ((unsigned char)g_search_query[n - 1] & 0xC0) == 0x80) n--;
                if (n > 0) n--;
                g_search_query[n] = '\0'; g_search_ran = 0;
            }
            return;
        }
        if (g_search_query[0] == '\0' && (key == RKEY_TAB1 || key == RKEY_TAB2 || key == RKEY_TAB3 || key == RKEY_TAB4)) {
            const char *presets[4] = { "第一章", "序章", "結論", "重點" };
            snprintf(g_search_query, sizeof(g_search_query), "%s", presets[key - RKEY_TAB1]);
            run_fulltext_search();
            return;
        }
        if (key == RKEY_UP) { if (g_search_sel > 0) g_search_sel--; return; }
        if (key == RKEY_DOWN) { if (g_search_sel + 1 < g_search_cnt) g_search_sel++; return; }
        if (key == RKEY_OK) {
            if (!g_search_ran) run_fulltext_search();
            else if (g_search_cnt > 0) {
                SearchHit *sh = &g_search_hits[g_search_sel];
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) { g_pdf_page = (int)sh->pos_val; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                else jump_to_offset(sh->pos_val);
                save_bookmark();
                g_screen = active_read_screen();
            } else run_fulltext_search();
            return;
        }
        if (raw_char >= 0x20 && raw_char < 0x7F) {
            size_t n = strlen(g_search_query);
            if (n + 1 < sizeof(g_search_query)) { g_search_query[n] = raw_char; g_search_query[n + 1] = '\0'; g_search_ran = 0; }
        }
        return;
    }

    if (g_screen == SCR_BOOKMARKS) {
        if (key == RKEY_BACK || key == RKEY_BOOKMARK) { g_screen = active_read_screen(); return; }
        if (key == RKEY_UP || key == RKEY_LEFT) { if (g_bmark_sel > 0) g_bmark_sel--; return; }
        if (key == RKEY_DOWN || key == RKEY_RIGHT) { if (g_bmark_sel + 1 < g_bmark_count) g_bmark_sel++; return; }
        if (key == RKEY_SPACE || key == RKEY_FONT_UP) { add_current_manual_bookmark(); return; }
        if (key == RKEY_DELETE && g_bmark_count > 0) {
            for (int i = g_bmark_sel; i + 1 < g_bmark_count; i++) g_bmarks[i] = g_bmarks[i + 1];
            g_bmark_count--;
            if (g_bmark_sel >= g_bmark_count && g_bmark_count > 0) g_bmark_sel = g_bmark_count - 1;
            save_manual_bookmarks();
            return;
        }
        if (key == RKEY_OK) {
            if (g_bmark_count == 0) add_current_manual_bookmark();
            else {
                ManualBookmark *bm = &g_bmarks[g_bmark_sel];
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
                    int p = (int)bm->pos_val;
                    if (p >= 0 && p < pdf_page_count(g_pdf)) { g_pdf_page = p; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                } else jump_to_offset(bm->pos_val);
                save_bookmark();
                g_screen = active_read_screen();
            }
        }
        return;
    }

    if (g_screen == SCR_HOME) {
        if (key == RKEY_TAB1) { g_home_tab = TAB_SHELF; return; }
        if (key == RKEY_TAB2) { g_home_tab = TAB_MANAGE; return; }
        if (key == RKEY_TAB3) { g_home_tab = TAB_STATS; return; }
        if (key == RKEY_TAB4) { g_home_tab = TAB_APPEARANCE; return; }
        if (key == RKEY_TAB5 || key == RKEY_OPEN) { open_new_book_dialog(); return; }
        if (key == RKEY_THEME) { g_theme_idx = (g_theme_idx + 1) % 4; save_library(); return; }
        if (key == RKEY_SPACING) { g_spacing_idx = (g_spacing_idx + 1) % 3; save_library(); return; }
        if (key == RKEY_BACK) { if (g_home_tab == TAB_SHELF && g_cur_folder[0]) { g_cur_folder[0] = '\0'; g_shelf_sel = 0; } else hal_request_quit(); return; }

        /* 365 天年度統計頁：方向鍵瀏覽每日格、PgUp/PgDn 切換年份 */
        if (g_home_tab == TAB_STATS) {
            int max_d = stats_days_in_year(g_stats_year) - 1;
            if (key == RKEY_UP)        g_stats_doy -= 1;
            else if (key == RKEY_DOWN) g_stats_doy += 1;
            else if (key == RKEY_LEFT) g_stats_doy -= 7;
            else if (key == RKEY_RIGHT) g_stats_doy += 7;
            else if (key == RKEY_PGUP) { if (g_stats_year > 2020) g_stats_year--; }
            else if (key == RKEY_PGDN) { if (g_stats_year < 2040) g_stats_year++; }
            max_d = stats_days_in_year(g_stats_year) - 1;
            if (g_stats_doy < 0) g_stats_doy = 0;
            if (g_stats_doy > max_d) g_stats_doy = max_d;
            return;
        }

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

        /* 按 N 建立新資料夾 */
        if (raw_char == 'n' || raw_char == 'N') {
            open_new_folder_modal();
            return;
        }

        if (g_home_tab == TAB_SHELF) {
            /* 在資料夾內部按 Backspace 返回外層 */
            if (g_cur_folder[0] != '\0' && (key == RKEY_BACKSPACE || key == RKEY_SHELF)) {
                g_cur_folder[0] = '\0';
                g_shelf_sel = 0;
                return;
            }

            int gtypes[MAX_BOOKS + MAX_FOLDERS], gidxs[MAX_BOOKS + MAX_FOLDERS];
            int gcnt = shelf_build_grid(gtypes, gidxs, MAX_BOOKS + MAX_FOLDERS);

            if (g_cur_folder[0] == '\0') {
                /* 外層主書架：sel 0 = Hero Card (若有書)，sel 1..gcnt = 下方網格 (資料夾 + 未分類書) */
                int has_hero = (g_book_count > 0) ? 1 : 0;
                int total = has_hero + gcnt;
                if (total == 0) { if (is_ok) open_new_book_dialog(); return; }
                if (g_shelf_sel >= total) g_shelf_sel = total - 1;

                if (key == RKEY_LEFT) { if (g_shelf_sel > 0) g_shelf_sel--; }
                else if (key == RKEY_RIGHT) { if (g_shelf_sel + 1 < total) g_shelf_sel++; }
                else if (key == RKEY_UP) {
                    if (has_hero && g_shelf_sel >= 5) g_shelf_sel -= 4;
                    else if (has_hero && g_shelf_sel >= 1) g_shelf_sel = 0;
                    else if (!has_hero && g_shelf_sel >= 4) g_shelf_sel -= 4;
                } else if (key == RKEY_DOWN) {
                    if (has_hero && g_shelf_sel == 0 && gcnt > 0) g_shelf_sel = 1;
                    else if (g_shelf_sel + 4 < total) g_shelf_sel += 4;
                    else if (g_shelf_sel + 1 < total) g_shelf_sel = total - 1;
                } else if (key == RKEY_PGUP) { g_shelf_sel = (g_shelf_sel >= 4) ? (g_shelf_sel - 4) : 0; }
                else if (key == RKEY_PGDN) { g_shelf_sel = (g_shelf_sel + 4 < total) ? (g_shelf_sel + 4) : (total - 1); }
                else if (key == RKEY_MENU) {
                    if (has_hero && g_shelf_sel == 0) open_move_folder_modal(0);
                    else {
                        int gi = g_shelf_sel - has_hero;
                        if (gi >= 0 && gi < gcnt && gtypes[gi] == 0) open_move_folder_modal(gidxs[gi]);
                    }
                } else if (key == RKEY_DELETE) {
                    if (has_hero && g_shelf_sel == 0) remove_book_at(0);
                    else {
                        int gi = g_shelf_sel - has_hero;
                        if (gi >= 0 && gi < gcnt) {
                            if (gtypes[gi] == 1) folder_delete(gidxs[gi]);
                            else remove_book_at(gidxs[gi]);
                        }
                    }
                } else if (is_ok) {
                    if (has_hero && g_shelf_sel == 0) {
                        char target[384]; snprintf(target, sizeof(target), "%s", g_books[0].path);
                        reader_open_file(target);
                    } else {
                        int gi = g_shelf_sel - has_hero;
                        if (gi >= 0 && gi < gcnt) {
                            if (gtypes[gi] == 1) {
                                snprintf(g_cur_folder, sizeof(g_cur_folder), "%s", g_folders[gidxs[gi]]);
                                g_shelf_sel = 0;
                            } else {
                                char target[384]; snprintf(target, sizeof(target), "%s", g_books[gidxs[gi]].path);
                                reader_open_file(target);
                            }
                        }
                    }
                }
            } else {
                /* 資料夾內部：純 2x4 書籍網格 */
                if (gcnt == 0) { if (is_ok) open_new_book_dialog(); return; }
                if (g_shelf_sel >= gcnt) g_shelf_sel = gcnt - 1;
                if (key == RKEY_LEFT) { if (g_shelf_sel > 0) g_shelf_sel--; }
                else if (key == RKEY_RIGHT) { if (g_shelf_sel + 1 < gcnt) g_shelf_sel++; }
                else if (key == RKEY_UP) { if (g_shelf_sel >= 4) g_shelf_sel -= 4; }
                else if (key == RKEY_DOWN) {
                    if (g_shelf_sel + 4 < gcnt) g_shelf_sel += 4;
                    else if (g_shelf_sel + 1 < gcnt) g_shelf_sel = gcnt - 1;
                } else if (key == RKEY_PGUP) { g_shelf_sel = (g_shelf_sel >= 8) ? (g_shelf_sel - 8) : 0; }
                else if (key == RKEY_PGDN) { g_shelf_sel = (g_shelf_sel + 8 < gcnt) ? (g_shelf_sel + 8) : (gcnt - 1); }
                else if (key == RKEY_MENU) { open_move_folder_modal(gidxs[g_shelf_sel]); }
                else if (key == RKEY_DELETE) { remove_book_at(gidxs[g_shelf_sel]); }
                else if (is_ok) {
                    char target[384]; snprintf(target, sizeof(target), "%s", g_books[gidxs[g_shelf_sel]].path);
                    reader_open_file(target);
                }
            }
            return;
        }

        if (g_book_count == 0) { if (is_ok) open_new_book_dialog(); return; }

        if (g_home_tab == TAB_MANAGE) {
            if (key == RKEY_UP || key == RKEY_LEFT || key == RKEY_PGUP) { if (g_shelf_sel > 0) g_shelf_sel--; }
            else if (key == RKEY_DOWN || key == RKEY_RIGHT || key == RKEY_PGDN) { if (g_shelf_sel + 1 < g_book_count) g_shelf_sel++; }
            else if (key == RKEY_MENU) open_move_folder_modal(g_shelf_sel);
            else if (key == RKEY_DELETE) remove_book_at(g_shelf_sel);
            else if (is_ok) {
                char target[384]; snprintf(target, sizeof(target), "%s", g_books[g_shelf_sel].path);
                reader_open_file(target);
            }
            return;
        }
        return;
    }

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
        if (is_ok) activate_read_menu_item(g_read_menu_sel);
        return;
    }

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
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) { g_pdf_page = (int)g_doc.chapters[g_toc_sel].offset; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                else jump_to_offset(g_doc.chapters[g_toc_sel].offset);
                save_bookmark();
            }
            g_screen = active_read_screen();
        }
        return;
    }

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

    if (g_screen == SCR_READ_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf);
        int vw = hal_get_width(), vh = hal_get_height() - 74;
        int can_pan_x = (!g_pdf_reflow && g_pdf_last_pw > vw);
        int can_pan_y = (!g_pdf_reflow && g_pdf_last_ph > vh);

        switch (key) {
            case RKEY_PGUP:
                if (g_pdf_page > 0) { g_pdf_page--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(); save_bookmark(); }
                break;
            case RKEY_PGDN:
            case RKEY_SPACE:
                if (g_pdf_page + 1 < tot) { g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(); save_bookmark(); }
                else { int nv = find_next_volume_in_folder(g_filepath); if (nv >= 0) { g_next_vol_idx = nv; g_screen = SCR_NEXT_VOL; } }
                break;
            case RKEY_LEFT:
                if (can_pan_x && g_pdf_pan_x > 0) { g_pdf_pan_x -= 90; clamp_pdf_pan(); }
                else if (g_pdf_page > 0) { g_pdf_page--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(); save_bookmark(); }
                break;
            case RKEY_RIGHT:
                if (can_pan_x && g_pdf_pan_x < g_pdf_last_pw - vw) { g_pdf_pan_x += 90; clamp_pdf_pan(); }
                else if (g_pdf_page + 1 < tot) { g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(); save_bookmark(); }
                break;
            case RKEY_UP:
                if (can_pan_y && g_pdf_pan_y > 0) { g_pdf_pan_y -= 90; clamp_pdf_pan(); }
                else if (g_pdf_page > 0) { g_pdf_page--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(); save_bookmark(); }
                break;
            case RKEY_DOWN:
                if (can_pan_y && g_pdf_pan_y < g_pdf_last_ph - vh) { g_pdf_pan_y += 90; clamp_pdf_pan(); }
                else if (g_pdf_page + 1 < tot) { g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(); save_bookmark(); }
                break;
            case RKEY_FONT_UP:   if (g_pdf_zoom < 4) { g_pdf_zoom++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; } break;
            case RKEY_FONT_DOWN: if (g_pdf_zoom > 0) { g_pdf_zoom--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; } break;
            case RKEY_REFLOW:    g_pdf_reflow = !g_pdf_reflow; break;
            case RKEY_THEME:     g_theme_idx = (g_theme_idx + 1) % 4; save_library(); break;
            case RKEY_TOC:       open_toc_screen(); break;
            case RKEY_JUMP:      g_jump_target = g_pdf_page; g_screen = SCR_JUMP; break;
            case RKEY_BOOKMARK:  g_screen = SCR_BOOKMARKS; break;
            case RKEY_SEARCH:    g_search_ran = 0; g_screen = SCR_SEARCH; break;
            case RKEY_OPEN:      open_new_book_dialog(); break;
            case RKEY_SHELF:
            case RKEY_BACKSPACE: save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; break;
            case RKEY_BACK:
            case RKEY_MENU:      g_read_menu_sel = 0; g_screen = SCR_READ_MENU; break;
            default: break;
        }
        return;
    }

    if (g_screen == SCR_READ_IMG) {
        if (key == RKEY_LEFT || key == RKEY_UP || key == RKEY_PGUP) {
            char np[384] = {0};
            if (hal_find_sibling_image(g_filepath, -1, np, sizeof(np))) {
                snprintf(g_filepath, sizeof(g_filepath), "%s", np);
                utf8_ellipsis(get_filename_only(np), 44, g_doc.title, sizeof(g_doc.title));
                record_page_turn_stat();
            }
        } else if (key == RKEY_RIGHT || key == RKEY_DOWN || key == RKEY_PGDN || key == RKEY_SPACE) {
            char np[384] = {0};
            if (hal_find_sibling_image(g_filepath, 1, np, sizeof(np))) {
                snprintf(g_filepath, sizeof(g_filepath), "%s", np);
                utf8_ellipsis(get_filename_only(np), 44, g_doc.title, sizeof(g_doc.title));
                record_page_turn_stat();
            }
        } else if (key == RKEY_THEME) { g_theme_idx = (g_theme_idx + 1) % 4; save_library(); }
        else if (key == RKEY_OPEN) open_new_book_dialog();
        else if (key == RKEY_SHELF || key == RKEY_BACKSPACE) { save_bookmark(); g_screen = SCR_HOME; }
        else if (key == RKEY_BACK || key == RKEY_MENU) { g_read_menu_sel = 0; g_screen = SCR_READ_MENU; }
        return;
    }

    switch (key) {
        case RKEY_LEFT:
        case RKEY_UP:
        case RKEY_PGUP:
            if (g_current_page > 0) { g_current_page--; record_page_turn_stat(); save_bookmark(); }
            break;
        case RKEY_RIGHT:
        case RKEY_DOWN:
        case RKEY_PGDN:
        case RKEY_SPACE:
            if (g_current_page + 1 < g_page_count) { g_current_page++; record_page_turn_stat(); save_bookmark(); }
            else { int nv = find_next_volume_in_folder(g_filepath); if (nv >= 0) { g_next_vol_idx = nv; g_screen = SCR_NEXT_VOL; } }
            break;
        case RKEY_TOC:       open_toc_screen(); break;
        case RKEY_JUMP:      if (g_page_count > 0) { g_jump_target = g_current_page; g_screen = SCR_JUMP; } break;
        case RKEY_BOOKMARK:  g_screen = SCR_BOOKMARKS; break;
        case RKEY_SEARCH:    g_search_ran = 0; g_screen = SCR_SEARCH; break;
        case RKEY_THEME:     g_theme_idx = (g_theme_idx + 1) % 4; save_library(); break;
        case RKEY_SPACING:   g_spacing_idx = (g_spacing_idx + 1) % 3; save_library(); reader_on_resize(); break;
        case RKEY_OPEN:      open_new_book_dialog(); break;
        case RKEY_SHELF:
        case RKEY_BACKSPACE: save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; break;
        case RKEY_BACK:
        case RKEY_MENU:      g_read_menu_sel = 0; g_screen = SCR_READ_MENU; break;
        default: break;
    }
}

/* ============================================================
 * 全畫面觸控點擊互動 (含 365 天年度熱力圖直接點選任意一天!)
 * ============================================================ */
void reader_on_touch(int tx, int ty) {
    int w = hal_get_width(), h = hal_get_height();
    if (tx < 0 || ty < 0 || tx >= w || ty >= h) return;

    if (g_screen == SCR_HOME) {
        /* 左側 5 顆現代導覽列 (y=66, pitch=68) */
        if (tx < 76) {
            for (int i = 0; i < 5; i++) {
                int y0 = 66 + i * 68;
                if (ty >= y0 && ty <= y0 + 58) {
                    if (i == 0) g_home_tab = TAB_SHELF;
                    else if (i == 1) g_home_tab = TAB_MANAGE;
                    else if (i == 2) g_home_tab = TAB_STATS;
                    else if (i == 3) g_home_tab = TAB_APPEARANCE;
                    else open_new_book_dialog();
                    return;
                }
            }
            return;
        }

        int mx = 94, mw = w - mx - 18;

        if (g_home_tab == TAB_SHELF) {
            /* 頂部按鈕列 */
            if (ty <= 48) {
                if (g_cur_folder[0] != '\0' && tx < mx + 92) {
                    g_cur_folder[0] = '\0'; g_shelf_sel = 0; return;
                }
                if (tx >= w - 188) { open_new_book_dialog(); return; }
                if (g_cur_folder[0] == '\0' && tx >= w - 310 && tx < w - 192) { open_new_folder_modal(); return; }
                return;
            }

            int gtypes[MAX_BOOKS + MAX_FOLDERS], gidxs[MAX_BOOKS + MAX_FOLDERS];
            int gcnt = shelf_build_grid(gtypes, gidxs, MAX_BOOKS + MAX_FOLDERS);

            if (g_cur_folder[0] == '\0') {
                if (g_book_count == 0 && g_folder_count == 0) { open_new_book_dialog(); return; }
                if (g_book_count > 0 && ty >= 54 && ty <= 174) {
                    g_shelf_sel = 0;
                    char target[384]; snprintf(target, sizeof(target), "%s", g_books[0].path);
                    reader_open_file(target);
                    return;
                }
                int grid_y = 208, gap = 14, cols = 4;
                int card_w = (mw - gap * (cols - 1)) / cols;
                if (ty >= grid_y && ty <= h - 30) {
                    int has_hero = (g_book_count > 0) ? 1 : 0;
                    int grid_sel = (g_shelf_sel >= has_hero) ? (g_shelf_sel - has_hero) : 0;
                    int start_gi = (grid_sel / 4) * 4;
                    for (int col = 0; col < cols; col++) {
                        int cx = mx + col * (card_w + gap);
                        int gi = start_gi + col;
                        if (gi < gcnt && tx >= cx && tx <= cx + card_w) {
                            g_shelf_sel = gi + has_hero;
                            if (gtypes[gi] == 1) {
                                snprintf(g_cur_folder, sizeof(g_cur_folder), "%s", g_folders[gidxs[gi]]);
                                g_shelf_sel = 0;
                            } else {
                                char target[384]; snprintf(target, sizeof(target), "%s", g_books[gidxs[gi]].path);
                                reader_open_file(target);
                            }
                            return;
                        }
                    }
                }
            } else {
                if (gcnt == 0) { open_new_book_dialog(); return; }
                int grid_y = 62, gap = 12, cols = 4;
                int card_w = (mw - gap * (cols - 1)) / cols;
                int card_h = (h - grid_y - 34 - gap) / 2;
                if (ty >= grid_y && ty <= h - 30) {
                    int start_gi = (g_shelf_sel / 8) * 8;
                    int row = (ty - grid_y) / (card_h + gap);
                    if (row >= 0 && row < 2) {
                        for (int col = 0; col < cols; col++) {
                            int cx = mx + col * (card_w + gap);
                            int gi = start_gi + row * 4 + col;
                            if (gi < gcnt && tx >= cx && tx <= cx + card_w) {
                                g_shelf_sel = gi;
                                char target[384]; snprintf(target, sizeof(target), "%s", g_books[gidxs[gi]].path);
                                reader_open_file(target);
                                return;
                            }
                        }
                    }
                }
            }
            return;
        }

        if (g_home_tab == TAB_MANAGE) {
            int row_h = 68, top_y = 54, visible = (h - top_y - 36) / row_h;
            if (visible < 1) visible = 1;
            if (ty >= top_y) {
                int row = (ty - top_y) / row_h;
                int idx = g_manage_scroll + row;
                if (idx >= 0 && idx < g_book_count && row < visible) {
                    if (tx >= mx + mw - 58) remove_book_at(idx);
                    else if (tx >= mx + mw - 110) open_move_folder_modal(idx);
                    else {
                        g_shelf_sel = idx;
                        char target[384]; snprintf(target, sizeof(target), "%s", g_books[idx].path);
                        reader_open_file(target);
                    }
                }
            }
            return;
        }

        if (g_home_tab == TAB_STATS) {
            /* 右上年分切換 [<] [>] */
            if (ty <= 46) {
                if (tx >= w - 180 && tx < w - 102) { if (g_stats_year > 2020) g_stats_year--; return; }
                if (tx >= w - 98 && tx <= w - 16)  { if (g_stats_year < 2040) g_stats_year++; return; }
            }
            /* 直接點選 365 天熱力圖中的任意一個方格！ */
            int gx0 = mx + 36, gy0 = 186;
            int cell_pitch_x = 12, cell_pitch_y = 14;
            if (tx >= gx0 && ty >= gy0 && ty < gy0 + 7 * cell_pitch_y) {
                int col = (tx - gx0) / cell_pitch_x;
                int row = (ty - gy0) / cell_pitch_y;
                if (col >= 0 && col < 53 && row >= 0 && row < 7) {
                    int jan1_dow = stats_day_of_week(g_stats_year, 1, 1);
                    int doy = col * 7 + row - jan1_dow;
                    int tot_d = stats_days_in_year(g_stats_year);
                    if (doy >= 0 && doy < tot_d) g_stats_doy = doy;
                }
            }
            return;
        }

        if (g_home_tab == TAB_APPEARANCE) {
            int gap = 14, cw = (mw - gap * 3) / 4;
            if (ty >= 80 && ty <= 232) {
                for (int i = 0; i < 4; i++) {
                    int cx = mx + i * (cw + gap);
                    if (tx >= cx && tx <= cx + cw) { g_theme_idx = i; g_appear_row = 0; save_library(); return; }
                }
            }
            if (ty >= 274 && ty <= 316) {
                for (int i = 0; i < 3; i++) {
                    int px = mx + i * 140;
                    if (tx >= px && tx <= px + 126) { g_spacing_idx = i; g_appear_row = 1; save_library(); return; }
                }
            }
            return;
        }
        return;
    }

    if (g_screen == SCR_NEW_FOLDER) {
        int mw = 480, mh = 250, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = SCR_HOME; return; }
        /* 4 顆常用預設分類按鈕 */
        if (ty >= my + 114 && ty <= my + 150) {
            const char *presets[4] = { "鬼滅之刃", "漫畫", "小說", "講義" };
            int bw = (mw - 60) / 4;
            int idx = (tx - (mx + 18)) / (bw + 8);
            if (idx >= 0 && idx < 4) { folder_create(presets[idx]); g_screen = SCR_HOME; return; }
        }
        /* 下方確定 / 取消按鈕 */
        if (ty >= my + 186) {
            if (tx < mx + mw / 2) { if (g_new_folder_buf[0]) folder_create(g_new_folder_buf); }
            g_screen = SCR_HOME;
        }
        return;
    }

    if (g_screen == SCR_MOVE_FOLDER) {
        int mw = 440, mh = 340, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = SCR_HOME; return; }
        if (ty >= my + 56 && ty < my + mh - 30) {
            int start = (g_move_folder_sel >= 6) ? (g_move_folder_sel - 5) : 0;
            int row = (ty - (my + 56)) / 38;
            int opt = start + row;
            if (opt >= 0 && opt <= g_folder_count) {
                g_move_folder_sel = opt;
                reader_on_key(RKEY_OK, 0);
            }
        }
        return;
    }

    if (g_screen == SCR_NEXT_VOL) {
        int bw = 460, bh = 170, bx = (w - bw) / 2, by = (h - bh) / 2;
        if (tx < bx || tx > bx + bw || ty < by || ty > by + bh) { g_screen = active_read_screen(); return; }
        if (ty >= by + 110) {
            if (tx < bx + bw / 2) reader_on_key(RKEY_OK, 0);
            else g_screen = active_read_screen();
        }
        return;
    }

    if (g_screen == SCR_READ_MENU) {
        int mw = 440, mh = 376, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = active_read_screen(); return; }
        if (ty >= my + 52) {
            int idx = (ty - (my + 52)) / 39;
            if (idx >= 0 && idx < 8) {
                g_read_menu_sel = idx;
                if (idx == 4 && tx >= mx + mw - 118) {
                    int ti = (tx - (mx + mw - 112)) / 22;
                    if (ti >= 0 && ti < 4) { g_theme_idx = ti; save_library(); return; }
                }
                activate_read_menu_item(idx);
            }
        }
        return;
    }

    if (g_screen == SCR_BOOKMARKS) {
        int mw = 540, mh = 360, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = active_read_screen(); return; }
        if (ty <= my + 46 && tx >= mx + mw - 140) { add_current_manual_bookmark(); return; }
        if (g_bmark_count == 0) { add_current_manual_bookmark(); return; }
        if (ty >= my + 56 && ty < my + mh - 34) {
            int start = (g_bmark_sel >= 7) ? (g_bmark_sel - 6) : 0;
            int row = (ty - (my + 56)) / 38;
            int idx = start + row;
            if (idx >= 0 && idx < g_bmark_count) {
                if (tx >= mx + mw - 64) {
                    for (int i = idx; i + 1 < g_bmark_count; i++) g_bmarks[i] = g_bmarks[i + 1];
                    g_bmark_count--;
                    if (g_bmark_sel >= g_bmark_count && g_bmark_count > 0) g_bmark_sel = g_bmark_count - 1;
                    save_manual_bookmarks();
                } else { g_bmark_sel = idx; reader_on_key(RKEY_OK, 0); }
            }
        }
        return;
    }

    if (g_screen == SCR_SEARCH) {
        int mw = 580, mh = 380, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = active_read_screen(); return; }
        if (g_search_ran && g_search_cnt > 0 && ty >= my + 104 && ty < my + mh - 34) {
            int row = (ty - (my + 104)) / 38;
            int idx = g_search_scroll + row;
            if (idx >= 0 && idx < g_search_cnt) { g_search_sel = idx; reader_on_key(RKEY_OK, 0); }
        }
        return;
    }

    if (g_screen == SCR_JUMP) {
        int bw = 460, bh = 160, bx = (w - bw) / 2, by = (h - bh) / 2;
        if (tx < bx || tx > bx + bw || ty < by || ty > by + bh) { g_screen = active_read_screen(); return; }
        int tot = (g_doc.format == BOOK_FMT_PDF && g_pdf) ? pdf_page_count(g_pdf) : g_page_count;
        if (ty >= by + 50 && ty <= by + 90 && tot > 1) {
            int sx = bx + 24, sw = bw - 48, rel = tx - sx;
            if (rel < 0) rel = 0;
            if (rel > sw) rel = sw;
            g_jump_target = (rel * (tot - 1)) / sw;
            return;
        }
        if (ty >= by + 104) {
            if (tx < bx + bw / 2) reader_on_key(RKEY_OK, 0);
            else g_screen = active_read_screen();
        }
        return;
    }

    if (g_screen == SCR_TOC) {
        if (ty <= 44 && tx >= w - 100) { g_screen = active_read_screen(); return; }
        if (ty >= h - 34) {
            if (tx < w / 3) reader_on_key(RKEY_PGUP, 0);
            else if (tx < (w * 2) / 3) reader_on_key(RKEY_PGDN, 0);
            else g_screen = active_read_screen();
            return;
        }
        int row = (ty - 56) / 38, idx = g_toc_scroll + row;
        if (row >= 0 && idx >= 0 && idx < g_doc.chapter_count) { g_toc_sel = idx; reader_on_key(RKEY_OK, 0); }
        return;
    }

    if (g_screen == SCR_READ_PDF || g_screen == SCR_READ_TEXT || g_screen == SCR_READ_IMG) {
        if (ty <= 36) {
            if (tx < 86) { save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; return; }
            if (g_screen == SCR_READ_PDF) {
                if (tx >= w - 252 && tx < w - 208) { reader_on_key(RKEY_FONT_DOWN, '-'); return; }
                if (tx >= w - 204 && tx < w - 160) { reader_on_key(RKEY_FONT_UP, '+'); return; }
                if (tx >= w - 156 && tx < w - 70)  { reader_on_key(RKEY_REFLOW, 'R'); return; }
            }
            return;
        }
        if (ty >= h - 38) {
            if (tx >= w - 340) {
                int slot = (tx - (w - 340)) / 68;
                if (slot == 0) open_toc_screen();
                else if (slot == 1) g_screen = SCR_BOOKMARKS;
                else if (slot == 2) { g_search_ran = 0; g_screen = SCR_SEARCH; }
                else if (slot == 3) {
                    g_jump_target = (g_screen == SCR_READ_PDF) ? g_pdf_page : g_current_page;
                    g_screen = SCR_JUMP;
                } else { g_read_menu_sel = 0; g_screen = SCR_READ_MENU; }
            } else { g_read_menu_sel = 0; g_screen = SCR_READ_MENU; }
            return;
        }
        if (tx < (w * 30) / 100) reader_on_key(RKEY_PGUP, 0);
        else if (tx > (w * 70) / 100) reader_on_key(RKEY_PGDN, 0);
        else { g_read_menu_sel = 0; g_screen = SCR_READ_MENU; }
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

static void draw_clock_pill(const AppTheme *th, int rx_right, int y) {
    int y0, m0, d0, hh, mm, ss;
    hal_get_local_time(&y0, &m0, &d0, &hh, &mm, &ss);
    char clk[12];
    snprintf(clk, sizeof(clk), "%02d:%02d", hh, mm);
    int cw = ui_ascii_width(clk, 1) + 14;
    int x = rx_right - cw;
    ui_fill_round_rect(x, y, cw, 20, 8, th->card_bg);
    ui_draw_round_rect(x, y, cw, 20, 8, th->card_border);
    ui_draw_ascii(x + 7, y + 6, clk, 1, th->text);
}

static void draw_book_cover_smart(int x, int y, int w, int h, const BookEntry *be, int is_dark) {
    int cw = 0, ch = 0;
    const uint8_t *rgb = (be->format == BOOK_FMT_EPUB) ? get_cached_cover(be->path, &cw, &ch) : NULL;
    if (rgb && cw > 0 && ch > 0) {
        ui_draw_book_cover_img(x, y, w, h, rgb, cw, ch, be->format, is_dark);
    } else {
        ui_draw_book_cover(x, y, w, h, be->title, be->format, is_dark);
    }
}

static void render_left_rail(const AppTheme *th, int h) {
    int rw = 76;
    hal_fill_rect(0, 0, rw, h, th->rail_bg);
    hal_fill_rect(rw, 0, 1, h, th->card_border);

    ui_fill_round_rect(19, 14, 38, 38, 10, th->accent);
    ui_draw_ascii(32, 24, "R", 2, 0xFFFFFF);

    const char *labels[5] = { "書架", "管理", "統計", "外觀", "匯入" };
    for (int i = 0; i < 5; i++) {
        int ty = 66 + i * 68;
        int active = (i < 4 && (int)g_home_tab == i);
        uint32_t bg = active ? th->card_bg : th->rail_bg;
        uint32_t fg = active ? th->accent : th->muted;
        if (active) {
            ui_fill_round_rect(8, ty, rw - 16, 58, 10, bg);
            hal_fill_rect(0, ty + 12, 4, 34, th->accent);
        }
        if (i == 0) ui_icon_shelf(rw / 2, ty + 18, fg);
        else if (i == 1) ui_icon_list(rw / 2, ty + 18, fg);
        else if (i == 2) ui_icon_stats(rw / 2, ty + 18, fg);
        else if (i == 3) ui_icon_palette(rw / 2, ty + 18, fg);
        else ui_icon_import(rw / 2, ty + 18, fg);

        hal_set_text_bg(bg);
        draw_str(20, ty + 35, labels[i], 44, active ? th->text : th->muted);
        char kb[4]; snprintf(kb, sizeof(kb), "%d", i + 1);
        ui_draw_ascii(rw - 15, ty + 5, kb, 1, th->muted);
    }
    ui_draw_ascii(18, h - 22, "V6.0", 1, th->muted);
}

static void render_home_shelf(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    int cy, cm, cd, chh, cmm, css;
    hal_get_local_time(&cy, &cm, &cd, &chh, &cmm, &css);
    uint32_t today_s = 0, today_p = 0;
    stats_get_day(cy, cm, cd, &today_s, &today_p);

    int gtypes[MAX_BOOKS + MAX_FOLDERS], gidxs[MAX_BOOKS + MAX_FOLDERS];
    int gcnt = shelf_build_grid(gtypes, gidxs, MAX_BOOKS + MAX_FOLDERS);

    /* === A. 資料夾內部專屬畫面 === */
    if (g_cur_folder[0] != '\0') {
        ui_fill_round_rect(mx, 12, 86, 30, 8, th->card_bg);
        ui_draw_round_rect(mx, 12, 86, 30, 8, th->card_border);
        hal_set_text_bg(th->card_bg);
        draw_str(mx + 10, 18, "< 返回", 68, th->text);

        hal_set_text_bg(th->bg);
        char fhdr[96];
        snprintf(fhdr, sizeof(fhdr), "資料夾：%s  (%d 本)", g_cur_folder, gcnt);
        draw_str(mx + 98, 18, fhdr, 300, th->text);

        int bx = w - 184;
        ui_fill_round_rect(bx, 12, 166, 32, 16, th->accent);
        hal_set_text_bg(th->accent);
        draw_str(bx + 14, 19, "+ 匯入此資料夾", 144, 0xFFFFFF);

        if (gcnt == 0) {
            ui_card(mx + 20, 74, mw - 40, h - 120, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent);
            hal_set_text_bg(th->card_bg);
            draw_str(mx + 60, 150, "這個資料夾目前是空的。", mw - 120, th->text);
            draw_str(mx + 60, 184, "點右上 [+ 匯入此資料夾]，或在 [管理] 頁按 [移] 把書放進來！", mw - 120, th->muted);
            hal_set_text_bg(th->bg);
            draw_str(mx, h - 24, "[ESC/Backspace] 返回書架  ·  [O] 直接匯入至此資料夾", mw, th->muted);
            return;
        }

        int grid_y = 62, gap = 12, cols = 4;
        int card_w = (mw - gap * (cols - 1)) / cols;
        int card_h = (h - grid_y - 34 - gap) / 2;
        int start_gi = (g_shelf_sel / 8) * 8;

        for (int slot = 0; slot < 8; slot++) {
            int gi = start_gi + slot;
            if (gi >= gcnt) break;
            int row = slot / 4, col = slot % 4;
            int cx = mx + col * (card_w + gap);
            int cy0 = grid_y + row * (card_h + gap);
            BookEntry *be = &g_books[gidxs[gi]];
            ui_card(cx, cy0, card_w, card_h, 12, th->card_bg, th->card_border, th->shadow, (gi == g_shelf_sel), th->accent);

            int cov_h = card_h - 62; if (cov_h < 56) cov_h = 56;
            draw_book_cover_smart(cx + 10, cy0 + 8, card_w - 20, cov_h, be, th->is_dark);

            hal_set_text_bg(th->card_bg);
            char stitle[64]; utf8_ellipsis(be->title, (card_w - 20) / 9, stitle, sizeof(stitle));
            draw_str(cx + 10, cy0 + cov_h + 14, stitle, card_w - 20, th->text);

            int pct = calc_book_pct(be);
            ui_progress_bar(cx + 10, cy0 + card_h - 22, card_w - 20, 5, pct, th->track, ui_format_color(be->format));
            char ptxt[24]; snprintf(ptxt, sizeof(ptxt), "%d%%", pct);
            ui_draw_ascii(cx + 10, cy0 + card_h - 13, ptxt, 1, th->muted);
        }

        hal_set_text_bg(th->bg);
        draw_str(mx, h - 24, "[Enter]閱讀  [M]移出/換資料夾  [D]移出  [ESC]返回外層書架", mw, th->muted);
        return;
    }

    /* === B. 外層主書架 === */
    hal_set_text_bg(th->bg);
    char hdr[128], dur[32];
    format_duration_zh(today_s, dur, sizeof(dur));
    snprintf(hdr, sizeof(hdr), "我的書房 (%d本) · 今日 %s", g_book_count, dur);
    draw_str(mx, 18, hdr, 290, th->text);

    draw_clock_pill(th, w - 318, 18);

    /* + 資料夾 膠囊按鈕 */
    ui_fill_round_rect(w - 308, 12, 114, 32, 16, th->card_bg);
    ui_draw_round_rect(w - 308, 12, 114, 32, 16, th->accent);
    hal_set_text_bg(th->card_bg);
    draw_str(w - 294, 19, "+ 資料夾 [N]", 96, th->accent);

    int bx = w - 184;
    ui_fill_round_rect(bx, 12, 166, 32, 16, th->accent);
    hal_set_text_bg(th->accent);
    draw_str(bx + 18, 19, "+ 匯入書檔 [O]", 140, 0xFFFFFF);

    if (g_book_count == 0 && g_folder_count == 0) {
        int cw = mw - 40, ch = h - 108, cx = mx + 20, cy0 = 62;
        ui_card(cx, cy0, cw, ch, 16, th->card_bg, th->card_border, th->shadow, 0, th->accent);
        ui_icon_open_book(cx + cw / 2, cy0 + 78, th->accent, th->bg, th->card_border);

        int px = cx + cw / 2 - 144;
        ui_draw_badge(px,       cy0 + 126, "EPUB", ui_format_color(BOOK_FMT_EPUB), 0xFFFFFF);
        ui_draw_badge(px + 60,  cy0 + 126, "PDF",  ui_format_color(BOOK_FMT_PDF),  0xFFFFFF);
        ui_draw_badge(px + 114, cy0 + 126, "PNG",  ui_format_color(BOOK_FMT_PNG),  0xFFFFFF);
        ui_draw_badge(px + 168, cy0 + 126, "TXT",  ui_format_color(BOOK_FMT_TXT),  0xFFFFFF);
        ui_draw_badge(px + 222, cy0 + 126, "BMP",  ui_format_color(BOOK_FMT_BMP),  0xFFFFFF);

        hal_set_text_bg(th->card_bg);
        draw_str(cx + cw / 2 - 150, cy0 + 160, "書架資料夾分類 · EPUB 真封面插圖 · 365天統計", 380, th->text);
        draw_str(cx + cw / 2 - 196, cy0 + 190, "可建立系列資料夾（如鬼滅之刃）並自動接續閱讀下一卷！", 420, th->muted);

        int btn_w = 246, btn_h = 42, btn_x = cx + (cw - btn_w) / 2, btn_y = cy0 + 240;
        ui_fill_round_rect(btn_x, btn_y, btn_w, btn_h, 21, th->accent);
        hal_set_text_bg(th->accent);
        draw_str(btn_x + 28, btn_y + 12, "點擊或按 [Enter] 匯入新書", 210, 0xFFFFFF);
        return;
    }

    int has_hero = (g_book_count > 0) ? 1 : 0;
    if (has_hero) {
        BookEntry *hero = &g_books[0];
        int hero_y = 56, hero_h = 114;
        ui_card(mx, hero_y, mw, hero_h, 14, th->card_bg, th->card_border, th->shadow, (g_shelf_sel == 0), th->accent);
        draw_book_cover_smart(mx + 14, hero_y + 12, 72, 90, hero, th->is_dark);

        int tx = mx + 102, tw = mw - 240;
        hal_set_text_bg(th->card_bg);
        char htitle[96]; utf8_ellipsis(hero->title, tw / 9, htitle, sizeof(htitle));
        draw_str(tx, hero_y + 16, htitle, tw, th->text);

        char sub[128], bdur[32];
        format_duration_zh(hero->read_secs, bdur, sizeof(bdur));
        if (hero->folder[0]) snprintf(sub, sizeof(sub), "[%s] · %s · 已讀 %s", hero->folder, book_format_tag(hero->format), bdur);
        else if (hero->author[0]) snprintf(sub, sizeof(sub), "%s · %s · 已讀 %s", hero->author, book_format_tag(hero->format), bdur);
        else snprintf(sub, sizeof(sub), "%s · 累計閱讀 %s", book_format_tag(hero->format), bdur);
        draw_str(tx, hero_y + 42, sub, tw, th->muted);

        int hpct = calc_book_pct(hero);
        ui_progress_bar(tx, hero_y + 72, tw - 12, 8, hpct, th->track, th->accent);

        char pinfo[96];
        if (hero->page_count > 0) snprintf(pinfo, sizeof(pinfo), "進度 %d%%  (第 %d / %d 頁)", hpct, hero->page_idx + 1, hero->page_count);
        else snprintf(pinfo, sizeof(pinfo), "圖片書模式 / 點擊開啟");
        draw_str(tx, hero_y + 86, pinfo, tw, th->muted);

        int cta_x = mx + mw - 126, cta_y = hero_y + 39;
        uint32_t cta_bg = (g_shelf_sel == 0) ? th->accent : th->track;
        uint32_t cta_fg = (g_shelf_sel == 0) ? 0xFFFFFF : th->text;
        ui_fill_round_rect(cta_x, cta_y, 110, 36, 18, cta_bg);
        hal_set_text_bg(cta_bg);
        draw_str(cta_x + 18, cta_y + 9, "繼續閱讀", 80, cta_fg);
    }

    hal_set_text_bg(th->bg);
    draw_str(mx, 182, "資料夾與書籍  ([N]新資料夾 · [M]移入資料夾 · [D]移除)", mw, th->muted);

    int grid_y = 208, cols = 4, gap = 14;
    int card_w = (mw - gap * (cols - 1)) / cols;
    int card_h = h - grid_y - 34; if (card_h > 236) card_h = 236;

    int grid_sel = (g_shelf_sel >= has_hero) ? (g_shelf_sel - has_hero) : 0;
    int start_gi = (grid_sel / 4) * 4;

    for (int col = 0; col < cols; col++) {
        int gi = start_gi + col;
        if (gi >= gcnt) break;
        int cx = mx + col * (card_w + gap);
        int is_sel = (g_shelf_sel == gi + has_hero);

        ui_card(cx, grid_y, card_w, card_h, 12, th->card_bg, th->card_border, th->shadow, is_sel, th->accent);
        int cov_h = card_h - 82; if (cov_h < 68) cov_h = 68;

        if (gtypes[gi] == 1) {
            /* 繪製立體資料夾卡片 (自動取裡面第一本書的真封面當預覽圖!) */
            const char *fname = g_folders[gidxs[gi]];
            int fcnt = folder_count_books(fname);
            int ffirst = folder_first_book_idx(fname);
            int tw = 0, thh = 0;
            const uint8_t *trgb = (ffirst >= 0 && g_books[ffirst].format == BOOK_FMT_EPUB)
                                  ? get_cached_cover(g_books[ffirst].path, &tw, &thh) : NULL;
            ui_draw_folder_card(cx + 10, grid_y + 10, card_w - 20, cov_h, trgb, tw, thh, fcnt, th->accent, th->is_dark);

            hal_set_text_bg(th->card_bg);
            char fshort[64]; utf8_ellipsis(fname, (card_w - 20) / 9, fshort, sizeof(fshort));
            draw_str(cx + 10, grid_y + cov_h + 18, fshort, card_w - 20, th->text);

            char fsub[32]; snprintf(fsub, sizeof(fsub), "資料夾 · %d 本", fcnt);
            draw_str(cx + 10, grid_y + card_h - 32, fsub, card_w - 20, th->muted);
        } else {
            BookEntry *be = &g_books[gidxs[gi]];
            draw_book_cover_smart(cx + 10, grid_y + 10, card_w - 20, cov_h, be, th->is_dark);

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
    }

    hal_set_text_bg(th->bg);
    draw_str(mx, h - 24, "[1]書架  [2]管理  [3]年度統計  [4]外觀  [5/O]匯入  [N]新資料夾", mw, th->muted);
}

static void render_home_manage(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    hal_set_text_bg(th->bg);
    draw_str(mx, 18, "書庫管理  (點擊開啟 · 點右側 [刪] 或按 [D] 移出紀錄)", mw, th->text);
    if (g_book_count == 0) { draw_str(mx, 80, "目前沒有書籍紀錄，點擊左側 [匯入] 或按 [O] 新增。", mw, th->muted); return; }

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
        char tbuf[96], dstr[32];
        format_duration_zh(be->read_secs, dstr, sizeof(dstr));
        utf8_ellipsis(be->title, (mw - 240) / 9, tbuf, sizeof(tbuf));
        draw_str(mx + 70, ry + 10, tbuf, mw - 240, th->text);
        char sub[160], pbuf[100];
        utf8_ellipsis(be->path, 34, pbuf, sizeof(pbuf));
        if (be->folder[0]) snprintf(sub, sizeof(sub), "[%s] · 已讀 %s · %s", be->folder, dstr, pbuf);
        else snprintf(sub, sizeof(sub), "未分類 · 已讀 %s · %s", dstr, pbuf);
        draw_str(mx + 14, ry + 34, sub, mw - 150, th->muted);

        ui_fill_round_rect(mx + mw - 108, ry + 14, 46, 30, 8, th->accent);
        hal_set_text_bg(th->accent);
        draw_str(mx + mw - 96, ry + 20, "移", 24, 0xFFFFFF);

        ui_fill_round_rect(mx + mw - 56, ry + 14, 44, 30, 8, 0xEF4444);
        hal_set_text_bg(0xEF4444);
        draw_str(mx + mw - 44, ry + 20, "刪", 24, 0xFFFFFF);
    }
    hal_set_text_bg(th->bg);
    draw_str(mx, h - 24, "[1]書架  [2]管理  [3]年度統計  [4]外觀  [5/O]匯入  [ESC]離開", mw, th->muted);
}

/* ============================================================
 * 365 天全年每日閱讀統計與熱力圖 (GitHub 貢獻格風格！)
 * ============================================================ */
static void render_home_stats(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    int cy, cm, cd, chh, cmm, css;
    hal_get_local_time(&cy, &cm, &cd, &chh, &cmm, &css);

    /* 頂部標題與年份切換按鈕 */
    hal_set_text_bg(th->bg);
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "%d 年度閱讀足跡  (365天每日紀錄)", g_stats_year);
    draw_str(mx, 18, hdr, 380, th->text);

    ui_fill_round_rect(w - 178, 12, 74, 28, 8, th->card_bg);
    ui_draw_round_rect(w - 178, 12, 74, 28, 8, th->card_border);
    hal_set_text_bg(th->card_bg);
    draw_str(w - 166, 17, "< 去年", 56, th->text);

    ui_fill_round_rect(w - 96, 12, 74, 28, 8, th->card_bg);
    ui_draw_round_rect(w - 96, 12, 74, 28, 8, th->card_border);
    draw_str(w - 84, 17, "明年 >", 56, th->text);

    /* 1. 頂部 4 張現代 KPI 統計卡 */
    uint32_t today_s = 0, today_p = 0;
    stats_get_day(cy, cm, cd, &today_s, &today_p);
    int streak = stats_calc_streak(cy, cm, cd);
    uint32_t yr_s = stats_year_total_secs(g_stats_year);
    int yr_days = stats_year_active_days(g_stats_year);
    int tot_yr_days = stats_days_in_year(g_stats_year);

    char k_val[4][40];
    format_duration_zh(today_s, k_val[0], sizeof(k_val[0]));
    snprintf(k_val[1], sizeof(k_val[1]), "%d 天", streak);
    format_duration_zh(yr_s, k_val[2], sizeof(k_val[2]));
    snprintf(k_val[3], sizeof(k_val[3]), "%d / %d 天", yr_days, tot_yr_days);
    const char *k_lbl[4] = { "今日閱讀", "連續打卡", "今年累計", "打卡天數" };

    int gap = 12, kw = (mw - gap * 3) / 4, kh = 68, ky = 54;
    for (int i = 0; i < 4; i++) {
        int kx = mx + i * (kw + gap);
        ui_card(kx, ky, kw, kh, 12, th->card_bg, th->card_border, th->shadow, 0, th->accent);
        hal_set_text_bg(th->card_bg);
        draw_str(kx + 14, ky + 10, k_lbl[i], kw - 24, th->muted);
        draw_str(kx + 14, ky + 36, k_val[i], kw - 24, (i == 1 && streak > 0) ? th->accent : th->text);
    }

    /* 2. 中央 365 天年度熱力圖大卡片 (53週 × 7天) */
    int hm_y = 136, hm_h = 198;
    ui_card(mx, hm_y, mw, hm_h, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent);

    hal_set_text_bg(th->card_bg);
    draw_str(mx + 14, hm_y + 10, "全年每日熱力圖 (方向鍵或直接點擊方格查看當天紀錄)", mw - 160, th->text);

    /* 右上 5 階色階圖例 */
    uint32_t lv_cols[5] = {
        th->track,
        ui_blend_color(th->track, th->accent, 30),
        ui_blend_color(th->track, th->accent, 55),
        ui_blend_color(th->track, th->accent, 78),
        th->accent
    };
    ui_draw_ascii(mx + mw - 122, hm_y + 14, "0", 1, th->muted);
    for (int lv = 0; lv < 5; lv++) {
        hal_fill_rect(mx + mw - 108 + lv * 13, hm_y + 13, 10, 10, lv_cols[lv]);
    }
    ui_draw_ascii(mx + mw - 38, hm_y + 14, "60M+", 1, th->muted);

    /* 月份刻度 (1..12) + 星期標籤 (日/二/四/六) */
    int gx0 = mx + 36, gy0 = 186;
    int px_x = 12, px_y = 14;
    int jan1_dow = stats_day_of_week(g_stats_year, 1, 1);

    for (int m = 1; m <= 12; m++) {
        int m_doy = stats_day_of_year(g_stats_year, m, 1);
        int col = (m_doy + jan1_dow) / 7;
        char ml[8]; snprintf(ml, sizeof(ml), "%dM", m);
        ui_draw_ascii(gx0 + col * px_x, gy0 - 13, ml, 1, th->muted);
    }

    const char *wlbl[7] = { "日", "", "二", "", "四", "", "六" };
    for (int r = 0; r < 7; r++) {
        if (wlbl[r][0]) draw_str(mx + 12, gy0 + r * px_y - 2, wlbl[r], 20, th->muted);
    }

    /* 繪製全年 365/366 個每日熱力方格 */
    for (int doy = 0; doy < tot_yr_days; doy++) {
        int slot = doy + jan1_dow;
        int col = slot / 7;
        int row = slot % 7;
        int cx = gx0 + col * px_x;
        int cy_cell = gy0 + row * px_y;
        if (cx + 10 > mx + mw - 8) break;

        int dm = 1, dd = 1;
        stats_doy_to_ymd(g_stats_year, doy, &dm, &dd);
        uint32_t dsec = 0, dpg = 0;
        stats_get_day(g_stats_year, dm, dd, &dsec, &dpg);

        int lv = 0;
        if (dsec >= 3600u) lv = 4;
        else if (dsec >= 1800u) lv = 3;
        else if (dsec >= 900u) lv = 2;
        else if (dsec >= 10u || dpg > 0) lv = 1;

        hal_fill_rect(cx, cy_cell, 9, 10, lv_cols[lv]);

        if (doy == g_stats_doy) {
            hal_draw_rect(cx - 1, cy_cell - 1, 11, 12, th->is_dark ? 0xFFFFFF : 0x111111);
        }
    }

    /* 熱力圖下方選中日期的詳細數據條 */
    int sel_m = 1, sel_d = 1;
    stats_doy_to_ymd(g_stats_year, g_stats_doy, &sel_m, &sel_d);
    int sel_dow = stats_day_of_week(g_stats_year, sel_m, sel_d);
    const char *wd_names[7] = { "週日", "週一", "週二", "週三", "週四", "週五", "週六" };
    uint32_t s_sec = 0, s_pg = 0;
    stats_get_day(g_stats_year, sel_m, sel_d, &s_sec, &s_pg);

    ui_fill_round_rect(mx + 14, hm_y + hm_h - 42, mw - 28, 30, 8, th->bg);
    hal_set_text_bg(th->bg);
    char det[140], sdur[32];
    format_duration_zh(s_sec, sdur, sizeof(sdur));
    snprintf(det, sizeof(det), "選中日期：%04d-%02d-%02d (%s)  ·  閱讀 %s  ·  翻閱 %lu 頁",
             g_stats_year, sel_m, sel_d, wd_names[sel_dow], sdur, (unsigned long)s_pg);
    draw_str(mx + 26, hm_y + hm_h - 35, det, mw - 52, th->text);

    /* 3. 底部最常閱讀書籍卡片 */
    int bot_y = 346, bot_h = h - bot_y - 34;
    ui_card(mx, bot_y, mw, bot_h, 12, th->card_bg, th->card_border, th->shadow, 0, th->accent);
    hal_set_text_bg(th->card_bg);
    draw_str(mx + 14, bot_y + 10, "閱讀時長排行 (Top 2)：", 240, th->muted);

    int top_idx[2] = { -1, -1 };
    for (int i = 0; i < g_book_count; i++) {
        if (top_idx[0] < 0 || g_books[i].read_secs > g_books[top_idx[0]].read_secs) {
            top_idx[1] = top_idx[0]; top_idx[0] = i;
        } else if (top_idx[1] < 0 || g_books[i].read_secs > g_books[top_idx[1]].read_secs) {
            top_idx[1] = i;
        }
    }
    for (int r = 0; r < 2; r++) {
        if (top_idx[r] < 0) break;
        BookEntry *be = &g_books[top_idx[r]];
        char bdur[32], tshort[48], line[128];
        format_duration_zh(be->read_secs, bdur, sizeof(bdur));
        utf8_ellipsis(be->title, 26, tshort, sizeof(tshort));
        snprintf(line, sizeof(line), "%d. [%s] %s — 累計 %s (%d%%)",
                 r + 1, book_format_tag(be->format), tshort, bdur, calc_book_pct(be));
        draw_str(mx + 14, bot_y + 34 + r * 24, line, mw - 28, th->text);
    }

    hal_set_text_bg(th->bg);
    draw_str(mx, h - 24, "[↑↓←→]選日期  [PgUp/Dn]切換年份  [1]書架  [2]管理  [4]外觀  [ESC]離開", mw, th->muted);
}

static void render_home_appearance(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    hal_set_text_bg(th->bg);
    draw_str(mx, 18, "外觀與閱讀排版  (直接點擊色票卡片或 ←→ 即時切換)", mw, th->text);
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
    draw_str(mx + 18, py + 14, "排版預覽：EPUB 真封面插圖 + 365天閱讀熱力圖 + 頂部時鐘。", mw - 36, th->text);
    if (14 + lh + FONT_H < ph) draw_str(mx + 18, py + 14 + lh, "點左側翻上一頁、點右側翻下一頁、點中間開控制選單。", mw - 36, th->text);

    hal_set_text_bg(th->bg);
    draw_str(mx, h - 24, "[1]書架  [2]管理  [3]年度統計  [4]外觀  [5/O]匯入  [ESC]離開", mw, th->muted);
}

static void draw_bottom_touch_pills(const AppTheme *th, int w, int by) {
    const char *btns[5] = { "目錄", "書籤", "搜尋", "跳頁", "選單" };
    int start_x = w - 340;
    for (int i = 0; i < 5; i++) {
        int bx = start_x + i * 66;
        uint32_t bg = (i == 4) ? th->accent : th->card_bg;
        uint32_t fg = (i == 4) ? 0xFFFFFF : th->text;
        ui_fill_round_rect(bx, by + 6, 58, 26, 7, bg);
        if (i != 4) ui_draw_round_rect(bx, by + 6, 58, 26, 7, th->card_border);
        hal_set_text_bg(bg);
        draw_str(bx + 11, by + 10, btns[i], 42, fg);
    }
}

static void render_pdf_base(const AppTheme *th, int w, int h) {
    int top_bar = 36, bot_bar = 38;
    int tot = pdf_page_count(g_pdf);
    int pct = (tot > 0) ? (((g_pdf_page + 1) * 100) / tot) : 0;

    hal_clear(th->bg);
    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    hal_fill_rect(0, top_bar, w, 1, th->card_border);

    ui_fill_round_rect(10, 6, 68, 24, 7, th->card_bg);
    ui_draw_round_rect(10, 6, 68, 24, 7, th->card_border);
    hal_set_text_bg(th->card_bg);
    draw_str(18, 9, "< 書架", 56, th->text);

    ui_draw_badge(86, 10, "PDF", ui_format_color(BOOK_FMT_PDF), 0xFFFFFF);
    hal_set_text_bg(th->bar_bg);
    char tbuf[72]; utf8_ellipsis(g_doc.title, 22, tbuf, sizeof(tbuf));
    draw_str(132, 9, tbuf, 220, th->text);

    /* 右上縮放 [-][+] + [重排/原版] + 即時時鐘 */
    ui_fill_round_rect(w - 252, 6, 40, 24, 7, th->card_bg);
    ui_draw_round_rect(w - 252, 6, 40, 24, 7, th->card_border);
    ui_draw_ascii(w - 235, 14, "-", 1, th->text);

    ui_fill_round_rect(w - 204, 6, 40, 24, 7, th->card_bg);
    ui_draw_round_rect(w - 204, 6, 40, 24, 7, th->card_border);
    ui_draw_ascii(w - 187, 14, "+", 1, th->text);

    uint32_t rbg = g_pdf_reflow ? th->accent : th->card_bg;
    uint32_t rfg = g_pdf_reflow ? 0xFFFFFF : th->text;
    ui_fill_round_rect(w - 156, 6, 82, 24, 7, rbg);
    if (!g_pdf_reflow) ui_draw_round_rect(w - 156, 6, 82, 24, 7, th->card_border);
    hal_set_text_bg(rbg);
    draw_str(w - 148, 9, g_pdf_reflow ? "原版視圖" : "重排檢視", 72, rfg);

    draw_clock_pill(th, w - 10, 8);

    int by = h - bot_bar;
    hal_fill_rect(0, by, w, bot_bar, th->bar_bg);
    hal_fill_rect(0, by, w, 3, th->track);
    if (pct > 0) hal_fill_rect(0, by, (w * pct) / 100, 3, ui_format_color(BOOK_FMT_PDF));

    hal_set_text_bg(th->bar_bg);
    char foot_l[96];
    snprintf(foot_l, sizeof(foot_l), "第 %d/%d 頁 (%d%%) · %s", g_pdf_page + 1, tot, pct, PDF_ZOOM_NAMES[g_pdf_zoom]);
    draw_str(14, by + 11, foot_l, 340, th->text);
    draw_bottom_touch_pills(th, w, by);

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
            draw_str(32, top_bar + 32, "此頁為圖像/掃描 PDF 頁面，請點右上 [原版視圖] 或按 [R] 查看圖片！", w - 64, th->accent);
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
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) { render_pdf_base(th, w, h); return; }

    int top_bar = 36, bot_bar = 38;
    hal_clear(th->bg);

    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    hal_fill_rect(0, top_bar, w, 1, th->card_border);

    ui_fill_round_rect(10, 6, 68, 24, 7, th->card_bg);
    ui_draw_round_rect(10, 6, 68, 24, 7, th->card_border);
    hal_set_text_bg(th->card_bg);
    draw_str(18, 9, "< 書架", 56, th->text);

    ui_draw_badge(86, 10, book_format_tag(g_doc.format), ui_format_color(g_doc.format), 0xFFFFFF);
    hal_set_text_bg(th->bar_bg);
    char tbuf[80]; utf8_ellipsis(g_doc.title[0] ? g_doc.title : get_filename_only(g_filepath), 24, tbuf, sizeof(tbuf));
    draw_str(140, 9, tbuf, 240, th->text);

    const char *ch_name = get_current_chapter_name();
    if (ch_name && ch_name[0]) {
        char cbuf[64]; utf8_ellipsis(ch_name, 20, cbuf, sizeof(cbuf));
        draw_str(w - 280, 9, cbuf, 200, th->muted);
    }

    draw_clock_pill(th, w - 10, 8);

    int by = h - bot_bar;
    hal_fill_rect(0, by, w, bot_bar, th->bar_bg);
    int pct = (g_page_count > 0) ? (((g_current_page + 1) * 100) / g_page_count) : 0;
    hal_fill_rect(0, by, w, 3, th->track);
    if (pct > 0) hal_fill_rect(0, by, (w * pct) / 100, 3, th->accent);

    hal_set_text_bg(th->bar_bg);
    char foot_l[96];
    if (is_image_fmt(g_doc.format)) snprintf(foot_l, sizeof(foot_l), "圖片書 · 點左右翻上下張");
    else if (g_page_count > 0) snprintf(foot_l, sizeof(foot_l), "第 %d / %d 頁 (%d%%)", g_current_page + 1, g_page_count, pct);
    else snprintf(foot_l, sizeof(foot_l), "準備就緒");
    draw_str(14, by + 11, foot_l, 320, th->text);
    draw_bottom_touch_pills(th, w, by);

    if (is_image_fmt(g_doc.format)) {
        hal_draw_image_file(g_filepath, 0, top_bar + 1, w, h - top_bar - bot_bar - 1);
        return;
    }

    if (g_doc.text && g_page_offsets && g_page_count > 0) {
        size_t pos = g_page_offsets[g_current_page];

        /* EPUB 內嵌高清插圖專屬頁面！ */
        int img_id = -1;
        if (parse_img_marker(g_doc.text, g_doc.text_len, pos, &img_id, NULL)) {
            int iw = 0, ih = 0;
            const uint8_t *rgb = get_cached_illust(img_id, &iw, &ih);
            int area_y = top_bar + 12, area_h = h - top_bar - bot_bar - 44;
            int area_x = 24, area_w = w - 48;
            if (rgb && iw > 0 && ih > 0) {
                int dw = area_w, dh = area_h;
                if (iw * area_h > area_w * ih) dh = (area_w * ih) / iw;
                else dw = (area_h * iw) / ih;
                int dx = area_x + (area_w - dw) / 2;
                int dy = area_y + (area_h - dh) / 2;
                ui_card(dx - 6, dy - 6, dw + 12, dh + 12, 10, th->card_bg, th->card_border, th->shadow, 0, th->accent);
                hal_blit_rgb888_scaled(rgb, iw, ih, dx, dy, dw, dh, dx, dy, dw, dh, 0);
            } else {
                hal_set_text_bg(th->bg);
                draw_str(36, top_bar + 48, "【此插圖格式不支援解碼】", w - 72, th->muted);
            }
            hal_set_text_bg(th->bg);
            char cap[64];
            snprintf(cap, sizeof(cap), "EPUB 內嵌插圖  (%d / %d)", img_id + 1, g_doc.image_count);
            draw_str(w / 2 - 90, h - bot_bar - 26, cap, 220, th->muted);
            return;
        }

        int max_units, lines_per_page, line_h;
        get_layout_params(&max_units, &lines_per_page, &line_h);
        hal_set_text_bg(th->bg);
        int y = top_bar + 12;
        for (int line = 0; line < lines_per_page && pos < g_doc.text_len; line++) {
            if (parse_img_marker(g_doc.text, g_doc.text_len, pos, NULL, NULL)) break;
            size_t draw_len = 0;
            size_t step = scan_one_line_buf(g_doc.text, g_doc.text_len, pos, max_units, &draw_len);
            if (draw_len > 0) hal_draw_line_utf8(28, y, g_doc.text + pos, (int)draw_len, w - 56, th->text);
            y += line_h;
            if (step == 0) break;
            pos += step;
        }
    }
}

static void begin_modal_backdrop(int w, int h, int mx, int my, int mw, int mh) {
    hal_clear_text_in_rect(mx - 8, my - 6, mw + 16, mh + 12);
    hal_dim_rect(0, 0, w, h);
}

static void render_read_menu_overlay(const AppTheme *th, int w, int h) {
    int mw = 440, mh = 376, mx = (w - mw) / 2, my = (h - mh) / 2;
    begin_modal_backdrop(w, h, mx, my, mw, mh);

    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 42, 12, th->bar_bg);
    hal_fill_rect(mx + 2, my + 42, mw - 4, 1, th->card_border);

    hal_set_text_bg(th->bar_bg);
    draw_str(mx + 20, my + 12, "閱讀控制面板  (點項目或點外面關閉)", mw - 40, th->text);

    char items[8][64];
    snprintf(items[0], sizeof(items[0]), "繼續閱讀  (ESC)");
    snprintf(items[1], sizeof(items[1]), "章節 / 頁面目錄  (共 %d 項)", g_doc.chapter_count);
    snprintf(items[2], sizeof(items[2]), "書籤管理  (已存 %d 筆 · [K])", g_bmark_count);
    snprintf(items[3], sizeof(items[3]), "全文關鍵字搜尋  ([F])");
    snprintf(items[4], sizeof(items[4]), "主題配色：%s", th->name);
    if (g_doc.format == BOOK_FMT_PDF && g_pdf)
        snprintf(items[5], sizeof(items[5]), "PDF 縮放比例：%s", PDF_ZOOM_NAMES[g_pdf_zoom]);
    else
        snprintf(items[5], sizeof(items[5]), "正文行距：%s", SPACING_NAMES[g_spacing_idx]);
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
    begin_modal_backdrop(w, h, mx, my, mw, mh);

    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 44, 12, th->bar_bg);
    hal_fill_rect(mx + 2, my + 44, mw - 4, 1, th->card_border);

    hal_set_text_bg(th->bar_bg);
    char hdr[96]; snprintf(hdr, sizeof(hdr), "我的書籤 (%d / %d)", g_bmark_count, MAX_BOOKMARKS);
    draw_str(mx + 18, my + 13, hdr, 240, th->text);

    ui_fill_round_rect(mx + mw - 132, my + 8, 118, 28, 14, th->accent);
    hal_set_text_bg(th->accent);
    draw_str(mx + mw - 120, my + 13, "+ 新增本頁", 96, 0xFFFFFF);

    if (g_bmark_count == 0) {
        hal_set_text_bg(th->card_bg);
        draw_str(mx + 32, my + 110, "這本書還沒有手動書籤。", mw - 64, th->text);
        draw_str(mx + 32, my + 144, "點擊右上 [+ 新增本頁] 或按 [空白鍵] 加入目前這頁！", mw - 64, th->accent);
    } else {
        int row_h = 38, start = (g_bmark_sel >= 7) ? (g_bmark_sel - 6) : 0;
        for (int i = start; i < g_bmark_count && i < start + 7; i++) {
            int ry = my + 56 + (i - start) * row_h;
            int is_sel = (i == g_bmark_sel);
            uint32_t rbg = is_sel ? th->accent : th->bg;
            uint32_t rfg = is_sel ? 0xFFFFFF : th->text;
            ui_fill_round_rect(mx + 16, ry, mw - 78, 32, 8, rbg);
            hal_set_text_bg(rbg);
            char line[128];
            snprintf(line, sizeof(line), "P.%d (%d%%)  %s", g_bmarks[i].page_num, g_bmarks[i].pct, g_bmarks[i].label);
            draw_str(mx + 26, ry + 7, line, mw - 98, rfg);

            ui_fill_round_rect(mx + mw - 56, ry + 2, 40, 28, 7, 0xEF4444);
            hal_set_text_bg(0xEF4444);
            draw_str(mx + mw - 45, ry + 7, "刪", 24, 0xFFFFFF);
        }
    }
    hal_set_text_bg(th->card_bg);
    draw_str(mx + 18, my + mh - 28, "點擊跳至書籤 · 點 [刪] 移除 · 點外部或按 [ESC] 關閉", mw - 36, th->muted);
}

static void render_search_overlay(const AppTheme *th, int w, int h) {
    int mw = 580, mh = 380, mx = (w - mw) / 2, my = (h - mh) / 2;
    begin_modal_backdrop(w, h, mx, my, mw, mh);

    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 44, 12, th->bar_bg);
    hal_fill_rect(mx + 2, my + 44, mw - 4, 1, th->card_border);

    hal_set_text_bg(th->bar_bg);
    draw_str(mx + 18, my + 13, "全文關鍵字搜尋  (打字 + Enter 搜尋 · 點結果跳轉)", mw - 36, th->text);

    ui_fill_round_rect(mx + 18, my + 56, mw - 36, 36, 10, th->bg);
    ui_draw_round_rect(mx + 18, my + 56, mw - 36, 36, 10, th->accent);
    hal_set_text_bg(th->bg);
    char qdisp[96];
    if (g_search_query[0]) snprintf(qdisp, sizeof(qdisp), "搜尋：%s_", g_search_query);
    else snprintf(qdisp, sizeof(qdisp), "輸入關鍵字，或按 [1]第一章 [2]序章 [3]結論 [4]重點");
    draw_str(mx + 30, my + 65, qdisp, mw - 60, g_search_query[0] ? th->text : th->muted);

    int list_y = my + 104;
    if (!g_search_ran) {
        hal_set_text_bg(th->card_bg);
        draw_str(mx + 26, list_y + 20, "輸入關鍵字後按 [Enter] 開始檢索（支援 EPUB / PDF / TXT）。", mw - 52, th->muted);
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
            char line[128]; snprintf(line, sizeof(line), "[P.%d]  %s", g_search_hits[i].page_num, g_search_hits[i].snippet);
            draw_str(mx + 28, ry + 7, line, mw - 56, rfg);
        }
    }

    hal_set_text_bg(th->card_bg);
    char foot[96];
    if (g_search_ran) snprintf(foot, sizeof(foot), "共找到 %d 筆結果 · 點選項目或按 [Enter] 立即跳轉", g_search_cnt);
    else snprintf(foot, sizeof(foot), "[Enter]開始搜尋  [Backspace]刪字  點外部或[ESC]關閉");
    draw_str(mx + 18, my + mh - 28, foot, mw - 36, th->muted);
}

static void render_toc_page(const AppTheme *th, int w, int h) {
    hal_clear(th->bg);
    int top_bar = 44, bot_bar = 34;
    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    hal_fill_rect(0, top_bar, w, 1, th->card_border);
    hal_set_text_bg(th->bar_bg);
    char hdr[128]; snprintf(hdr, sizeof(hdr), "目錄  ·  %s  (共 %d 項)", g_doc.title, g_doc.chapter_count);
    draw_str(22, 12, hdr, w - 130, th->text);

    ui_fill_round_rect(w - 92, 8, 76, 28, 8, th->card_bg);
    ui_draw_round_rect(w - 92, 8, 76, 28, 8, th->card_border);
    hal_set_text_bg(th->card_bg);
    draw_str(w - 76, 13, "返回", 50, th->text);

    hal_fill_rect(0, h - bot_bar, w, bot_bar, th->bar_bg);
    hal_set_text_bg(th->bar_bg);
    draw_str(22, h - bot_bar + 8, "點項目跳轉 · 點底部：[左]上捲  [中]下捲  [右]返回閱讀", w - 44, th->muted);

    if (g_doc.chapter_count == 0) {
        ui_card(80, 110, w - 160, 140, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent);
        hal_set_text_bg(th->card_bg);
        draw_str(110, 148, "本書未偵測到章節標題，請用 [跳頁] 滑桿。", w - 220, th->text);
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
    int bw = 460, bh = 160, bx = (w - bw) / 2, by = (h - bh) / 2;
    begin_modal_backdrop(w, h, bx, by, bw, bh);

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

    ui_fill_round_rect(bx + 24, by + 106, 196, 36, 10, th->accent);
    hal_set_text_bg(th->accent);
    draw_str(bx + 76, by + 115, "確認跳轉", 100, 0xFFFFFF);

    ui_fill_round_rect(bx + 240, by + 106, 196, 36, 10, th->bar_bg);
    ui_draw_round_rect(bx + 240, by + 106, 196, 36, 10, th->card_border);
    hal_set_text_bg(th->bar_bg);
    draw_str(bx + 314, by + 115, "取消", 60, th->text);
}

static void render_new_folder_overlay(const AppTheme *th, int w, int h) {
    int mw = 480, mh = 250, mx = (w - mw) / 2, my = (h - mh) / 2;
    begin_modal_backdrop(w, h, mx, my, mw, mh);

    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 42, 12, th->bar_bg);
    hal_fill_rect(mx + 2, my + 42, mw - 4, 1, th->card_border);

    hal_set_text_bg(th->bar_bg);
    draw_str(mx + 18, my + 12, "新增書架資料夾  (輸入名稱或點下方快捷分類)", mw - 36, th->text);

    ui_fill_round_rect(mx + 18, my + 58, mw - 36, 38, 10, th->bg);
    ui_draw_round_rect(mx + 18, my + 58, mw - 36, 38, 10, th->accent);
    hal_set_text_bg(th->bg);
    char disp[96];
    if (g_new_folder_buf[0]) snprintf(disp, sizeof(disp), "名稱：%s_", g_new_folder_buf);
    else snprintf(disp, sizeof(disp), "請用鍵盤輸入英文/數字，或直接點/按 [1~4] 快捷建立");
    draw_str(mx + 28, my + 68, disp, mw - 56, g_new_folder_buf[0] ? th->text : th->muted);

    const char *presets[4] = { "1.鬼滅之刃", "2.漫畫", "3.小說", "4.講義" };
    int bw = (mw - 60) / 4;
    for (int i = 0; i < 4; i++) {
        int px = mx + 18 + i * (bw + 8);
        ui_fill_round_rect(px, my + 114, bw, 34, 8, th->bar_bg);
        ui_draw_round_rect(px, my + 114, bw, 34, 8, th->card_border);
        hal_set_text_bg(th->bar_bg);
        draw_str(px + 8, my + 122, presets[i], bw - 12, th->text);
    }

    ui_fill_round_rect(mx + 24, my + 188, 200, 38, 10, th->accent);
    hal_set_text_bg(th->accent);
    draw_str(mx + 82, my + 198, "確認建立", 90, 0xFFFFFF);

    ui_fill_round_rect(mx + 256, my + 188, 200, 38, 10, th->bar_bg);
    ui_draw_round_rect(mx + 256, my + 188, 200, 38, 10, th->card_border);
    hal_set_text_bg(th->bar_bg);
    draw_str(mx + 332, my + 198, "取消", 60, th->text);
}

static void render_move_folder_overlay(const AppTheme *th, int w, int h) {
    int mw = 440, mh = 340, mx = (w - mw) / 2, my = (h - mh) / 2;
    begin_modal_backdrop(w, h, mx, my, mw, mh);

    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 44, 12, th->bar_bg);
    hal_fill_rect(mx + 2, my + 44, mw - 4, 1, th->card_border);

    hal_set_text_bg(th->bar_bg);
    char hdr[96], tshort[40];
    if (g_move_book_idx >= 0 && g_move_book_idx < g_book_count)
        utf8_ellipsis(g_books[g_move_book_idx].title, 18, tshort, sizeof(tshort));
    else tshort[0] = '\0';
    snprintf(hdr, sizeof(hdr), "移動至資料夾：%s", tshort);
    draw_str(mx + 18, my + 13, hdr, mw - 36, th->text);

    int total_opts = g_folder_count + 1;
    int start = (g_move_folder_sel >= 6) ? (g_move_folder_sel - 5) : 0;
    for (int i = start; i < total_opts && i < start + 6; i++) {
        int ry = my + 56 + (i - start) * 38;
        int is_sel = (i == g_move_folder_sel);
        uint32_t rbg = is_sel ? th->accent : th->bg;
        uint32_t rfg = is_sel ? 0xFFFFFF : th->text;
        ui_fill_round_rect(mx + 16, ry, mw - 32, 32, 8, rbg);
        hal_set_text_bg(rbg);
        if (i == 0) {
            draw_str(mx + 28, ry + 7, "移出資料夾 (放回外層主書架)", mw - 56, rfg);
        } else {
            char line[96];
            snprintf(line, sizeof(line), "%d.  %s  (%d 本)", i, g_folders[i - 1], folder_count_books(g_folders[i - 1]));
            draw_str(mx + 28, ry + 7, line, mw - 56, rfg);
        }
    }

    hal_set_text_bg(th->card_bg);
    draw_str(mx + 18, my + mh - 28, "點擊或按 [Enter] 確認移動 · [ESC] 取消", mw - 36, th->muted);
}

static void render_next_vol_overlay(const AppTheme *th, int w, int h) {
    int bw = 460, bh = 170, bx = (w - bw) / 2, by = (h - bh) / 2;
    begin_modal_backdrop(w, h, bx, by, bw, bh);

    ui_card(bx, by, bw, bh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    hal_set_text_bg(th->card_bg);
    draw_str(bx + 24, by + 22, "本卷已讀完！是否接著閱讀同資料夾下一卷？", bw - 48, th->text);

    if (g_next_vol_idx >= 0 && g_next_vol_idx < g_book_count) {
        char ntitle[80];
        utf8_ellipsis(g_books[g_next_vol_idx].title, 34, ntitle, sizeof(ntitle));
        draw_str(bx + 24, by + 58, ntitle, bw - 48, th->accent);
    }

    ui_fill_round_rect(bx + 24, by + 112, 196, 38, 10, th->accent);
    hal_set_text_bg(th->accent);
    draw_str(bx + 62, by + 122, "立即閱讀下一卷", 120, 0xFFFFFF);

    ui_fill_round_rect(bx + 240, by + 112, 196, 38, 10, th->bar_bg);
    ui_draw_round_rect(bx + 240, by + 112, 196, 38, 10, th->card_border);
    hal_set_text_bg(th->bar_bg);
    draw_str(bx + 300, by + 122, "留在本頁", 80, th->text);
}

void reader_render(void) {
    const AppTheme *th = &THEMES[g_theme_idx];
    int w = hal_get_width(), h = hal_get_height();

    if (g_screen == SCR_HOME || g_screen == SCR_NEW_FOLDER || g_screen == SCR_MOVE_FOLDER) {
        hal_clear(th->bg);
        render_left_rail(th, h);
        if (g_home_tab == TAB_SHELF) render_home_shelf(th, w, h);
        else if (g_home_tab == TAB_MANAGE) render_home_manage(th, w, h);
        else if (g_home_tab == TAB_STATS) render_home_stats(th, w, h);
        else render_home_appearance(th, w, h);
        if (g_screen == SCR_NEW_FOLDER) render_new_folder_overlay(th, w, h);
        else if (g_screen == SCR_MOVE_FOLDER) render_move_folder_overlay(th, w, h);
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
    else if (g_screen == SCR_NEXT_VOL) render_next_vol_overlay(th, w, h);

    hal_present();
}
