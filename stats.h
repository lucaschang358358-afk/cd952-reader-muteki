#ifndef STATS_H
#define STATS_H
#include <stdint.h>
#define STATS_MAX_DAYS 732
typedef struct { int year, month, day; uint32_t seconds, pages; } DayStat;
void stats_init(const char *filepath);
void stats_save(const char *filepath);
void stats_add_reading(int year, int month, int day, uint32_t add_secs, uint32_t add_pages);
void stats_get_day(int year, int month, int day, uint32_t *out_secs, uint32_t *out_pages);
int  stats_is_leap(int year);
int  stats_days_in_year(int year);
int  stats_days_in_month(int year, int month);
int  stats_day_of_week(int year, int month, int day);
int  stats_day_of_year(int year, int month, int day);
void stats_doy_to_ymd(int year, int doy, int *out_m, int *out_d);
uint32_t stats_year_total_secs(int year);
int      stats_year_active_days(int year);
int      stats_calc_streak(int cur_y, int cur_m, int cur_d);
void     stats_get_last_7_days(int cur_y, int cur_m, int cur_d, uint32_t out_secs[7], uint32_t out_pages[7], char out_labels[7][8]);
void     stats_get_12_months(int year, uint32_t out_secs[12], uint32_t out_pages[12]);
uint32_t stats_lifetime_secs(void);
uint32_t stats_lifetime_pages(void);
int      stats_lifetime_active_days(void);
int      stats_count_goal_days(int goal_mins);
void     stats_get_week_totals(int cur_y, int cur_m, int cur_d, int goal_mins, uint32_t *out_secs, uint32_t *out_pages, int *out_active_days, int *out_goal_days);
#endif
