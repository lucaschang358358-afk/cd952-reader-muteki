#ifndef JPEG_TINY_H
#define JPEG_TINY_H

#include <stddef.h>
#include <stdint.h>

/*
 * 精簡純 C 整數 Baseline JPEG (SOF0) 解碼器
 * 支援 PDF /DCTDecode 內嵌圖片 (灰階、YCbCr 4:4:4 / 4:2:2 / 4:2:0)
 * 輸出：RGB888 (width * height * 3 bytes，呼叫端 free)
 */
uint8_t *tiny_jpeg_decode(const uint8_t *data, size_t len, int *out_w, int *out_h);

#endif
