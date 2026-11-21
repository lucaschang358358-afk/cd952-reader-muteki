#ifndef TXT_TOOLS_H
#define TXT_TOOLS_H
#include <stddef.h>
int  txt_count_utf8_chars(const char *s, size_t len);
int  txt_estimate_read_mins(int char_count);
void txt_clean_line_buf(char *line, size_t cap, int normalize_punct);
int  txt_replace_line_and_save(const char *filepath, char **io_text, size_t *io_len, size_t line_start, size_t line_old_len, const char *new_line);
#endif
