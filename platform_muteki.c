/*
 * 無敵 CD-952 (Besta RTOS / Project Muteki) 現代化雙緩衝閱讀器平台 v4
 * - RAM 像素雙緩衝 + ARM9 展開式高速填滿 + 單次 memcpy 刷屏 (零閃爍、不卡頓)
 * - 支援真 PDF 圖片高速縮放貼圖 (hal_blit_rgb888_scaled)
 * - 支援實體鍵盤直接打字搜尋、原生開檔選擇器 (*.epub / *.pdf / *.txt / *.bmp)
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<muteki/common.h>)
#include <muteki/common.h>
#endif
#if __has_include(<muteki/threading.h>)
#include <muteki/threading.h>
#endif
#if __has_include(<muteki/utf16.h>)
#include <muteki/utf16.h>
#endif

#if __has_include(<muteki/draw/font.h>)
#include <muteki/draw/font.h>
#elif __has_include(<muteki/ui/font.h>)
#include <muteki/ui/font.h>
#elif __has_include(<muteki/font.h>)
#include <muteki/font.h>
#endif

#if __has_include(<muteki/draw/lcd.h>)
#include <muteki/draw/lcd.h>
#elif __has_include(<muteki/ui/lcd.h>)
#include <muteki/ui/lcd.h>
#elif __has_include(<muteki/lcd.h>)
#include <muteki/lcd.h>
#endif

#if __has_include(<muteki/draw/surface.h>)
#include <muteki/draw/surface.h>
#elif __has_include(<muteki/ui/surface.h>)
#include <muteki/ui/surface.h>
#elif __has_include(<muteki/surface.h>)
#include <muteki/surface.h>
#endif

#if __has_include(<muteki/ui/canvas.h>)
#include <muteki/ui/canvas.h>
#endif
#if __has_include(<muteki/ui/common.h>)
#include <muteki/ui/common.h>
#endif
#if __has_include(<muteki/ui.h>)
#include <muteki/ui.h>
#endif

#if __has_include(<muteki/ui/framework/event.h>)
#include <muteki/ui/framework/event.h>
#elif __has_include(<muteki/ui/event.h>)
#include <muteki/ui/event.h>
#elif __has_include(<muteki/event.h>)
#include <muteki/event.h>
#endif

#if __has_include(<muteki/ui/modals/filepicker.h>)
#include <muteki/ui/modals/filepicker.h>
#elif __has_include(<muteki/ui/views/filepicker.h>)
#include <muteki/ui/views/filepicker.h>
#elif __has_include(<muteki/ui/filepicker.h>)
#include <muteki/ui/filepicker.h>
#elif __has_include(<muteki/filepicker.h>)
#include <muteki/filepicker.h>
#endif

#include "hal.h"
#include "reader_core.h"

#define MAX_TEXT_QUEUE 220

typedef struct {
    int x;
    int y;
    int max_w;
    uint32_t fg;
    uint32_t bg;
    char text[256];
} TextCmd;

static lcd_t *g_lcd = NULL;
static int g_w = 800;
static int g_h = 480;
static int g_is_xrgb = 1;
static void *g_backbuf = NULL;
static size_t g_fb_bytes = 0;

static TextCmd g_text_q[MAX_TEXT_QUEUE];
static int g_text_q_len = 0;
static uint32_t g_text_bg = 0xF4F1EA;
static int g_running = 1;

static inline uint16_t rgb888_to_rgb565(uint32_t rgb) {
    uint16_t r = (uint16_t)((rgb >> 16) & 0xFF);
    uint16_t g = (uint16_t)((rgb >> 8) & 0xFF);
    uint16_t b = (uint16_t)(rgb & 0xFF);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static size_t utf8_to_utf16(const char *src, int src_len, UTF16 *dst, size_t max_dst) {
    size_t out = 0; int i = 0;
    while (i < src_len && out + 1 < max_dst) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp = 0;
        if ((c & 0x80) == 0) { cp = c; i += 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < src_len) {
            cp = ((c & 0x1F) << 6) | (src[i + 1] & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < src_len) {
            cp = ((c & 0x0F) << 12) | ((src[i + 1] & 0x3F) << 6) | (src[i + 2] & 0x3F); i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < src_len) {
            cp = 0x3F; i += 4;
        } else { cp = 0x3F; i += 1; }
        dst[out++] = (UTF16)cp;
    }
    dst[out] = 0;
    return out;
}

static size_t utf16_to_utf8(const UTF16 *src, char *dst, size_t max_dst) {
    size_t out = 0;
    if (!src || !dst || max_dst == 0) return 0;
    for (size_t i = 0; src[i] != 0; i++) {
        uint32_t cp = (uint32_t)src[i];
        if (cp < 0x80u) {
            if (out + 1 >= max_dst) break;
            dst[out++] = (char)cp;
        } else if (cp < 0x800u) {
            if (out + 2 >= max_dst) break;
            dst[out++] = (char)(0xC0u | (cp >> 6));
            dst[out++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (out + 3 >= max_dst) break;
            dst[out++] = (char)(0xE0u | (cp >> 12));
            dst[out++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[out++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    dst[out] = '\0';
    return out;
}

int hal_get_width(void)  { return g_w; }
int hal_get_height(void) { return g_h; }

void hal_put_pixel(int x, int y, uint32_t rgb) {
    if (!g_backbuf || x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    if (g_is_xrgb) ((uint32_t *)g_backbuf)[y * g_w + x] = rgb;
    else ((uint16_t *)g_backbuf)[y * g_w + x] = rgb888_to_rgb565(rgb);
}

void hal_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!g_backbuf || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = (x + w > g_w) ? g_w : (x + w);
    int y1 = (y + h > g_h) ? g_h : (y + h);
    int span = x1 - x0;
    if (span <= 0 || y0 >= y1) return;

    if (g_is_xrgb) {
        uint32_t *fb = (uint32_t *)g_backbuf;
        for (int py = y0; py < y1; py++) {
            uint32_t *row = fb + py * g_w + x0;
            int n = span;
            while (n >= 4) { row[0] = rgb; row[1] = rgb; row[2] = rgb; row[3] = rgb; row += 4; n -= 4; }
            while (n-- > 0) *row++ = rgb;
        }
    } else {
        uint16_t c16 = rgb888_to_rgb565(rgb);
        uint32_t c32 = ((uint32_t)c16 << 16) | (uint32_t)c16;
        uint16_t *fb = (uint16_t *)g_backbuf;
        for (int py = y0; py < y1; py++) {
            uint16_t *row = fb + py * g_w + x0;
            int n = span;
            if (((uintptr_t)row & 2u) && n > 0) { *row++ = c16; n--; }
            uint32_t *row32 = (uint32_t *)row;
            while (n >= 4) { row32[0] = c32; row32[1] = c32; row32 += 2; n -= 4; }
            row = (uint16_t *)row32;
            while (n-- > 0) *row++ = c16;
        }
    }
}

void hal_clear(uint32_t rgb) {
    g_text_q_len = 0;
    g_text_bg = rgb;
    hal_fill_rect(0, 0, g_w, g_h, rgb);
}

void hal_draw_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    hal_fill_rect(x, y, w, 1, rgb);
    hal_fill_rect(x, y + h - 1, w, 1, rgb);
    hal_fill_rect(x, y, 1, h, rgb);
    hal_fill_rect(x + w - 1, y, 1, h, rgb);
}

void hal_fill_circle(int cx, int cy, int r, uint32_t rgb) {
    if (r <= 0) return;
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy; if (py < 0 || py >= g_h) continue;
        int dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= r2) dx++;
        hal_fill_rect(cx - dx, py, dx * 2 + 1, 1, rgb);
    }
}

void hal_draw_circle(int cx, int cy, int r, uint32_t rgb) {
    if (r <= 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        hal_put_pixel(cx + x, cy + y, rgb); hal_put_pixel(cx + y, cy + x, rgb);
        hal_put_pixel(cx - y, cy + x, rgb); hal_put_pixel(cx - x, cy + y, rgb);
        hal_put_pixel(cx - x, cy - y, rgb); hal_put_pixel(cx - y, cy - x, rgb);
        hal_put_pixel(cx + y, cy - x, rgb); hal_put_pixel(cx + x, cy - y, rgb);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x + 1); }
    }
}

void hal_draw_line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        hal_put_pixel(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 零除法 DDA 高速縮放貼圖 (供 PDF 內嵌 JPEG / Flate 圖片直接繪製於頁面上!) */
void hal_blit_rgb888_scaled(const uint8_t *rgb, int img_w, int img_h,
                            int dst_x, int dst_y, int dst_w, int dst_h,
                            int clip_x, int clip_y, int clip_w, int clip_h,
                            int invert_colors) {
    if (!g_backbuf || !rgb || img_w <= 0 || img_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    int cx0 = clip_x < 0 ? 0 : clip_x;
    int cy0 = clip_y < 0 ? 0 : clip_y;
    int cx1 = (clip_x + clip_w > g_w) ? g_w : (clip_x + clip_w);
    int cy1 = (clip_y + clip_h > g_h) ? g_h : (clip_y + clip_h);

    int x0 = dst_x < cx0 ? cx0 : dst_x;
    int y0 = dst_y < cy0 ? cy0 : dst_y;
    int x1 = (dst_x + dst_w > cx1) ? cx1 : (dst_x + dst_w);
    int y1 = (dst_y + dst_h > cy1) ? cy1 : (dst_y + dst_h);
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t step_x = ((uint32_t)img_w << 16) / (uint32_t)dst_w;
    uint32_t step_y = ((uint32_t)img_h << 16) / (uint32_t)dst_h;
    uint32_t sy_fp = (uint32_t)(y0 - dst_y) * step_y;
    uint32_t sx_start = (uint32_t)(x0 - dst_x) * step_x;

    for (int py = y0; py < y1; py++) {
        int sy = (int)(sy_fp >> 16);
        if (sy >= img_h) sy = img_h - 1;
        const uint8_t *src_row = rgb + (size_t)sy * (size_t)img_w * 3u;
        uint32_t sx_fp = sx_start;

        if (g_is_xrgb) {
            uint32_t *dst_row = (uint32_t *)g_backbuf + py * g_w + x0;
            for (int px = x0; px < x1; px++) {
                int sx = (int)(sx_fp >> 16);
                if (sx >= img_w) sx = img_w - 1;
                const uint8_t *p = src_row + sx * 3;
                uint32_t r = invert_colors ? (255u - p[0]) : p[0];
                uint32_t g = invert_colors ? (255u - p[1]) : p[1];
                uint32_t b = invert_colors ? (255u - p[2]) : p[2];
                *dst_row++ = (r << 16) | (g << 8) | b;
                sx_fp += step_x;
            }
        } else {
            uint16_t *dst_row = (uint16_t *)g_backbuf + py * g_w + x0;
            for (int px = x0; px < x1; px++) {
                int sx = (int)(sx_fp >> 16);
                if (sx >= img_w) sx = img_w - 1;
                const uint8_t *p = src_row + sx * 3;
                uint32_t r = invert_colors ? (255u - p[0]) : p[0];
                uint32_t g = invert_colors ? (255u - p[1]) : p[1];
                uint32_t b = invert_colors ? (255u - p[2]) : p[2];
                *dst_row++ = rgb888_to_rgb565((r << 16) | (g << 8) | b);
                sx_fp += step_x;
            }
        }
        sy_fp += step_y;
    }
}

void hal_set_text_bg(uint32_t rgb) { g_text_bg = rgb; }

void hal_draw_line_utf8(int x, int y, const char *utf8_text, int byte_len, int max_px_w, uint32_t rgb) {
    if (!utf8_text || byte_len <= 0 || g_text_q_len >= MAX_TEXT_QUEUE) return;
    if (y < -18 || y >= g_h || x >= g_w) return;
    TextCmd *tc = &g_text_q[g_text_q_len++];
    tc->x = x; tc->y = y;
    tc->max_w = (max_px_w > 0 && x + max_px_w <= g_w) ? max_px_w : (g_w - x);
    tc->fg = rgb; tc->bg = g_text_bg;
    size_t cp = (size_t)byte_len;
    if (cp >= sizeof(tc->text)) cp = sizeof(tc->text) - 1;
    memcpy(tc->text, utf8_text, cp);
    tc->text[cp] = '\0';
}

void hal_present(void) {
    if (!g_lcd || !g_lcd->surface || !g_lcd->surface->buffer || !g_backbuf) return;
    memcpy(g_lcd->surface->buffer, g_backbuf, g_fb_bytes);

    if (g_text_q_len > 0) {
        SetFontType(MONOSPACE_CJK);
        UTF16 u16[256];
        for (int i = 0; i < g_text_q_len; i++) {
            TextCmd *tc = &g_text_q[i];
            int dx = tc->x < 0 ? 0 : tc->x;
            int mw = tc->max_w;
            if (dx + mw > g_w) mw = g_w - dx;
            if (mw <= 8) continue;
            utf8_to_utf16(tc->text, (int)strlen(tc->text), u16, 256);
            rgbSetColor(tc->fg);
            rgbSetBkColor(tc->bg);
            WriteAlignString(dx, tc->y, u16, mw, STR_ALIGN_LEFT, PRINT_NONE);
        }
        g_text_q_len = 0;
    }
}

int hal_draw_bmp_file(const char *filepath, int x, int y, int w, int h) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return 0;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return 0; }

    uint32_t data_off = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) | ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
    int32_t bmp_w = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) | ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
    int32_t bmp_h = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) | ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
    uint16_t bpp  = (uint16_t)hdr[28] | ((uint16_t)hdr[29] << 8);
    if (bpp != 24 || bmp_w <= 0 || bmp_h == 0) { fclose(f); return 0; }

    int top_down = (bmp_h < 0), abs_h = top_down ? -bmp_h : bmp_h;
    int row_stride = ((bmp_w * 3) + 3) & ~3;
    uint8_t *row_buf = (uint8_t *)malloc((size_t)row_stride);
    if (!row_buf) { fclose(f); return 0; }

    int dw = w, dh = h;
    if (bmp_w * h > w * abs_h) dh = (w * abs_h) / bmp_w;
    else dw = (h * bmp_w) / abs_h;
    int dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;

    for (int ty = 0; ty < dh; ty++) {
        int sy = (ty * abs_h) / dh;
        int file_row = top_down ? sy : (abs_h - 1 - sy);
        fseek(f, (long)(data_off + (uint32_t)(file_row * row_stride)), SEEK_SET);
        if (fread(row_buf, 1, (size_t)row_stride, f) != (size_t)row_stride) break;
        for (int tx = 0; tx < dw; tx++) {
            int sx = (tx * bmp_w) / dw;
            uint32_t b = row_buf[sx * 3 + 0], g = row_buf[sx * 3 + 1], r = row_buf[sx * 3 + 2];
            hal_put_pixel(dx + tx, dy + ty, (r << 16) | (g << 8) | b);
        }
    }
    free(row_buf); fclose(f);
    return 1;
}

const char *hal_get_library_path(void) { return "C:\\cd952_books_v4.idx"; }

int hal_open_file_dialog(char *out_path, int max_len) {
    UTF16 utf16path[FILEPICKER_CONTEXT_OUTPUT_MAX_LFN] = {0};
    filepicker_context_t ctx = {0};
    void *ctx_out = FILEPICKER_CONTEXT_OUTPUT_ALLOC(calloc, 1, true);
    if (!ctx_out) return 0;

    ctx.paths = ctx_out;
    ctx.ctx_size = sizeof(ctx);
    ctx.path_max_cu = FILEPICKER_CONTEXT_OUTPUT_MAX_LFN;
    ctx.type_list = (
        "All E-Books (*.epub, *.pdf, *.txt, *.bmp)\0*.epub|*.pdf|*.txt|*.bmp\0"
        "PDF Documents (*.pdf)\0*.pdf\0"
        "EPUB Books (*.epub)\0*.epub\0"
        "Text Files (*.txt)\0*.txt\0"
        "BMP Image Books (*.bmp)\0*.bmp\0"
        "All Files (*.*)\0*.*\0"
        "\0\0\0"
    );

    bool ret = _GetOpenFileName(&ctx);
    if (!ret || _GetNextFileName(&ctx, utf16path) != 0) {
        free(ctx_out);
        return 0;
    }
    free(ctx_out);

    return (utf16_to_utf8(utf16path, out_path, (size_t)max_len) > 0) ? 1 : 0;
}

int hal_find_sibling_bmp(const char *current_path, int delta, char *out_path, int max_len) {
    if (!current_path || !out_path || max_len < 16) return 0;
    const char *dot = strrchr(current_path, '.');
    if (!dot) return 0;
    const char *p = dot;
    while (p > current_path && isdigit((unsigned char)p[-1])) p--;
    int digits = (int)(dot - p);
    if (digits <= 0 || digits > 6) return 0;

    long num = strtol(p, NULL, 10), next = num + delta;
    if (next < 0) return 0;

    size_t prefix_len = (size_t)(p - current_path);
    char cand[400];
    if (prefix_len + 24 >= sizeof(cand)) return 0;

    for (int mode = 0; mode < 2; mode++) {
        memcpy(cand, current_path, prefix_len);
        if (mode == 0) snprintf(cand + prefix_len, sizeof(cand) - prefix_len, "%0*ld%s", digits, next, dot);
        else snprintf(cand + prefix_len, sizeof(cand) - prefix_len, "%ld%s", next, dot);
        FILE *f = fopen(cand, "rb");
        if (f) { fclose(f); snprintf(out_path, (size_t)max_len, "%s", cand); return 1; }
    }
    return 0;
}

void hal_request_quit(void) { g_running = 0; }

static ReaderKey map_key(short code, char *out_char) {
    *out_char = (code >= 0x20 && code < 0x7F) ? (char)code : '\0';
    switch (code) {
        case 1:   return RKEY_BACK;
        case 2:   return RKEY_LEFT;
        case 3:   return RKEY_UP;
        case 4:   return RKEY_RIGHT;
        case 5:   return RKEY_DOWN;
        case 6:   return RKEY_PGUP;
        case 7:   return RKEY_PGDN;
        case 8:   return RKEY_BACKSPACE;
        case 10:
        case 13:  return RKEY_OK;
        case 32:  return RKEY_SPACE;
        case 49:  return RKEY_TAB1;
        case 50:  return RKEY_TAB2;
        case 51:  return RKEY_TAB3;
        case 52:  return RKEY_TAB4;
        case 66: case 98:  return RKEY_SHELF;
        case 67: case 99:  return RKEY_TOC;
        case 68: case 100: return RKEY_DELETE;
        case 70: case 102: return RKEY_SEARCH;
        case 71: case 103: return RKEY_JUMP;
        case 75: case 107: return RKEY_BOOKMARK;
        case 76: case 108: return RKEY_SPACING;
        case 77: case 109: return RKEY_MENU;
        case 79: case 111: return RKEY_OPEN;
        case 82: case 114: return RKEY_REFLOW;
        case 84: case 116: return RKEY_THEME;
        case 43: case 61:  return RKEY_FONT_UP;
        case 45:           return RKEY_FONT_DOWN;
        default:
            if (code >= 0x20 && code < 0x7F) return RKEY_CHAR;
            return RKEY_NONE;
    }
}

int main(void) {
    g_lcd = GetActiveLCD();
    if (!g_lcd || !g_lcd->surface || !g_lcd->surface->buffer) return 1;

    g_w = (int)g_lcd->width;
    g_h = (int)g_lcd->height;
    g_is_xrgb = (g_lcd->surface->depth == LCD_SURFACE_PIXFMT_XRGB);
    g_fb_bytes = (size_t)g_w * (size_t)g_h * (g_is_xrgb ? 4u : 2u);
    g_backbuf = malloc(g_fb_bytes);
    if (!g_backbuf) return 1;

    reader_init();
    reader_render();

    ui_event_t uievent = {0};
    while (g_running) {
        if (TestKeyEvent(&uievent) && GetEvent(&uievent)) {
            if (uievent.event_type == UI_EVENT_TYPE_KEY) {
                char ch = 0;
                ReaderKey k = map_key(uievent.key_code0, &ch);
                if (k != RKEY_NONE) {
                    reader_on_key(k, ch);
                    if (g_running) reader_render();
                }
            }
            ClearEvent(&uievent);
        } else {
            OSSleep(25);
        }
    }

    reader_shutdown();
    free(g_backbuf);
    return 0;
}
