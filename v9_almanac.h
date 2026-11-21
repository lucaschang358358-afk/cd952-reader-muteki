#ifndef V9_ALMANAC_H
#define V9_ALMANAC_H
#include <stdint.h>
const char *almanac_daily_quote(int year, int month, int day);
const char *almanac_radar_archetype(const int scores[5]);
uint32_t    almanac_paper_grain(int x, int y, int theme_idx);
#endif
