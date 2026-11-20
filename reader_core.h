#ifndef READER_CORE_H
#define READER_CORE_H

#include "hal.h"

void reader_init(void);
void reader_shutdown(void);
int  reader_open_file(const char *filepath);
void reader_on_resize(void);
void reader_on_key(ReaderKey key, char raw_char);
void reader_on_touch(int tx, int ty);
int  reader_tick_second(void); /* 每秒呼叫一次：累計閱讀時間並回傳是否需刷新時鐘 */
void reader_render(void);

#endif
