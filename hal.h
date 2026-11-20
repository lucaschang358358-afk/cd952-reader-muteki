#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    KEY_NONE = 0,
    KEY_PREV,
    KEY_NEXT,
    KEY_OPEN,
    KEY_THEME,
    KEY_FONT_UP,
    KEY_FONT_DOWN,
    KEY_QUIT
} KeyCode;

/* 螢幕尺寸 */
int hal_get_width(void);
int hal_get_height(void);

/* 繪圖介面 (rgb 格式為 0xRRGGBB) */
void hal_clear(uint32_t rgb);
void hal_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void hal_draw_line_utf8(int x, int y, const char *utf8_text, int byte_len, int font_size, uint32_t rgb);
int  hal_draw_bmp_file(const char *filepath, int x, int y, int w, int h);

/* 檔案與系統介面 */
int  hal_open_file_dialog(char *out_path, int max_len);
int  hal_find_sibling_bmp(const char *current_path, int delta, char *out_path, int max_len);
void hal_request_quit(void);

#endif
