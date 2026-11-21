#include "txt_tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int txt_count_utf8_chars(const char *s, size_t len) {
    if (!s || len == 0) return 0;
    int cnt = 0; size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\x01') { while (i < len && s[i] != '\n') i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if ((c & 0x80u) == 0) { cnt++; i += 1; }
        else if ((c & 0xE0u) == 0xC0u && i + 1 < len) { cnt++; i += 2; }
        else if ((c & 0xF0u) == 0xE0u && i + 2 < len) { cnt++; i += 3; }
        else if ((c & 0xF8u) == 0xF0u && i + 3 < len) { cnt++; i += 4; }
        else i += 1;
    }
    return cnt;
}
int txt_estimate_read_mins(int char_count) {
    if (char_count <= 0) return 1;
    int m = (char_count + 449) / 450;
    return m < 1 ? 1 : m;
}
void txt_clean_line_buf(char *line, size_t cap, int normalize_punct) {
    if (!line || cap < 8) return;
    size_t len = strlen(line), s = 0, e = len;
    while (s < len) {
        if (line[s] == ' ' || line[s] == '\t' || line[s] == '\r' || line[s] == '\n') s++;
        else if (s + 2 < len && (unsigned char)line[s] == 0xE3 && (unsigned char)line[s+1] == 0x80 && (unsigned char)line[s+2] == 0x80) s += 3;
        else break;
    }
    while (e > s && (line[e-1] == ' ' || line[e-1] == '\t' || line[e-1] == '\r' || line[e-1] == '\n')) e--;
    char tmp[512]; size_t o = 0;
    for (size_t i = s; i < e && o + 4 < sizeof(tmp) && o + 4 < cap; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 && c != '\t') continue;
        if (normalize_punct) {
            if (c == ',') { tmp[o++] = (char)0xEF; tmp[o++] = (char)0xBC; tmp[o++] = (char)0x8C; continue; }
            if (c == '?') { tmp[o++] = (char)0xEF; tmp[o++] = (char)0xBC; tmp[o++] = (char)0x9F; continue; }
            if (c == '!') { tmp[o++] = (char)0xEF; tmp[o++] = (char)0xBC; tmp[o++] = (char)0x81; continue; }
            if (c == ':') { tmp[o++] = (char)0xEF; tmp[o++] = (char)0xBC; tmp[o++] = (char)0x9A; continue; }
        }
        tmp[o++] = (char)c;
    }
    tmp[o] = '\0'; memcpy(line, tmp, o + 1);
}
int txt_replace_line_and_save(const char *filepath, char **io_text, size_t *io_len, size_t line_start, size_t line_old_len, const char *new_line) {
    if (!filepath || !io_text || !*io_text || !io_len || !new_line || line_start > *io_len) return 0;
    if (line_start + line_old_len > *io_len) line_old_len = *io_len - line_start;
    size_t nlen = strlen(new_line), toff = line_start + line_old_len, tlen = *io_len - toff, total = line_start + nlen + tlen;
    char *nb = (char *)malloc(total + 1);
    if (!nb) return 0;
    if (line_start > 0) memcpy(nb, *io_text, line_start);
    if (nlen > 0) memcpy(nb + line_start, new_line, nlen);
    if (tlen > 0) memcpy(nb + line_start + nlen, *io_text + toff, tlen);
    nb[total] = '\0';
    FILE *f = fopen(filepath, "wb");
    if (!f) { free(nb); return 0; }
    size_t w = fwrite(nb, 1, total, f); fclose(f);
    if (w != total) { free(nb); return 0; }
    free(*io_text); *io_text = nb; *io_len = total;
    return 1;
}
