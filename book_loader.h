#ifndef BOOK_LOADER_H
#define BOOK_LOADER_H

#include <stddef.h>
#include <stdint.h>

#define BOOK_MAX_CHAPTERS 512
#define BOOK_MAX_IMAGES   128

typedef enum {
    BOOK_FMT_UNKNOWN = 0,
    BOOK_FMT_TXT,
    BOOK_FMT_EPUB,
    BOOK_FMT_PDF,
    BOOK_FMT_BMP,
    BOOK_FMT_PNG
} BookFormat;

typedef struct {
    size_t offset;
    char title[88];
} BookChapter;

typedef struct {
    char zip_path[192];
} BookImage;

typedef struct {
    BookFormat format;
    char title[96];
    char author[64];
    char cover_zip_path[192];
    char *text;
    size_t text_len;
    BookChapter chapters[BOOK_MAX_CHAPTERS];
    int chapter_count;
    BookImage images[BOOK_MAX_IMAGES];
    int image_count;
} BookDoc;

BookFormat  book_detect_format(const char *filepath);
const char *book_format_tag(BookFormat fmt);

int  book_load_document(const char *filepath, BookDoc *out_doc);
void book_doc_free(BookDoc *doc);

/* 從 EPUB 檔案內按需解壓並解碼指定圖片 (JPEG / PNG -> RGB888，呼叫端 free) */
uint8_t *book_extract_epub_image(const char *epub_path, const char *zip_path, int *out_w, int *out_h);

/* 載入已快取的書籍封面縮圖 (<filepath>.cov，呼叫端 free) */
uint8_t *book_load_cover_thumb(const char *filepath, int *out_w, int *out_h);

#endif
