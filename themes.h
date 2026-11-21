#ifndef THEMES_H
#define THEMES_H
#include <stdint.h>
#define NUM_THEMES 12
typedef struct {
    const char *name;
    const char *eng;
    int is_dark;
    uint32_t bg, rail_bg, card_bg, card_border, shadow, bar_bg, text, muted, accent, track;
} AppTheme;
const AppTheme *theme_get(int idx);
#endif
