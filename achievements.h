#ifndef ACHIEVEMENTS_H
#define ACHIEVEMENTS_H
#include <stdint.h>
#define NUM_ACHIEVEMENTS 60
#define NUM_ACH_CATS      6
#define NUM_WEEKLY_QUESTS 3
typedef struct { int id, cat; const char *name, *desc; int target, xp; } AchievementDef;
typedef struct { const char *title; int target, cur, xp, done; } WeeklyQuestView;
typedef struct {
    uint32_t total_searches, total_bookmarks_added, total_txt_edits, total_auto_turns;
    uint32_t total_themes_tried, total_covers_styled, total_reports_viewed, total_eye_breaks;
    uint32_t night_owl_reads, early_bird_reads, weekend_reads, quests_completed;
    int week_stamp;
    uint32_t week_base_bmarks, week_base_searches, week_base_auto, week_base_edits;
    uint8_t week_quest_done[NUM_WEEKLY_QUESTS], unlocked[NUM_ACHIEVEMENTS];
} RpgProfile;
typedef struct {
    uint32_t lifetime_secs, lifetime_pages, week_secs, week_pages;
    int lifetime_days, streak, goal_days, book_count, finished_books, rated_books, folder_count;
    int epub_count, pdf_count, txt_count, img_count, cur_year, cur_month, cur_day, goal_mins;
    int week_active_days, week_goal_days;
} RpgSnapshot;
void rpg_init(const char *filepath);
void rpg_save(const char *filepath);
RpgProfile *rpg_profile(void);
const AchievementDef *rpg_get_achievement(int id);
const char *rpg_cat_name(int cat);
int  rpg_evaluate(const RpgSnapshot *snap, char *out_toast, int toast_sz);
int  rpg_count_unlocked(void);
int  rpg_get_achievement_progress(int id, const RpgSnapshot *snap);
uint32_t    rpg_calc_total_xp(const RpgSnapshot *snap);
int         rpg_calc_level(uint32_t total_xp, int *out_cur_lv_xp, int *out_need_lv_xp);
const char *rpg_level_title(int level);
void rpg_get_weekly_quests(const RpgSnapshot *snap, WeeklyQuestView out_q[NUM_WEEKLY_QUESTS]);
#endif
