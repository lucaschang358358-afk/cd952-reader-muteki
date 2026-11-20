#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stddef.h>

/* 全部使用 RKEY_ 前綴，完全避開 Muteki SDK 內建 KEY_* 撞名 */
typedef enum {
    RKEY_NONE = 0,
    RKEY_UP,
    RKEY_DOWN,
    RKEY_LEFT,
    RKEY_RIGHT,
    RKEY_PGUP,
    RKEY_PGDN,
    RKEY_OK,       /* Enter */
    RKEY_SPACE,    /* Space */
    RKEY_BACK,     /* ESC */
    RKEY_BACKSPACE,/* Backspace */
    RKEY_SHELF,    /* B */
    RKEY_OPEN,     /* O */
    RKEY_TOC,      /* C */
    RKEY_JUMP,     /* G */
    RKEY_THEME,    /* T */
    RKEY_SPACING,  /* L */
    RKEY_MENU,     /* M */
    RKEY_DELETE,   /* D */
    RKEY_BOOKMARK, /* K: 書籤列表 / 新增書籤 */
    RKEY_SEARCH,   /* F: 全文搜尋 */
    RKEY_REFLOW,   /* R: PDF 原版面 / 重排模式切換 */
    RKEY_TAB1,     /* 1 */
    RKEY_TAB2,     /* 2 */
    RKEY_TAB3,     /* 3 */
    RKEY_TAB4,     /* 4 */
    RKEY_FONT_UP,  /* + / = */
    RKEY_FONT_DOWN,/* - */
    RKEY_CHAR      /* 一般鍵盤字元輸入 (搜尋用) */
} ReaderKey;

/* 螢幕尺寸 */
int hal_get_width(void);
int hal_get_height(void);

/* 雙緩衝像素繪圖介面 (rgb 格式為 0xRRGGBB) */
void hal_clear(uint32_t rgb);
void hal_put_pixel(int x, int y, uint32_t rgb);
void hal_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void hal_draw_rect(int x, int y, int w, int h, uint32_t rgb);
void hal_fill_circle(int cx, int cy, int r, uint32_t rgb);
void hal_draw_circle(int cx, int cy, int r, uint32_t rgb);
void hal_draw_line(int x0, int y0, int x1, int y1, uint32_t rgb);

/* 繪製 RGB888 圖片至雙緩衝並裁切於 (clip_x, clip_y, clip_w, clip_h) */
void hal_blit_rgb888_scaled(const uint8_t *rgb, int img_w, int img_h,
                            int dst_x, int dst_y, int dst_w, int dst_h,
                            int clip_x, int clip_y, int clip_w, int clip_h,
                            int invert_colors);

/* 中文文字繪製佇列 (於 hal_present() 時一次刷上 LCD，徹底零閃爍) */
void hal_set_text_bg(uint32_t rgb);
void hal_draw_line_utf8(int x, int y, const char *utf8_text, int byte_len, int max_px_w, uint32_t rgb);
void hal_present(void);

/* 24-bit BMP 繪製至雙緩衝 */
int  hal_draw_bmp_file(const char *filepath, int x, int y, int w, int h);

/* 檔案與系統介面 */
const char *hal_get_library_path(void);
int  hal_open_file_dialog(char *out_path, int max_len);
int  hal_find_sibling_bmp(const char *current_path, int delta, char *out_path, int max_len);
void hal_request_quit(void);

#endif
