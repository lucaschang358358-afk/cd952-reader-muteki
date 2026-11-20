#ifndef PDF_ENGINE_H
#define PDF_ENGINE_H

#include <stddef.h>
#include <stdint.h>

typedef struct PdfDoc PdfDoc;

/* 開啟並建立 PDF 頁面索引 (延遲解碼，開啟大檔不卡頓) */
PdfDoc *pdf_open(const char *filepath);
void    pdf_close(PdfDoc *doc);

int         pdf_page_count(const PdfDoc *doc);
const char *pdf_get_title(const PdfDoc *doc);
const char *pdf_get_author(const PdfDoc *doc);
void        pdf_get_page_size(const PdfDoc *doc, int page_idx, float *out_w, float *out_h);

/*
 * 真正視覺化渲染 PDF 頁面：
 * - 繪製真實頁面紙張、陰影、向量矩形與線條
 * - 解碼並精準定位貼上內嵌 JPEG (/DCTDecode) 與 Flate (/FlateDecode) 圖片
 * - 依真實 PDF 頁面座標 (BT/ET/Tm/Td/Tj/TJ) 繪製中英文定位文字
 * zoom_mode: 0=適應整頁, 1=適應寬度, 2=135%, 3=175%, 4=225%
 * 回傳值：本頁實際渲染後的 (page_w, page_h) 供平移邊界控制
 */
void pdf_render_page(PdfDoc *doc, int page_idx,
                     int vx, int vy, int vw, int vh,
                     int zoom_mode, int pan_x, int pan_y,
                     uint32_t backdrop_rgb, int night_invert,
                     int *out_page_w, int *out_page_h);

/* 抽取單頁文字 (供全文搜尋與可選 Reflow 放大閱讀模式使用，呼叫端 free) */
char *pdf_extract_page_text(PdfDoc *doc, int page_idx, size_t *out_len);

#endif
