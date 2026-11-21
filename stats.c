#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DayStat g_days[STATS_MAX_DAYS];
static int g_day_count = 0;

int stats_is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int stats_days_in_year(int year) {
    return stats_is_leap(year) ? 366 : 365;
}

int stats_days_in_month(int year, int month) {
    static const int DM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 30;
    if (month == 2 && stats_is_leap(year)) return 29;
    return DM[month - 1];
}

/* Sakamoto 演算法：計算西元年月日對應星期幾 (0=日 .. 6=六) */
int stats_day_of_week(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

int stats_day_of_year(int year, int month, int day) {
    int doy = 0;
    for (int m = 1; m < month && m <= 12; m++) doy += stats_days_in_month(year, m);
    doy += (day - 1);
    if (doy < 0) doy = 0;
    int max_d = stats_days_in_year(year) - 1;
    if (doy > max_d) doy = max_d;
    return doy;
}

void stats_doy_to_ymd(int year, int doy, int *out_m, int *out_d) {
    int rem = doy;
    if (rem < 0) rem = 0;
    for (int m = 1; m <= 12; m++) {
        int dm = stats_days_in_month(year, m);
        if (rem < dm) {
            if (out_m) *out_m = m;
            if (out_d) *out_d = rem + 1;
            return;
        }
        rem -= dm;
    }
    if (out_m) *out_m = 12;
    if (out_d) *out_d = 31;
}

static void prev_date(int *y, int *m, int *d) {
    if (*d > 1) {
        (*d)--;
    } else if (*m > 1) {
        (*m)--;
        *d = stats_days_in_month(*y, *m);
    } else {
        (*y)--;
        *m = 12;
        *d = 31;
    }
}

void stats_init(const char *filepath) {
    g_day_count = 0;
    if (!filepath || !filepath[0]) return;
    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f) && g_day_count < STATS_MAX_DAYS) {
        int y = 0, m = 0, d = 0;
        unsigned long s = 0, p = 0;
        if (sscanf(line, "%d-%d-%d|%lu|%lu", &y, &m, &d, &s, &p) == 5) {
            if (y >= 2000 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                g_days[g_day_count].year = y;
                g_days[g_day_count].month = m;
                g_days[g_day_count].day = d;
                g_days[g_day_count].seconds = (uint32_t)s;
                g_days[g_day_count].pages = (uint32_t)p;
                g_day_count++;
            }
        }
    }
    fclose(f);
}

void stats_save(const char *filepath) {
    if (!filepath || !filepath[0]) return;
    FILE *f = fopen(filepath, "wb");
    if (!f) return;
    fprintf(f, "#STATS1\n");
    for (int i = 0; i < g_day_count; i++) {
        fprintf(f, "%04d-%02d-%02d|%lu|%lu\n",
                g_days[i].year, g_days[i].month, g_days[i].day,
                (unsigned long)g_days[i].seconds, (unsigned long)g_days[i].pages);
    }
    fclose(f);
}

void stats_add_reading(int year, int month, int day, uint32_t add_secs, uint32_t add_pages) {
    if (year < 2000 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) return;
    for (int i = 0; i < g_day_count; i++) {
        if (g_days[i].year == year && g_days[i].month == month && g_days[i].day == day) {
            g_days[i].seconds += add_secs;
            g_days[i].pages += add_pages;
            return;
        }
    }
    if (g_day_count >= STATS_MAX_DAYS) {
        memmove(&g_days[0], &g_days[1], (size_t)(STATS_MAX_DAYS - 1) * sizeof(DayStat));
        g_day_count = STATS_MAX_DAYS - 1;
    }
    g_days[g_day_count].year = year;
    g_days[g_day_count].month = month;
    g_days[g_day_count].day = day;
    g_days[g_day_count].seconds = add_secs;
    g_days[g_day_count].pages = add_pages;
    g_day_count++;
}

void stats_get_day(int year, int month, int day, uint32_t *out_secs, uint32_t *out_pages) {
    if (out_secs) *out_secs = 0;
    if (out_pages) *out_pages = 0;
    for (int i = 0; i < g_day_count; i++) {
        if (g_days[i].year == year && g_days[i].month == month && g_days[i].day == day) {
            if (out_secs) *out_secs = g_days[i].seconds;
            if (out_pages) *out_pages = g_days[i].pages;
            return;
        }
    }
}

uint32_t stats_year_total_secs(int year) {
    uint32_t sum = 0;
    for (int i = 0; i < g_day_count; i++) {
        if (g_days[i].year == year) sum += g_days[i].seconds;
    }
    return sum;
}

int stats_year_active_days(int year) {
    int cnt = 0;
    for (int i = 0; i < g_day_count; i++) {
        if (g_days[i].year == year && (g_days[i].seconds >= 10 || g_days[i].pages > 0)) cnt++;
    }
    return cnt;
}

int stats_calc_streak(int cur_y, int cur_m, int cur_d) {
    int y = cur_y, m = cur_m, d = cur_d;
    uint32_t s = 0, p = 0;
    stats_get_day(y, m, d, &s, &p);
    /* 若今天還沒讀，從昨天開始算連續天數 */
    if (s < 10 && p == 0) {
        prev_date(&y, &m, &d);
    }
    int streak = 0;
    for (int i = 0; i < 400; i++) {
        stats_get_day(y, m, d, &s, &p);
        if (s >= 10 || p > 0) {
            streak++;
            prev_date(&y, &m, &d);
        } else {
            break;
        }
    }
    return streak;
}

void stats_get_last_7_days(int cur_y, int cur_m, int cur_d, uint32_t out_secs[7], uint32_t out_pages[7], char out_labels[7][8]) {
    int y = cur_y, m = cur_m, d = cur_d;
    for (int i = 6; i >= 0; i--) {
        stats_get_day(y, m, d, &out_secs[i], &out_pages[i]);
        snprintf(out_labels[i], 8, "%d/%d", m, d);
        prev_date(&y, &m, &d);
    }
}
void stats_get_12_months(int year, uint32_t out_secs[12], uint32_t out_pages[12]) {
    for (int m = 0; m < 12; m++) { out_secs[m] = 0; out_pages[m] = 0; }
    for (int i = 0; i < g_day_count; i++) {
        if (g_days[i].year == year && g_days[i].month >= 1 && g_days[i].month <= 12) {
            out_secs[g_days[i].month - 1] += g_days[i].seconds;
            out_pages[g_days[i].month - 1] += g_days[i].pages;
        }
    }
}
uint32_t stats_lifetime_secs(void) { uint32_t s = 0; for (int i = 0; i < g_day_count; i++) s += g_days[i].seconds; return s; }
uint32_t stats_lifetime_pages(void) { uint32_t p = 0; for (int i = 0; i < g_day_count; i++) p += g_days[i].pages; return p; }
int stats_lifetime_active_days(void) { int c = 0; for (int i = 0; i < g_day_count; i++) if (g_days[i].seconds >= 10 || g_days[i].pages > 0) c++; return c; }
int stats_count_goal_days(int goal_mins) {
    if (goal_mins <= 0) return 0;
    uint32_t need = (uint32_t)goal_mins * 60u; int c = 0;
    for (int i = 0; i < g_day_count; i++) if (g_days[i].seconds >= need) c++;
    return c;
}
void stats_get_week_totals(int cur_y, int cur_m, int cur_d, int goal_mins, uint32_t *out_secs, uint32_t *out_pages, int *out_active_days, int *out_goal_days) {
    int dow = stats_day_of_week(cur_y, cur_m, cur_d), y = cur_y, m = cur_m, d = cur_d, ad = 0, gd = 0;
    uint32_t ts = 0, tp = 0, gneed = (uint32_t)(goal_mins > 0 ? goal_mins : 20) * 60u;
    for (int i = 0; i <= dow; i++) {
        uint32_t s = 0, p = 0; stats_get_day(y, m, d, &s, &p);
        ts += s; tp += p; if (s >= 10 || p > 0) ad++; if (s >= gneed) gd++;
        prev_date(&y, &m, &d);
    }
    if (out_secs) *out_secs = ts;
    if (out_pages) *out_pages = tp;
    if (out_active_days) *out_active_days = ad;
    if (out_goal_days) *out_goal_days = gd;
}
