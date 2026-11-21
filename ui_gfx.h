#ifndef UI_GFX_H
#define UI_GFX_H

#include <stdint.h>
#include "hal.h"
#include "book_loader.h"

uint32_t ui_blend_color(uint32_t a, uint32_t b, int pct_b);
void ui_fill_round_rect(int x, int y, int w, int h, int r, uint32_t rgb);
void ui_draw_round_rect(int x, int y, int w, int h, int r, uint32_t rgb);
void ui_card(int x, int y, int w, int h, int r, uint32_t bg, uint32_t border, uint32_t shadow, int selected, uint32_t accent);
void ui_progress_bar(int x, int y, int w, int h, int pct, uint32_t track_col, uint32_t fill_col);

void ui_draw_ascii(int x, int y, const char *str, int scale, uint32_t color);
int  ui_ascii_width(const char *str, int scale);
void ui_draw_badge(int x, int y, const char *tag, uint32_t bg, uint32_t fg);

uint32_t ui_format_color(BookFormat fmt);
void ui_draw_book_cover(int x, int y, int w, int h, const char *title, BookFormat fmt, int is_dark);
void ui_draw_book_cover_img(int x, int y, int w, int h, const uint8_t *rgb, int img_w, int img_h, BookFormat fmt, int is_dark);
void ui_draw_folder_card(int x, int y, int w, int h, const uint8_t *thumb_rgb, int tw, int th, int book_count, uint32_t accent, int is_dark);

void ui_icon_shelf(int cx, int cy, uint32_t col);
void ui_icon_list(int cx, int cy, uint32_t col);
void ui_icon_stats(int cx, int cy, uint32_t col);
void ui_icon_palette(int cx, int cy, uint32_t col);
void ui_icon_import(int cx, int cy, uint32_t col);
void ui_icon_open_book(int cx, int cy, uint32_t accent, uint32_t page_col, uint32_t line_col);

#endif
