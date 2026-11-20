#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    RKEY_NONE = 0,
    RKEY_UP,
    RKEY_DOWN,
    RKEY_LEFT,
    RKEY_RIGHT,
    RKEY_PGUP,
    RKEY_PGDN,
    RKEY_OK,
    RKEY_SPACE,
    RKEY_BACK,
    RKEY_BACKSPACE,
    RKEY_SHELF,
    RKEY_OPEN,
    RKEY_TOC,
    RKEY_JUMP,
    RKEY_THEME,
    RKEY_SPACING,
    RKEY_MENU,
    RKEY_DELETE,
    RKEY_BOOKMARK,
    RKEY_SEARCH,
    RKEY_REFLOW,
    RKEY_TAB1,
    RKEY_TAB2,
    RKEY_TAB3,
    RKEY_TAB4,
    RKEY_FONT_UP,
    RKEY_FONT_DOWN,
    RKEY_CHAR
} ReaderKey;

int hal_get_width(void);
int hal_get_height(void);

void hal_clear(uint32_t rgb);
void hal_put_pixel(int x, int y, uint32_t rgb);
void hal_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void hal_draw_rect(int x, int y, int w, int h, uint32_t rgb);
void hal_fill_circle(int cx, int cy, int r, uint32_t rgb);
void hal_draw_circle(int cx, int cy, int r, uint32_t rgb);
void hal_draw_line(int x0, int y0, int x1, int y1, uint32_t rgb);

void hal_blit_rgb888_scaled(const uint8_t *rgb, int img_w, int img_h,
                            int dst_x, int dst_y, int dst_w, int dst_h,
                            int clip_x, int clip_y, int clip_w, int clip_h,
                            int invert_colors);

void hal_set_text_bg(uint32_t rgb);
void hal_draw_line_utf8(int x, int y, const char *utf8_text, int byte_len, int max_px_w, uint32_t rgb);
void hal_clear_text_in_rect(int rx, int ry, int rw, int rh);
void hal_dim_rect(int rx, int ry, int rw, int rh);
void hal_present(void);

int  hal_draw_image_file(const char *filepath, int x, int y, int w, int h);

const char *hal_get_library_path(void);
int  hal_open_file_dialog(char *out_path, int max_len);
int  hal_find_sibling_image(const char *current_path, int delta, char *out_path, int max_len);
void hal_request_quit(void);

#endif
