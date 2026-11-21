#ifndef COVERS_H
#define COVERS_H
#include <stdint.h>
#include "book_loader.h"
#define NUM_COVER_STYLES 25
const char *cover_style_name(int style_id);
void ui_draw_styled_cover(int x, int y, int w, int h, const char *title, BookFormat fmt, int style_id, int is_dark);
#endif
