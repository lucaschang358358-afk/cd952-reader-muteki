#include "reader_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
static FILE *open_utf8_file(const char *path, const char *mode) {
    int plen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    int mlen = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (plen <= 0 || mlen <= 0) return NULL;
    wchar_t *wpath = (wchar_t *)malloc((size_t)plen * sizeof(wchar_t));
    wchar_t *wmode = (wchar_t *)malloc((size_t)mlen * sizeof(wchar_t));
    if (!wpath || !wmode) {
        free(wpath);
        free(wmode);
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, plen);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, mlen);
    FILE *f = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return f;
}
#else
static FILE *open_utf8_file(const char *path, const char *mode) {
    return fopen(path, mode);
}
#endif

typedef enum {
    SCREEN_SHELF = 0,
    SCREEN_READ_TXT,
    SCREEN_READ_BMP,
    SCREEN_TOC,
    SCREEN_JUMP
} ScreenMode;

typedef struct {
    const char *name;
    uint32_t bg;
    uint32_t text;
    uint32_t bar_bg;
    uint32_t bar_text;
    uint32_t sel_bg;
    uint32_t sel_text;
    uint32_t muted_text;
} Theme;

static const Theme THEMES[4] = {
    /* 0: 日間暖紙色 */
    { "日間紙色", 0xF7F4EC, 0x222222, 0xE5DFD0, 0x333333, 0xD2C4A8, 0x111111, 0x666055 },
    /* 1: 夜間深色 */
    { "夜間深色", 0x1A1B1E, 0xD8D8D8, 0x27292E, 0xB8BCC4, 0x3A4659, 0xFFFFFF, 0x8E94A0 },
    /* 2: 護眼豆沙綠 */
    { "護眼綠",   0xCCE8CF, 0x1E3322, 0xB4D6B8, 0x1B2E1F, 0x95C29B, 0x0F1F12, 0x4A6B50 },
    /* 3: 純黑高對比 */
    { "純黑模式", 0x000000, 0xFFFFFF, 0x1E1E1E, 0xCCCCCC, 0x335588, 0xFFFFFF, 0x999999 }
};

#define MAX_BOOKS 32
#define MAX_CHAPTERS 512

typedef struct {
    char path[512];
    size_t byte_offset;
    size_t total_bytes;
    int page_idx;
    int page_count;
} BookEntry;

typedef struct {
    size_t offset;
    char title[84];
} ChapterEntry;

static ScreenMode g_screen = SCREEN_SHELF;
static int g_theme_idx = 0;
static int g_font_size = 20;
static int g_spacing_idx = 1; /* 0=緊湊(+4), 1=標準(+8), 2=寬鬆(+14) */
static const int SPACING_PX[3] = { 4, 8, 14 };
static const char *SPACING_NAMES[3] = { "緊湊", "標準", "寬鬆" };

/* 書架狀態 */
static BookEntry g_books[MAX_BOOKS];
static int g_book_count = 0;
static int g_shelf_sel = 0;
static int g_shelf_scroll = 0;

/* 當前開啟書籍 */
static char g_filepath[512] = {0};
static char *g_text_buf = NULL;
static size_t g_text_len = 0;
static size_t *g_page_offsets = NULL;
static int g_page_count = 0;
static int g_current_page = 0;

/* 目錄狀態 */
static ChapterEntry g_chapters[MAX_CHAPTERS];
static int g_chapter_count = 0;
static int g_toc_sel = 0;
static int g_toc_scroll = 0;

/* 快速跳頁狀態 */
static int g_jump_target = 0;

/* --- 工具函式 --- */
static int utf8_char_info(const unsigned char *s, size_t remaining, int *out_units) {
    if (remaining == 0) { *out_units = 0; return 0; }
    unsigned char c = s[0];
    if ((c & 0x80) == 0) {
        *out_units = 1;
        return 1;
    } else if ((c & 0xE0) == 0xC0 && remaining >= 2) {
        *out_units = 2;
        return 2;
    } else if ((c & 0xF0) == 0xE0 && remaining >= 3) {
        *out_units = 2;
        return 3;
    } else if ((c & 0xF8) == 0xF0 && remaining >= 4) {
        *out_units = 2;
        return 4;
    }
    *out_units = 1;
    return 1;
}

static int ends_with_ci(const char *str, const char *suffix) {
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    if (slen < xlen) return 0;
    const char *p = str + slen - xlen;
    for (size_t i = 0; i < xlen; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)suffix[i])) return 0;
    }
    return 1;
}

static const char *get_filename_only(const char *path) {
    const char *p1 = strrchr(path, '\\');
    const char *p2 = strrchr(path, '/');
    const char *p = (p1 > p2) ? p1 : p2;
    return p ? (p + 1) : path;
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

/* --- 書架索引讀寫 --- */
static void save_library(void) {
    const char *lib_path = hal_get_library_path();
    if (!lib_path || !lib_path[0]) return;
    FILE *f = open_utf8_file(lib_path, "wb");
    if (!f) return;
    fprintf(f, "#CFG|%d|%d\n", g_theme_idx, g_spacing_idx);
    for (int i = 0; i < g_book_count; i++) {
        fprintf(f, "%zu|%zu|%d|%d|%s\n",
                g_books[i].byte_offset,
                g_books[i].total_bytes,
                g_books[i].page_idx,
                g_books[i].page_count,
                g_books[i].path);
    }
    fclose(f);
}

static void load_library(void) {
    g_book_count = 0;
    const char *lib_path = hal_get_library_path();
    if (!lib_path || !lib_path[0]) return;
    FILE *f = open_utf8_file(lib_path, "rb");
    if (!f) return;

    char line[700];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        if (strncmp(line, "#CFG|", 5) == 0) {
            int th = 0, sp = 1;
            if (sscanf(line + 5, "%d|%d", &th, &sp) == 2) {
                if (th >= 0 && th < 4) g_theme_idx = th;
                if (sp >= 0 && sp < 3) g_spacing_idx = sp;
            }
            continue;
        }

        if (g_book_count >= MAX_BOOKS) break;

        size_t boff = 0, btot = 0;
        int pidx = 0, pcnt = 0;
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        char *p2 = strchr(p1 + 1, '|');
        if (!p2) continue;
        char *p3 = strchr(p2 + 1, '|');
        if (!p3) continue;
        char *p4 = strchr(p3 + 1, '|');
        if (!p4) continue;

        boff = (size_t)strtoull(line, NULL, 10);
        btot = (size_t)strtoull(p1 + 1, NULL, 10);
        pidx = atoi(p2 + 1);
        pcnt = atoi(p3 + 1);

        BookEntry *be = &g_books[g_book_count++];
        memset(be, 0, sizeof(*be));
        be->byte_offset = boff;
        be->total_bytes = btot;
        be->page_idx = pidx;
        be->page_count = pcnt;
        snprintf(be->path, sizeof(be->path), "%s", p4 + 1);
    }
    fclose(f);
}

static void update_book_in_library(const char *path, size_t boff, size_t btot, int pidx, int pcnt) {
    if (!path || !path[0]) return;
    int found = -1;
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].path, path) == 0) {
            found = i;
            break;
        }
    }

    BookEntry cur;
    memset(&cur, 0, sizeof(cur));
    snprintf(cur.path, sizeof(cur.path), "%s", path);
    cur.byte_offset = boff;
    cur.total_bytes = btot;
    cur.page_idx = pidx;
    cur.page_count = pcnt;

    if (found >= 0) {
        /* 移到書架最上方（最近閱讀） */
        for (int i = found; i > 0; i--) {
            g_books[i] = g_books[i - 1];
        }
        g_books[0] = cur;
    } else {
        int move_n = (g_book_count < MAX_BOOKS) ? g_book_count : (MAX_BOOKS - 1);
        for (int i = move_n; i > 0; i--) {
            g_books[i] = g_books[i - 1];
        }
        g_books[0] = cur;
        if (g_book_count < MAX_BOOKS) g_book_count++;
    }
    g_shelf_sel = 0;
    g_shelf_scroll = 0;
    save_library();
}

static void remove_selected_book(void) {
    if (g_book_count <= 0 || g_shelf_sel < 0 || g_shelf_sel >= g_book_count) return;
    for (int i = g_shelf_sel; i + 1 < g_book_count; i++) {
        g_books[i] = g_books[i + 1];
    }
    g_book_count--;
    if (g_shelf_sel >= g_book_count && g_book_count > 0) {
        g_shelf_sel = g_book_count - 1;
    }
    if (g_book_count == 0) {
        g_shelf_sel = 0;
        g_shelf_scroll = 0;
    }
    save_library();
}

/* --- 單本書籤讀寫 (<filepath>.bmk) --- */
static void save_bookmark(void) {
    if (g_filepath[0] == '\0') return;
    if (g_screen == SCREEN_READ_TXT || g_screen == SCREEN_TOC || g_screen == SCREEN_JUMP) {
        if (g_page_count <= 0 || !g_page_offsets) return;
        size_t off = g_page_offsets[g_current_page];
        char bmk_path[540];
        snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", g_filepath);
        FILE *f = open_utf8_file(bmk_path, "wb");
        if (f) {
            fprintf(f, "%zu", off);
            fclose(f);
        }
        update_book_in_library(g_filepath, off, g_text_len, g_current_page, g_page_count);
    } else if (g_screen == SCREEN_READ_BMP) {
        update_book_in_library(g_filepath, 0, 0, 0, 0);
    }
}

static size_t load_bookmark(const char *path) {
    if (!path || path[0] == '\0') return 0;
    char bmk_path[540];
    snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", path);
    FILE *f = open_utf8_file(bmk_path, "rb");
    if (f) {
        size_t off = 0;
        if (fscanf(f, "%zu", &off) == 1) {
            fclose(f);
            return off;
        }
        fclose(f);
    }
    /* 若無 .bmk，回退查詢書架紀錄 */
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].path, path) == 0) {
            return g_books[i].byte_offset;
        }
    }
    return 0;
}

/* --- 自動掃描章節目錄 --- */
static int is_chapter_line(const char *s, size_t len) {
    if (len == 0 || len > 96) return 0;
    /* 跳過開頭空白與全形空白 (E3 80 80) */
    size_t i = 0;
    while (i < len) {
        if (s[i] == ' ' || s[i] == '\t') {
            i++;
        } else if (i + 2 < len &&
                   (unsigned char)s[i] == 0xE3 &&
                   (unsigned char)s[i + 1] == 0x80 &&
                   (unsigned char)s[i + 2] == 0x80) {
            i += 3;
        } else {
            break;
        }
    }
    if (i >= len) return 0;
    const char *p = s + i;
    size_t rem = len - i;

    /* 英文 Chapter / CHAPTER / Part */
    if (rem >= 7 && (strncmp(p, "Chapter", 7) == 0 || strncmp(p, "CHAPTER", 7) == 0 || strncmp(p, "chapter", 7) == 0)) {
        return 1;
    }
    /* 中文「第」開頭，且後面 24 bytes 內有「章 / 回 / 節 / 卷 / 集 / 部」 */
    if (rem >= 6 &&
        (unsigned char)p[0] == 0xE7 && (unsigned char)p[1] == 0xAC && (unsigned char)p[2] == 0xAC) {
        size_t limit = rem < 30 ? rem : 30;
        for (size_t k = 3; k + 2 < limit; k++) {
            unsigned char b0 = (unsigned char)p[k];
            unsigned char b1 = (unsigned char)p[k + 1];
            unsigned char b2 = (unsigned char)p[k + 2];
            /* 章(E7 AB A0), 回(E5 9B 9E), 節(E7 AF 80), 节(E8 8A 82), 卷(E5 8D B7), 集(E9 9B 86), 部(E9 83 A8) */
            if ((b0 == 0xE7 && b1 == 0xAB && b2 == 0xA0) ||
                (b0 == 0xE5 && b1 == 0x9B && b2 == 0x9E) ||
                (b0 == 0xE7 && b1 == 0xAF && b2 == 0x80) ||
                (b0 == 0xE8 && b1 == 0x8A && b2 == 0x82) ||
                (b0 == 0xE5 && b1 == 0x8D && b2 == 0xB7) ||
                (b0 == 0xE9 && b1 == 0x9B && b2 == 0x86) ||
                (b0 == 0xE9 && b1 == 0x83 && b2 == 0xA8)) {
                return 1;
            }
        }
    }
    /* 常見特殊章節：序章 / 前言 / 楔子 / 尾聲 / 番外 / 後記 */
    const char *specials[] = { "序章", "前言", "楔子", "尾聲", "尾声", "番外", "後記", "后记", "目錄", "目录" };
    for (int j = 0; j < (int)(sizeof(specials) / sizeof(specials[0])); j++) {
        size_t slen = strlen(specials[j]);
        if (rem >= slen && strncmp(p, specials[j], slen) == 0 && rem <= 36) {
            return 1;
        }
    }
    return 0;
}

static void scan_chapters(void) {
    g_chapter_count = 0;
    if (!g_text_buf || g_text_len == 0) return;

    size_t pos = 0;
    while (pos < g_text_len && g_chapter_count < MAX_CHAPTERS) {
        size_t line_start = pos;
        while (pos < g_text_len && g_text_buf[pos] != '\n' && g_text_buf[pos] != '\r') {
            pos++;
        }
        size_t line_len = pos - line_start;
        if (is_chapter_line(g_text_buf + line_start, line_len)) {
            ChapterEntry *ce = &g_chapters[g_chapter_count++];
            ce->offset = line_start;
            trim_copy(ce->title, sizeof(ce->title), g_text_buf + line_start, line_len);
        }
        if (pos < g_text_len && g_text_buf[pos] == '\r') pos++;
        if (pos < g_text_len && g_text_buf[pos] == '\n') pos++;
    }
}

/* --- 排版與分頁 --- */
static size_t scan_one_line(size_t start_off, int max_units, size_t *out_draw_len) {
    size_t pos = start_off;
    int units = 0;
    while (pos < g_text_len) {
        char ch = g_text_buf[pos];
        if (ch == '\r') {
            *out_draw_len = pos - start_off;
            pos++;
            if (pos < g_text_len && g_text_buf[pos] == '\n') pos++;
            return pos - start_off;
        }
        if (ch == '\n') {
            *out_draw_len = pos - start_off;
            return (pos - start_off) + 1;
        }
        int u = 0;
        int clen = utf8_char_info((const unsigned char *)(g_text_buf + pos), g_text_len - pos, &u);
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
    int w = hal_get_width();
    int h = hal_get_height();
    int pad_x = 16;
    int top_bar = 30;
    int bot_bar = 30;
    int line_h = g_font_size + SPACING_PX[g_spacing_idx];

    int usable_w = w - pad_x * 2;
    if (usable_w < 100) usable_w = 100;
    int max_units = (usable_w * 2) / (g_font_size + 1);
    if (max_units < 10) max_units = 10;

    int usable_h = h - top_bar - bot_bar - 12;
    int lines = usable_h / line_h;
    if (lines < 1) lines = 1;

    *out_max_units = max_units;
    *out_lines_per_page = lines;
    *out_line_h = line_h;
}

static void jump_to_offset(size_t target_offset) {
    g_current_page = 0;
    for (int i = 0; i < g_page_count; i++) {
        if (g_page_offsets[i] <= target_offset) {
            g_current_page = i;
        } else {
            break;
        }
    }
}

static void repaginate(size_t target_offset) {
    if (!g_text_buf) return;

    int max_units, lines_per_page, line_h;
    get_layout_params(&max_units, &lines_per_page, &line_h);

    free(g_page_offsets);
    g_page_offsets = NULL;
    g_page_count = 0;

    int cap = 64;
    g_page_offsets = (size_t *)malloc((size_t)cap * sizeof(size_t));
    if (!g_page_offsets) return;

    size_t pos = 0;
    while (pos < g_text_len || g_page_count == 0) {
        if (g_page_count >= cap) {
            cap *= 2;
            size_t *tmp = (size_t *)realloc(g_page_offsets, (size_t)cap * sizeof(size_t));
            if (!tmp) break;
            g_page_offsets = tmp;
        }
        g_page_offsets[g_page_count++] = pos;

        for (int line = 0; line < lines_per_page && pos < g_text_len; line++) {
            size_t draw_len = 0;
            size_t step = scan_one_line(pos, max_units, &draw_len);
            if (step == 0) break;
            pos += step;
        }
        if (pos >= g_text_len) break;
    }

    jump_to_offset(target_offset);
}

/* --- 公開介面 --- */
void reader_init(void) {
    load_library();
    g_screen = SCREEN_SHELF;
}

void reader_shutdown(void) {
    save_bookmark();
    save_library();
    free(g_text_buf);
    free(g_page_offsets);
    g_text_buf = NULL;
    g_page_offsets = NULL;
}

int reader_open_file(const char *filepath) {
    if (!filepath || filepath[0] == '\0') return 0;

    if (g_screen == SCREEN_READ_TXT || g_screen == SCREEN_TOC || g_screen == SCREEN_JUMP) {
        save_bookmark();
    }

    if (ends_with_ci(filepath, ".bmp")) {
        snprintf(g_filepath, sizeof(g_filepath), "%s", filepath);
        g_screen = SCREEN_READ_BMP;
        update_book_in_library(g_filepath, 0, 0, 0, 0);
        return 1;
    }

    FILE *f = open_utf8_file(filepath, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return 0;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t read_n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[read_n] = '\0';

    size_t start = 0;
    if (read_n >= 3 &&
        (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        start = 3;
    }

    free(g_text_buf);
    if (start > 0) {
        g_text_len = read_n - start;
        g_text_buf = (char *)malloc(g_text_len + 1);
        if (g_text_buf) {
            memcpy(g_text_buf, buf + start, g_text_len);
            g_text_buf[g_text_len] = '\0';
        }
        free(buf);
    } else {
        g_text_buf = buf;
        g_text_len = read_n;
    }

    snprintf(g_filepath, sizeof(g_filepath), "%s", filepath);
    g_screen = SCREEN_READ_TXT;

    scan_chapters();
    size_t bmk = load_bookmark(g_filepath);
    repaginate(bmk);
    save_bookmark();
    return 1;
}

void reader_on_resize(void) {
    if ((g_screen == SCREEN_READ_TXT || g_screen == SCREEN_TOC || g_screen == SCREEN_JUMP) &&
        g_page_offsets && g_page_count > 0) {
        size_t cur_off = g_page_offsets[g_current_page];
        repaginate(cur_off);
    }
}

static void open_new_book_via_dialog(void) {
    char path[512] = {0};
    if (hal_open_file_dialog(path, sizeof(path))) {
        reader_open_file(path);
    }
}

/* --- 按鍵處理 --- */
void reader_on_key(ReaderKey key) {
    /* 共用配色切換 */
    if (key == RKEY_THEME) {
        g_theme_idx = (g_theme_idx + 1) % 4;
        save_library();
        return;
    }

    /* 1. 書架頁面 */
    if (g_screen == SCREEN_SHELF) {
        switch (key) {
            case RKEY_UP:
            case RKEY_LEFT:
            case RKEY_PGUP:
                if (g_shelf_sel > 0) g_shelf_sel--;
                break;
            case RKEY_DOWN:
            case RKEY_RIGHT:
            case RKEY_PGDN:
                if (g_shelf_sel + 1 < g_book_count) g_shelf_sel++;
                break;
            case RKEY_OK:
            case RKEY_SPACE:
                if (g_book_count > 0) {
                    char target[512];
                    snprintf(target, sizeof(target), "%s", g_books[g_shelf_sel].path);
                    reader_open_file(target);
                } else {
                    open_new_book_via_dialog();
                }
                break;
            case RKEY_OPEN:
                open_new_book_via_dialog();
                break;
            case RKEY_DELETE:
                remove_selected_book();
                break;
            case RKEY_BACK:
                hal_request_quit();
                break;
            default:
                break;
        }
        return;
    }

    /* 2. 章節目錄頁面 */
    if (g_screen == SCREEN_TOC) {
        switch (key) {
            case RKEY_UP:
            case RKEY_LEFT:
                if (g_toc_sel > 0) g_toc_sel--;
                break;
            case RKEY_DOWN:
            case RKEY_RIGHT:
                if (g_toc_sel + 1 < g_chapter_count) g_toc_sel++;
                break;
            case RKEY_PGUP:
                g_toc_sel -= 8;
                if (g_toc_sel < 0) g_toc_sel = 0;
                break;
            case RKEY_PGDN:
                g_toc_sel += 8;
                if (g_toc_sel >= g_chapter_count) g_toc_sel = g_chapter_count > 0 ? g_chapter_count - 1 : 0;
                break;
            case RKEY_OK:
            case RKEY_SPACE:
                if (g_chapter_count > 0) {
                    jump_to_offset(g_chapters[g_toc_sel].offset);
                    g_screen = SCREEN_READ_TXT;
                    save_bookmark();
                } else {
                    g_screen = SCREEN_READ_TXT;
                }
                break;
            case RKEY_BACK:
            case RKEY_TOC:
            case RKEY_SHELF:
                g_screen = SCREEN_READ_TXT;
                break;
            default:
                break;
        }
        return;
    }

    /* 3. 快速跳頁模式 */
    if (g_screen == SCREEN_JUMP) {
        switch (key) {
            case RKEY_LEFT:  g_jump_target -= 10; break;
            case RKEY_RIGHT: g_jump_target += 10; break;
            case RKEY_UP:    g_jump_target -= 50; break;
            case RKEY_DOWN:  g_jump_target += 50; break;
            case RKEY_PGUP:  g_jump_target -= 1;  break;
            case RKEY_PGDN:  g_jump_target += 1;  break;
            case RKEY_OK:
            case RKEY_SPACE:
                if (g_jump_target >= 0 && g_jump_target < g_page_count) {
                    g_current_page = g_jump_target;
                    save_bookmark();
                }
                g_screen = SCREEN_READ_TXT;
                return;
            case RKEY_BACK:
            case RKEY_JUMP:
                g_screen = SCREEN_READ_TXT;
                return;
            default:
                return;
        }
        if (g_jump_target < 0) g_jump_target = 0;
        if (g_jump_target >= g_page_count) g_jump_target = g_page_count - 1;
        return;
    }

    /* 4. 圖片閱讀模式 */
    if (g_screen == SCREEN_READ_BMP) {
        switch (key) {
            case RKEY_LEFT:
            case RKEY_UP:
            case RKEY_PGUP: {
                char next_path[512] = {0};
                if (hal_find_sibling_bmp(g_filepath, -1, next_path, sizeof(next_path))) {
                    snprintf(g_filepath, sizeof(g_filepath), "%s", next_path);
                }
                break;
            }
            case RKEY_RIGHT:
            case RKEY_DOWN:
            case RKEY_PGDN:
            case RKEY_SPACE: {
                char next_path[512] = {0};
                if (hal_find_sibling_bmp(g_filepath, 1, next_path, sizeof(next_path))) {
                    snprintf(g_filepath, sizeof(g_filepath), "%s", next_path);
                }
                break;
            }
            case RKEY_OPEN:
                open_new_book_via_dialog();
                break;
            case RKEY_BACK:
            case RKEY_SHELF:
                save_bookmark();
                g_screen = SCREEN_SHELF;
                break;
            default:
                break;
        }
        return;
    }

    /* 5. TXT 閱讀模式 */
    switch (key) {
        case RKEY_LEFT:
        case RKEY_UP:
        case RKEY_PGUP:
            if (g_current_page > 0) {
                g_current_page--;
                save_bookmark();
            }
            break;
        case RKEY_RIGHT:
        case RKEY_DOWN:
        case RKEY_PGDN:
        case RKEY_SPACE:
            if (g_current_page + 1 < g_page_count) {
                g_current_page++;
                save_bookmark();
            }
            break;
        case RKEY_TOC: {
            g_toc_sel = 0;
            if (g_page_offsets && g_page_count > 0) {
                size_t cur_off = g_page_offsets[g_current_page];
                for (int i = 0; i < g_chapter_count; i++) {
                    if (g_chapters[i].offset <= cur_off) g_toc_sel = i;
                    else break;
                }
            }
            g_toc_scroll = 0;
            g_screen = SCREEN_TOC;
            break;
        }
        case RKEY_JUMP:
            g_jump_target = g_current_page;
            g_screen = SCREEN_JUMP;
            break;
        case RKEY_SPACING:
            g_spacing_idx = (g_spacing_idx + 1) % 3;
            save_library();
            reader_on_resize();
            break;
        case RKEY_FONT_UP:
            if (g_font_size < 34) {
                g_font_size += 2;
                reader_on_resize();
            }
            break;
        case RKEY_FONT_DOWN:
            if (g_font_size > 14) {
                g_font_size -= 2;
                reader_on_resize();
            }
            break;
        case RKEY_OPEN:
            open_new_book_via_dialog();
            break;
        case RKEY_BACK:
        case RKEY_SHELF:
            save_bookmark();
            g_screen = SCREEN_SHELF;
            break;
        default:
            break;
    }
}

/* --- 畫面繪製 --- */
static void render_shelf(const Theme *th, int w, int h) {
    int top_bar = 32;
    int bot_bar = 32;

    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    hal_set_text_bg(th->bar_bg);
    char header[128];
    snprintf(header, sizeof(header), "CD-952 書架 (共 %d 本)  |  主題:%s", g_book_count, th->name);
    hal_draw_line_utf8(12, 7, header, (int)strlen(header), 16, th->bar_text);

    hal_fill_rect(0, h - bot_bar, w, bot_bar, th->bar_bg);
    hal_set_text_bg(th->bar_bg);
    const char *footer = "[↑↓]選書 [Enter/空白]閱讀 [O]新增書 [D]移出 [T]配色 [ESC]離開";
    hal_draw_line_utf8(10, h - bot_bar + 7, footer, (int)strlen(footer), 15, th->bar_text);

    if (g_book_count == 0) {
        hal_set_text_bg(th->bg);
        const char *m1 = "書架目前還沒有書籍紀錄";
        const char *m2 = "請按 [O] 或 [Enter] 開啟檔案選擇器加入 .txt / .bmp";
        const char *m3 = "之後每次閱讀都會自動記錄進度並顯示在這裡！";
        hal_draw_line_utf8(24, top_bar + 48, m1, (int)strlen(m1), 20, th->text);
        hal_draw_line_utf8(24, top_bar + 84, m2, (int)strlen(m2), 17, th->muted_text);
        hal_draw_line_utf8(24, top_bar + 114, m3, (int)strlen(m3), 17, th->muted_text);
        return;
    }

    int row_h = 46;
    int usable_h = h - top_bar - bot_bar - 8;
    int visible = usable_h / row_h;
    if (visible < 1) visible = 1;

    if (g_shelf_sel < g_shelf_scroll) g_shelf_scroll = g_shelf_sel;
    if (g_shelf_sel >= g_shelf_scroll + visible) g_shelf_scroll = g_shelf_sel - visible + 1;

    int y = top_bar + 4;
    for (int i = g_shelf_scroll; i < g_book_count && i < g_shelf_scroll + visible; i++) {
        BookEntry *be = &g_books[i];
        int is_sel = (i == g_shelf_sel);
        uint32_t rbg = is_sel ? th->sel_bg : th->bg;
        uint32_t rfg = is_sel ? th->sel_text : th->text;
        uint32_t subfg = is_sel ? th->sel_text : th->muted_text;

        if (is_sel) {
            hal_fill_rect(8, y, w - 16, row_h - 4, rbg);
        }

        hal_set_text_bg(rbg);
        char title_line[256];
        snprintf(title_line, sizeof(title_line), "%s %s", is_sel ? ">" : " ", get_filename_only(be->path));
        hal_draw_line_utf8(14, y + 3, title_line, (int)strlen(title_line), 18, rfg);

        char info_line[160];
        if (be->page_count > 0) {
            int pct = ((be->page_idx + 1) * 100) / be->page_count;
            snprintf(info_line, sizeof(info_line), "   進度：%d%%  (上次看到第 %d / %d 頁)",
                     pct, be->page_idx + 1, be->page_count);
        } else if (be->total_bytes > 0) {
            int pct = (int)((be->byte_offset * 100) / be->total_bytes);
            snprintf(info_line, sizeof(info_line), "   進度：%d%%", pct);
        } else {
            snprintf(info_line, sizeof(info_line), "   圖片書 / 尚未分頁");
        }
        hal_draw_line_utf8(14, y + 23, info_line, (int)strlen(info_line), 15, subfg);

        y += row_h;
    }
}

static void render_toc(const Theme *th, int w, int h) {
    int top_bar = 32;
    int bot_bar = 32;

    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    hal_set_text_bg(th->bar_bg);
    char header[128];
    snprintf(header, sizeof(header), "章節目錄 (共偵測到 %d 章)", g_chapter_count);
    hal_draw_line_utf8(12, 7, header, (int)strlen(header), 16, th->bar_text);

    hal_fill_rect(0, h - bot_bar, w, bot_bar, th->bar_bg);
    hal_set_text_bg(th->bar_bg);
    const char *footer = "[↑↓]選擇章節 [PgUp/PgDn]快捲 [Enter/空白]跳轉 [ESC/C]返回閱讀";
    hal_draw_line_utf8(10, h - bot_bar + 7, footer, (int)strlen(footer), 15, th->bar_text);

    if (g_chapter_count == 0) {
        hal_set_text_bg(th->bg);
        const char *m1 = "本書未偵測到標準章節標題（如「第X章」、「Chapter」）。";
        const char *m2 = "請按 [ESC] 返回，並改用 [G] 快速跳頁功能。";
        hal_draw_line_utf8(20, top_bar + 40, m1, (int)strlen(m1), 17, th->text);
        hal_draw_line_utf8(20, top_bar + 72, m2, (int)strlen(m2), 17, th->muted_text);
        return;
    }

    int row_h = 28;
    int usable_h = h - top_bar - bot_bar - 8;
    int visible = usable_h / row_h;
    if (visible < 1) visible = 1;

    if (g_toc_sel < g_toc_scroll) g_toc_scroll = g_toc_sel;
    if (g_toc_sel >= g_toc_scroll + visible) g_toc_scroll = g_toc_sel - visible + 1;

    int y = top_bar + 4;
    for (int i = g_toc_scroll; i < g_chapter_count && i < g_toc_scroll + visible; i++) {
        int is_sel = (i == g_toc_sel);
        uint32_t rbg = is_sel ? th->sel_bg : th->bg;
        uint32_t rfg = is_sel ? th->sel_text : th->text;
        if (is_sel) {
            hal_fill_rect(8, y, w - 16, row_h - 2, rbg);
        }
        hal_set_text_bg(rbg);
        char line[120];
        snprintf(line, sizeof(line), "%s %d. %s", is_sel ? ">" : " ", i + 1, g_chapters[i].title);
        hal_draw_line_utf8(12, y + 4, line, (int)strlen(line), 16, rfg);
        y += row_h;
    }
}

void reader_render(void) {
    const Theme *th = &THEMES[g_theme_idx];
    int w = hal_get_width();
    int h = hal_get_height();

    hal_clear(th->bg);

    if (g_screen == SCREEN_SHELF) {
        render_shelf(th, w, h);
        return;
    }
    if (g_screen == SCREEN_TOC) {
        render_toc(th, w, h);
        return;
    }

    int top_bar = 30;
    int bot_bar = 30;

    /* 頂部標題列 */
    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    hal_set_text_bg(th->bar_bg);
    char header[256];
    snprintf(header, sizeof(header), "%s  [行距:%s]", get_filename_only(g_filepath), SPACING_NAMES[g_spacing_idx]);
    hal_draw_line_utf8(10, 6, header, (int)strlen(header), 16, th->bar_text);

    /* 底部狀態列 */
    hal_fill_rect(0, h - bot_bar, w, bot_bar, th->bar_bg);
    hal_set_text_bg(th->bar_bg);
    char footer[256];
    if (g_screen == SCREEN_JUMP && g_page_count > 0) {
        int pct = ((g_jump_target + 1) * 100) / g_page_count;
        snprintf(footer, sizeof(footer),
                 "快速跳頁：第 %d/%d 頁 (%d%%) | [←→]±10 [↑↓]±50 [Enter]確認 [ESC]取消",
                 g_jump_target + 1, g_page_count, pct);
    } else if (g_screen == SCREEN_READ_TXT && g_page_count > 0) {
        int pct = ((g_current_page + 1) * 100) / g_page_count;
        snprintf(footer, sizeof(footer),
                 "%d/%d (%d%%) | [C]目錄 [G]跳頁 [L]行距 [T]配色 [ESC]書架",
                 g_current_page + 1, g_page_count, pct);
    } else {
        snprintf(footer, sizeof(footer),
                 "圖片模式 | [O]換圖 [T]配色 [ESC]回書架");
    }
    hal_draw_line_utf8(10, h - bot_bar + 7, footer, (int)strlen(footer), 15, th->bar_text);

    if (g_screen == SCREEN_READ_BMP) {
        hal_draw_bmp_file(g_filepath, 0, top_bar, w, h - top_bar - bot_bar);
        return;
    }

    /* 繪製正文 */
    if (g_text_buf && g_page_offsets && g_page_count > 0) {
        int max_units, lines_per_page, line_h;
        get_layout_params(&max_units, &lines_per_page, &line_h);

        hal_set_text_bg(th->bg);
        size_t pos = g_page_offsets[g_current_page];
        int y = top_bar + 8;
        for (int line = 0; line < lines_per_page && pos < g_text_len; line++) {
            size_t draw_len = 0;
            size_t step = scan_one_line(pos, max_units, &draw_len);
            if (draw_len > 0) {
                hal_draw_line_utf8(16, y, g_text_buf + pos, (int)draw_len, g_font_size, th->text);
            }
            y += line_h;
            if (step == 0) break;
            pos += step;
        }
    }

    /* 跳頁進度條彈窗 */
    if (g_screen == SCREEN_JUMP && g_page_count > 0) {
        int bw = w - 60;
        if (bw > 440) bw = 440;
        int bh = 90;
        int bx = (w - bw) / 2;
        int by = (h - bh) / 2;

        hal_fill_rect(bx - 2, by - 2, bw + 4, bh + 4, th->bar_text);
        hal_fill_rect(bx, by, bw, bh, th->bar_bg);
        hal_set_text_bg(th->bar_bg);

        int pct = ((g_jump_target + 1) * 100) / g_page_count;
        char t1[128];
        snprintf(t1, sizeof(t1), "跳轉至：第 %d / %d 頁 (%d%%)", g_jump_target + 1, g_page_count, pct);
        hal_draw_line_utf8(bx + 16, by + 14, t1, (int)strlen(t1), 18, th->bar_text);

        /* 進度條 */
        int bar_w = bw - 32;
        int fill_w = (bar_w * (g_jump_target + 1)) / g_page_count;
        hal_fill_rect(bx + 16, by + 44, bar_w, 12, th->bg);
        hal_fill_rect(bx + 16, by + 44, fill_w, 12, th->sel_bg);

        const char *t2 = "[←/→] ±10頁   [↑/↓] ±50頁   [Enter] 確認";
        hal_draw_line_utf8(bx + 16, by + 64, t2, (int)strlen(t2), 14, th->muted_text);
    }
}
