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
    MODE_WELCOME = 0,
    MODE_TXT,
    MODE_BMP
} ReaderMode;

typedef struct {
    uint32_t bg;
    uint32_t text;
    uint32_t bar_bg;
    uint32_t bar_text;
} Theme;

static const Theme THEMES[2] = {
    { 0xF7F4EC, 0x222222, 0xE5DFD0, 0x444444 },
    { 0x1A1B1E, 0xD8D8D8, 0x27292E, 0xA0A4AB }
};

static ReaderMode g_mode = MODE_WELCOME;
static int g_theme_idx = 0;
static int g_font_size = 20;
static char g_filepath[1024] = {0};

static char *g_text_buf = NULL;
static size_t g_text_len = 0;
static size_t *g_page_offsets = NULL;
static int g_page_count = 0;
static int g_current_page = 0;

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

static void save_bookmark(void) {
    if (g_mode != MODE_TXT || g_filepath[0] == '\0' || g_page_count <= 0) return;
    char bmk_path[1040];
    snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", g_filepath);
    FILE *f = open_utf8_file(bmk_path, "wb");
    if (f) {
        size_t off = g_page_offsets[g_current_page];
        fprintf(f, "%zu", off);
        fclose(f);
    }
}

static size_t load_bookmark(void) {
    if (g_filepath[0] == '\0') return 0;
    char bmk_path[1040];
    snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", g_filepath);
    FILE *f = open_utf8_file(bmk_path, "rb");
    if (!f) return 0;
    size_t off = 0;
    if (fscanf(f, "%zu", &off) != 1) off = 0;
    fclose(f);
    return off;
}

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
    int line_h = g_font_size + 6;

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

static void repaginate(size_t target_offset) {
    if (g_mode != MODE_TXT || !g_text_buf) return;

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

    g_current_page = 0;
    for (int i = 0; i < g_page_count; i++) {
        if (g_page_offsets[i] <= target_offset) {
            g_current_page = i;
        } else {
            break;
        }
    }
}

void reader_init(void) {
    g_mode = MODE_WELCOME;
}

void reader_shutdown(void) {
    save_bookmark();
    free(g_text_buf);
    free(g_page_offsets);
    g_text_buf = NULL;
    g_page_offsets = NULL;
}

int reader_open_file(const char *filepath) {
    if (!filepath || filepath[0] == '\0') return 0;

    save_bookmark();

    if (ends_with_ci(filepath, ".bmp")) {
        snprintf(g_filepath, sizeof(g_filepath), "%s", filepath);
        g_mode = MODE_BMP;
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
    g_mode = MODE_TXT;

    size_t bmk = load_bookmark();
    repaginate(bmk);
    return 1;
}

void reader_on_resize(void) {
    if (g_mode == MODE_TXT && g_page_offsets && g_page_count > 0) {
        size_t cur_off = g_page_offsets[g_current_page];
        repaginate(cur_off);
    }
}

void reader_on_key(KeyCode key) {
    switch (key) {
        case KEY_OPEN: {
            char path[1024] = {0};
            if (hal_open_file_dialog(path, sizeof(path))) {
                reader_open_file(path);
            }
            break;
        }
        case KEY_PREV:
            if (g_mode == MODE_TXT && g_current_page > 0) {
                g_current_page--;
                save_bookmark();
            } else if (g_mode == MODE_BMP) {
                char next_path[1024] = {0};
                if (hal_find_sibling_bmp(g_filepath, -1, next_path, sizeof(next_path))) {
                    snprintf(g_filepath, sizeof(g_filepath), "%s", next_path);
                }
            }
            break;
        case KEY_NEXT:
            if (g_mode == MODE_TXT && g_current_page + 1 < g_page_count) {
                g_current_page++;
                save_bookmark();
            } else if (g_mode == MODE_BMP) {
                char next_path[1024] = {0};
                if (hal_find_sibling_bmp(g_filepath, 1, next_path, sizeof(next_path))) {
                    snprintf(g_filepath, sizeof(g_filepath), "%s", next_path);
                }
            }
            break;
        case KEY_THEME:
            g_theme_idx = (g_theme_idx + 1) % 2;
            break;
        case KEY_FONT_UP:
            if (g_font_size < 36) {
                g_font_size += 2;
                reader_on_resize();
            }
            break;
        case KEY_FONT_DOWN:
            if (g_font_size > 14) {
                g_font_size -= 2;
                reader_on_resize();
            }
            break;
        case KEY_QUIT:
            hal_request_quit();
            break;
        default:
            break;
    }
}

void reader_render(void) {
    const Theme *th = &THEMES[g_theme_idx];
    int w = hal_get_width();
    int h = hal_get_height();
    int top_bar = 30;
    int bot_bar = 30;

    hal_clear(th->bg);

    hal_fill_rect(0, 0, w, top_bar, th->bar_bg);
    char header[512];
    if (g_mode == MODE_WELCOME) {
        snprintf(header, sizeof(header), "CD-952 電子書閱讀器");
    } else {
        snprintf(header, sizeof(header), "%s", get_filename_only(g_filepath));
    }
    hal_draw_line_utf8(10, 6, header, (int)strlen(header), 16, th->bar_text);

    hal_fill_rect(0, h - bot_bar, w, bot_bar, th->bar_bg);
    char footer[256];
    if (g_mode == MODE_TXT && g_page_count > 0) {
        int pct = ((g_current_page + 1) * 100) / g_page_count;
        snprintf(footer, sizeof(footer),
                 "%d/%d (%d%%) | [O]開檔 [←→]翻頁 [T]日夜 [ESC]離開",
                 g_current_page + 1, g_page_count, pct);
    } else if (g_mode == MODE_BMP) {
        snprintf(footer, sizeof(footer),
                 "圖片模式 | [O]開檔 [←→]切換 [T]日夜 [ESC]離開");
    } else {
        snprintf(footer, sizeof(footer),
                 "請按 [O] 選擇檔案 (.txt / .bmp)，[ESC] 離開");
    }
    hal_draw_line_utf8(10, h - bot_bar + 7, footer, (int)strlen(footer), 16, th->bar_text);

    if (g_mode == MODE_WELCOME) {
        const char *lines[] = {
            "CD-952 電子書閱讀器",
            "",
            "[O]      開啟檔案 (.txt / .bmp)",
            "[← / →]  上一頁 / 下一頁",
            "[T]      切換日間 / 夜間模式",
            "[ESC]    結束程式"
        };
        int y = top_bar + 30;
        for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); i++) {
            hal_draw_line_utf8(24, y, lines[i], (int)strlen(lines[i]), 18, th->text);
            y += 28;
        }
        return;
    }

    if (g_mode == MODE_BMP) {
        hal_draw_bmp_file(g_filepath, 0, top_bar, w, h - top_bar - bot_bar);
        return;
    }

    if (g_mode == MODE_TXT && g_text_buf && g_page_offsets && g_page_count > 0) {
        int max_units, lines_per_page, line_h;
        get_layout_params(&max_units, &lines_per_page, &line_h);

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
}
