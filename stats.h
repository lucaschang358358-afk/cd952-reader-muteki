#ifndef STATS_H
#define STATS_H

#include <stdint.h>

#define STATS_MAX_DAYS 732 /* 最多可連續保存超過兩年的每日紀錄 */

typedef struct {
    int year;
    int month;
    int day;
    uint32_t seconds;
    uint32_t pages;
} DayStat;

void stats_init(const char *filepath);
void stats_save(const char *filepath);

void stats_add_reading(int year, int month, int day, uint32_t add_secs, uint32_t add_pages);
void stats_get_day(int year, int month, int day, uint32_t *out_secs, uint32_t *out_pages);

/* 年度統計查詢 */
int      stats_is_leap(int year);
int      stats_days_in_year(int year);
int      stats_days_in_month(int year, int month);
int      stats_day_of_week(int year, int month, int day); /* 0=Sun .. 6=Sat */
int      stats_day_of_year(int year, int month, int day); /* 0-based: 0..364/365 */
void     stats_doy_to_ymd(int year, int doy, int *out_m, int *out_d);

uint32_t stats_year_total_secs(int year);
int      stats_year_active_days(int year);
int      stats_calc_streak(int cur_y, int cur_m, int cur_d);

#endif
