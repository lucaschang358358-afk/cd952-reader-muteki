#ifndef BOOK_LOADER_H
#define BOOK_LOADER_H

#include <stddef.h>
#include <stdint.h>

#define BOOK_MAX_CHAPTERS 512

typedef enum {
    BOOK_FMT_UNKNOWN = 0,
    BOOK_FMT_TXT,
    BOOK_FMT_EPUB,
    BOOK_FMT_PDF,
    BOOK_FMT_BMP
} BookFormat;

typedef struct {
    size_t offset;
    char title[88];
} BookChapter;

typedef struct {
    BookFormat format;
    char title[96];
    char author[64];
    char *text;
    size_t text_len;
    BookChapter chapters[BOOK_MAX_CHAPTERS];
    int chapter_count;
} BookDoc;

BookFormat  book_detect_format(const char *filepath);
const char *book_format_tag(BookFormat fmt);

/* 讀取並解析 .txt / .epub (PDF 由獨立的真視覺化 pdf_engine.c 處理) */
int  book_load_document(const char *filepath, BookDoc *out_doc);
void book_doc_free(BookDoc *doc);

#endif
