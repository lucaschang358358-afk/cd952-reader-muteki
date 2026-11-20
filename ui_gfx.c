#include "ui_gfx.h"
#include <string.h>

static const uint8_t FONT5X7[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    {0x23,0x13,0x08,0x64,0x62}, {0x20,0x10,0x08,0x04,0x02},
    {0x08,0x08,0x3E,0x08,0x08}, {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x60,0x60,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}
};

static int ascii_idx(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'Z') return 10 + (ch - 'A');
    if (ch >= 'a' && ch <= 'z') return 10 + (ch - 'a');
    switch (ch) {
        case '%': return 36; case '/': return 37; case '+': return 38;
        case '-': return 39; case ':': return 40; case '.': return 41;
        case '>': return 42; default: return -1;
    }
}

uint32_t ui_blend_color(uint32_t a, uint32_t b, int pct_b) {
    if (pct_b <= 0) return a;
    if (pct_b >= 100) return b;
    int pa = 100 - pct_b;
    int r  = ((int)((a >> 16) & 0xFF) * pa + (int)((b >> 16) & 0xFF) * pct_b) / 100;
    int g  = ((int)((a >> 8) & 0xFF)  * pa + (int)((b >> 8) & 0xFF)  * pct_b) / 100;
    int bl = ((int)(a & 0xFF)         * pa + (int)(b & 0xFF)         * pct_b) / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

void ui_fill_round_rect(int x, int y, int w, int h, int r, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    if (r <= 1) { hal_fill_rect(x, y, w, h, rgb); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    hal_fill_rect(x, y + r, w, h - r * 2, rgb);
    int r2 = r * r;
    for (int dy = 0; dy < r; dy++) {
        int cy = r - 1 - dy, dx = 0;
        while ((dx + 1) * (dx + 1) + cy * cy <= r2) dx++;
        int inset = r - dx;
        if (inset < 0) inset = 0;
        hal_fill_rect(x + inset, y + dy, w - inset * 2, 1, rgb);
        hal_fill_rect(x + inset, y + h - 1 - dy, w - inset * 2, 1, rgb);
    }
}

void ui_draw_round_rect(int x, int y, int w, int h, int r, uint32_t rgb) {
    if (w <= 2 || h <= 2) return;
    if (r <= 1) { hal_draw_rect(x, y, w, h, rgb); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    hal_fill_rect(x + r, y, w - r * 2, 1, rgb);
    hal_fill_rect(x + r, y + h - 1, w - r * 2, 1, rgb);
    hal_fill_rect(x, y + r, 1, h - r * 2, rgb);
    hal_fill_rect(x + w - 1, y + r, 1, h - r * 2, rgb);
    int r2 = r * r;
    for (int dy = 0; dy < r; dy++) {
        int cy = r - 1 - dy, dx = 0;
        while ((dx + 1) * (dx + 1) + cy * cy <= r2) dx++;
        int inset = r - dx;
        hal_put_pixel(x + inset, y + dy, rgb);
        hal_put_pixel(x + w - 1 - inset, y + dy, rgb);
        hal_put_pixel(x + inset, y + h - 1 - dy, rgb);
        hal_put_pixel(x + w - 1 - inset, y + h - 1 - dy, rgb);
    }
}

void ui_card(int x, int y, int w, int h, int r, uint32_t bg, uint32_t border, uint32_t shadow, int selected, uint32_t accent) {
    ui_fill_round_rect(x + 3, y + 4, w, h, r, shadow);
    ui_fill_round_rect(x, y, w, h, r, bg);
    if (selected) {
        ui_draw_round_rect(x, y, w, h, r, accent);
        ui_draw_round_rect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, accent);
    } else {
        ui_draw_round_rect(x, y, w, h, r, border);
    }
}

void ui_progress_bar(int x, int y, int w, int h, int pct, uint32_t track_col, uint32_t fill_col) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int r = h / 2;
    ui_fill_round_rect(x, y, w, h, r, track_col);
    int fw = (w * pct) / 100;
    if (fw > 0) {
        if (fw < h) fw = h;
        if (fw > w) fw = w;
        ui_fill_round_rect(x, y, fw, h, r, fill_col);
    }
}

void ui_draw_ascii(int x, int y, const char *str, int scale, uint32_t color) {
    if (!str || scale < 1) return;
    int cx = x;
    for (int i = 0; str[i]; i++) {
        int idx = ascii_idx(str[i]);
        if (idx >= 0) {
            for (int col = 0; col < 5; col++) {
                uint8_t bits = FONT5X7[idx][col];
                for (int row = 0; row < 7; row++) {
                    if (bits & (1u << row)) hal_fill_rect(cx + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        cx += 6 * scale;
    }
}

int ui_ascii_width(const char *str, int scale) {
    if (!str) return 0;
    int n = (int)strlen(str);
    return n > 0 ? (n * 6 * scale - scale) : 0;
}

void ui_draw_badge(int x, int y, const char *tag, uint32_t bg, uint32_t fg) {
    int tw = ui_ascii_width(tag, 1);
    ui_fill_round_rect(x, y, tw + 12, 15, 5, bg);
    ui_draw_ascii(x + 6, y + 4, tag, 1, fg);
}

uint32_t ui_format_color(BookFormat fmt) {
    switch (fmt) {
        case BOOK_FMT_EPUB: return 0x10B981;
        case BOOK_FMT_PDF:  return 0xEF4444;
        case BOOK_FMT_BMP:  return 0xF59E0B;
        case BOOK_FMT_PNG:  return 0x8B5CF6;
        default:            return 0x3B82F6;
    }
}

static const uint32_t COVER_PAL[6][2] = {
    { 0x254E70, 0x37718E }, { 0x8F3946, 0xB85D69 }, { 0x2B594B, 0x43826D },
    { 0x56427A, 0x7961A3 }, { 0x9C5A2E, 0xC47B47 }, { 0x334155, 0x4E617A }
};

static unsigned hash_title(const char *s) {
    unsigned h = 5381u;
    if (!s) return 0;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}

void ui_draw_book_cover(int x, int y, int w, int h, const char *title, BookFormat fmt, int is_dark) {
    int idx = (int)(hash_title(title) % 6u);
    uint32_t c1 = COVER_PAL[idx][0], c2 = COVER_PAL[idx][1];
    uint32_t spine = ui_blend_color(c1, 0x000000, 32);
    uint32_t gloss = ui_blend_color(c2, 0xFFFFFF, 22);
    uint32_t shcol = is_dark ? 0x090B0E : 0xC5BFB3;

    ui_fill_round_rect(x + 4, y + 4, w, h, 7, shcol);
    ui_fill_round_rect(x + 2, y + 2, w, h - 2, 6, 0xEDEBE4);
    ui_fill_round_rect(x, y, w - 2, h - 2, 6, c1);
    hal_fill_rect(x + 10, y + 10, w - 18, (h * 42) / 100, c2);
    hal_fill_rect(x + 10, y + 10 + (h * 42) / 100, w - 18, 2, gloss);
    hal_fill_rect(x, y + 4, 8, h - 10, spine);
    hal_fill_rect(x + 8, y + 2, 2, h - 6, gloss);

    int cr = w / 7; if (cr < 8) cr = 8;
    hal_fill_circle(x + w - cr - 10, y + h - cr - 12, cr, ui_blend_color(c1, 0xFFFFFF, 14));
    hal_draw_circle(x + w - cr - 10, y + h - cr - 12, cr + 4, ui_blend_color(c2, 0xFFFFFF, 25));

    const char *tag = book_format_tag(fmt);
    int bw = ui_ascii_width(tag, 1) + 12;
    ui_draw_badge(x + w - bw - 8, y + 8, tag, ui_format_color(fmt), 0xFFFFFF);
}

void ui_icon_shelf(int cx, int cy, uint32_t col) {
    hal_fill_rect(cx - 10, cy - 8, 5, 14, col);
    hal_fill_rect(cx - 3,  cy - 10, 5, 16, col);
    hal_draw_rect(cx + 4,  cy - 7, 5, 13, col);
    hal_fill_rect(cx - 12, cy + 8, 24, 2, col);
}

void ui_icon_list(int cx, int cy, uint32_t col) {
    for (int i = 0; i < 3; i++) {
        int ry = cy - 7 + i * 7;
        hal_fill_rect(cx - 10, ry, 3, 3, col);
        hal_fill_rect(cx - 4,  ry, 14, 3, col);
    }
}

void ui_icon_palette(int cx, int cy, uint32_t col) {
    hal_draw_circle(cx, cy, 10, col);
    hal_draw_circle(cx, cy, 9, col);
    hal_fill_circle(cx - 4, cy - 3, 2, 0xEF4444);
    hal_fill_circle(cx + 4, cy - 3, 2, 0x10B981);
    hal_fill_circle(cx,     cy + 4, 2, 0x3B82F6);
}

void ui_icon_import(int cx, int cy, uint32_t col) {
    ui_draw_round_rect(cx - 10, cy - 10, 20, 20, 5, col);
    hal_fill_rect(cx - 5, cy - 1, 10, 2, col);
    hal_fill_rect(cx - 1, cy - 5, 2, 10, col);
}

void ui_icon_open_book(int cx, int cy, uint32_t accent, uint32_t page_col, uint32_t line_col) {
    ui_fill_round_rect(cx - 56, cy - 28, 112, 62, 8, accent);
    ui_fill_round_rect(cx - 50, cy - 32, 48, 58, 5, page_col);
    ui_fill_round_rect(cx + 2,  cy - 32, 48, 58, 5, page_col);
    hal_fill_rect(cx - 1, cy - 32, 2, 58, accent);
    for (int i = 0; i < 4; i++) {
        int ly = cy - 20 + i * 11;
        hal_fill_rect(cx - 40, ly, 30, 3, line_col);
        hal_fill_rect(cx + 10, ly, 30, 3, line_col);
    }
}
