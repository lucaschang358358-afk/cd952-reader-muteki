#ifndef PDF_ENGINE_H
#define PDF_ENGINE_H

#include <stddef.h>
#include <stdint.h>

typedef struct PdfDoc PdfDoc;

PdfDoc *pdf_open(const char *filepath);
void    pdf_close(PdfDoc *doc);

int         pdf_page_count(const PdfDoc *doc);
const char *pdf_get_title(const PdfDoc *doc);
const char *pdf_get_author(const PdfDoc *doc);
void        pdf_get_page_size(const PdfDoc *doc, int page_idx, float *out_w, float *out_h);

void pdf_render_page(PdfDoc *doc, int page_idx,
                     int vx, int vy, int vw, int vh,
                     int zoom_mode, int pan_x, int pan_y,
                     uint32_t backdrop_rgb, int night_invert,
                     int *out_page_w, int *out_page_h);

char *pdf_extract_page_text(PdfDoc *doc, int page_idx, size_t *out_len);

#endif
