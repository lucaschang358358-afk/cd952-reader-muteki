#ifndef READER_CORE_H
#define READER_CORE_H

#include "hal.h"

void reader_init(void);
void reader_shutdown(void);
int  reader_open_file(const char *filepath);
void reader_on_resize(void);
void reader_on_key(KeyCode key);
void reader_render(void);

#endif
