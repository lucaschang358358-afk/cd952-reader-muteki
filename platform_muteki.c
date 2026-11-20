/*
 * 無敵 CD-952 (Besta RTOS / Project Muteki) 實機平台實作 v2
 * 新增：書架首頁、章節目錄、快速跳頁、4 種護眼配色、3 種行距切換
 */

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

static lcd_t *g_lcd = NULL;
static int g_running = 1;
static uint32_t g_text_bg = 0xF7F4EC;

/* 將 UTF-8 轉成 UTF-16 (供 Besta RTOS WriteAlignString 使用) */
static size_t utf8_to_utf16(const char *src, int src_len, UTF16 *dst, size_t max_dst) {
    size_t out = 0;
    int i = 0;
    while (i < src_len && out + 1 < max_dst) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp = 0;
        if ((c & 0x80) == 0) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < src_len) {
            cp = ((c & 0x1F) << 6) | (src[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < src_len) {
            cp = ((c & 0x0F) << 12) | ((src[i + 1] & 0x3F) << 6) | (src[i + 2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < src_len) {
            cp = 0x3F;
            i += 4;
        } else {
            cp = 0x3F;
            i += 1;
        }
        dst[out++] = (UTF16)cp;
    }
    dst[out] = 0;
    return out;
}

static inline uint16_t rgb888_to_rgb565(uint32_t rgb) {
    uint16_t r = (uint16_t)((rgb >> 16) & 0xFF);
    uint16_t g = (uint16_t)((rgb >> 8) & 0xFF);
    uint16_t b = (uint16_t)(rgb & 0xFF);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void put_pixel_fb(int x, int y, uint32_t rgb) {
    if (!g_lcd || !g_lcd->surface || !g_lcd->surface->buffer) return;
    if (x < 0 || y < 0 || x >= (int)g_lcd->width || y >= (int)g_lcd->height) return;

    int stride = g_lcd->width;
    if (g_lcd->surface->depth == LCD_SURFACE_PIXFMT_XRGB) {
        uint32_t *fb32 = (uint32_t *)g_lcd->surface->buffer;
        fb32[y * stride + x] = rgb;
    } else if (g_lcd->surface->depth == LCD_SURFACE_PIXFMT_RGB565) {
        uint16_t *fb16 = (uint16_t *)g_lcd->surface->buffer;
        fb16[y * stride + x] = rgb888_to_rgb565(rgb);
    }
}

int hal_get_width(void)  { return g_lcd ? (int)g_lcd->width  : 800; }
int hal_get_height(void) { return g_lcd ? (int)g_lcd->height : 480; }

void hal_set_text_bg(uint32_t rgb) {
    g_text_bg = rgb;
}

void hal_clear(uint32_t rgb) {
    g_text_bg = rgb;
    rgbSetBkColor(rgb);
    ClearScreen(false);
    hal_fill_rect(0, 0, hal_get_width(), hal_get_height(), rgb);
}

void hal_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            put_pixel_fb(px, py, rgb);
        }
    }
}

void hal_draw_line_utf8(int x, int y, const char *utf8_text, int byte_len, int font_size, uint32_t rgb) {
    (void)font_size;
    if (!g_lcd || !utf8_text || byte_len <= 0) return;

    UTF16 u16buf[512];
    utf8_to_utf16(utf8_text, byte_len, u16buf, 512);

    rgbSetColor(rgb);
    rgbSetBkColor(g_text_bg);
    SetFontType(MONOSPACE_CJK);
    WriteAlignString(
        x, y,
        u16buf,
        g_lcd->width - x,
        STR_ALIGN_LEFT,
        PRINT_NONE
    );
}

/* 純 C 24-bit BMP 解碼與等比例縮放至 LCD Framebuffer */
int hal_draw_bmp_file(const char *filepath, int x, int y, int w, int h) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return 0;

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f);
        return 0;
    }

    uint32_t data_off = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) | ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
    int32_t bmp_w = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) | ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
    int32_t bmp_h = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) | ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
    uint16_t bpp  = (uint16_t)hdr[28] | ((uint16_t)hdr[29] << 8);

    if (bpp != 24 || bmp_w <= 0 || bmp_h == 0) {
        fclose(f);
        return 0;
    }

    int top_down = (bmp_h < 0);
    int abs_h = top_down ? -bmp_h : bmp_h;
    int row_stride = ((bmp_w * 3) + 3) & ~3;

    uint8_t *row_buf = (uint8_t *)malloc((size_t)row_stride);
    if (!row_buf) {
        fclose(f);
        return 0;
    }

    int dw = w, dh = h;
    if (bmp_w * h > w * abs_h) {
        dh = (w * abs_h) / bmp_w;
    } else {
        dw = (h * bmp_w) / abs_h;
    }
    int dx = x + (w - dw) / 2;
    int dy = y + (h - dh) / 2;

    for (int ty = 0; ty < dh; ty++) {
        int sy = (ty * abs_h) / dh;
        int file_row = top_down ? sy : (abs_h - 1 - sy);
        fseek(f, (long)(data_off + (uint32_t)(file_row * row_stride)), SEEK_SET);
        if (fread(row_buf, 1, (size_t)row_stride, f) != (size_t)row_stride) break;

        for (int tx = 0; tx < dw; tx++) {
            int sx = (tx * bmp_w) / dw;
            uint32_t b = row_buf[sx * 3 + 0];
            uint32_t g = row_buf[sx * 3 + 1];
            uint32_t r = row_buf[sx * 3 + 2];
            put_pixel_fb(dx + tx, dy + ty, (r << 16) | (g << 8) | b);
        }
    }

    free(row_buf);
    fclose(f);
    return 1;
}

const char *hal_get_library_path(void) {
    return "C:\\cd952_books.idx";
}

int hal_open_file_dialog(char *out_path, int max_len) {
    UTF16 utf16path[FILEPICKER_CONTEXT_OUTPUT_MAX_LFN] = {0};
    filepicker_context_t ctx = {0};
    void *ctx_out = FILEPICKER_CONTEXT_OUTPUT_ALLOC(calloc, 1, true);
    if (!ctx_out) return 0;

    ctx.paths = ctx_out;
    ctx.ctx_size = sizeof(ctx);
    ctx.path_max_cu = FILEPICKER_CONTEXT_OUTPUT_MAX_LFN;
    ctx.type_list = (
        "E-Book & Image (*.txt, *.bmp)\0*.txt|*.bmp\0"
        "Text Files (*.txt)\0*.txt\0"
        "BMP Images (*.bmp)\0*.bmp\0"
        "All Files (*.*)\0*.*\0"
        "\0\0\0"
    );

    bool ret = _GetOpenFileName(&ctx);
    if (!ret || _GetNextFileName(&ctx, utf16path) != 0) {
        free(ctx_out);
        return 0;
    }
    free(ctx_out);

    if (wcstombs(out_path, utf16path, (size_t)max_len) == (size_t)-1) {
        return 0;
    }
    return 1;
}

int hal_find_sibling_bmp(const char *current_path, int delta, char *out_path, int max_len) {
    (void)current_path; (void)delta; (void)out_path; (void)max_len;
    return 0;
}

void hal_request_quit(void) {
    g_running = 0;
}

int main(void) {
    g_lcd = GetActiveLCD();
    if (!g_lcd || !g_lcd->surface) {
        return 1;
    }

    /* 啟動直接進入書架畫面，不再強制跳出選檔視窗 */
    reader_init();
    reader_render();

    ui_event_t uievent = {0};
    while (g_running) {
        if (TestKeyEvent(&uievent) && GetEvent(&uievent)) {
            if (uievent.event_type == UI_EVENT_TYPE_KEY) {
                KeyCode k = KEY_NONE;
                switch (uievent.key_code0) {
                    case 1:   k = KEY_BACK; break;    /* KEY_ESC */
                    case 2:   k = KEY_LEFT; break;    /* KEY_LEFT */
                    case 3:   k = KEY_UP; break;      /* KEY_UP */
                    case 4:   k = KEY_RIGHT; break;   /* KEY_RIGHT */
                    case 5:   k = KEY_DOWN; break;    /* KEY_DOWN */
                    case 6:   k = KEY_PGUP; break;    /* KEY_PGUP */
                    case 7:   k = KEY_PGDN; break;    /* KEY_PGDN */
                    case 8:   k = KEY_SHELF; break;   /* Backspace */
                    case 10:
                    case 13:  k = KEY_OK; break;      /* Enter */
                    case 32:  k = KEY_SPACE; break;   /* Space */
                    case 66:
                    case 98:  k = KEY_SHELF; break;   /* B */
                    case 67:
                    case 99:  k = KEY_TOC; break;     /* C */
                    case 68:
                    case 100: k = KEY_DELETE; break;  /* D */
                    case 71:
                    case 103: k = KEY_JUMP; break;    /* G */
                    case 76:
                    case 108: k = KEY_SPACING; break; /* L */
                    case 79:
                    case 111: k = KEY_OPEN; break;    /* O */
                    case 84:
                    case 116: k = KEY_THEME; break;   /* T */
                    case 43:
                    case 61:  k = KEY_FONT_UP; break; /* + */
                    case 45:  k = KEY_FONT_DOWN; break; /* - */
                    default:  break;
                }
                if (k != KEY_NONE) {
                    reader_on_key(k);
                    if (g_running) reader_render();
                }
            }
            ClearEvent(&uievent);
        } else {
            OSSleep(20);
        }
    }

    reader_shutdown();
    return 0;
}
