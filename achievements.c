#include "achievements.h"
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RPG_MAGIC 0x52504738u
static const AchievementDef ACH_LIST[NUM_ACHIEVEMENTS] = {
    {  0, 0, "初試書香", "累計閱讀滿 1 分鐘", 1, 50 },
    {  1, 0, "小試牛刀", "累計閱讀滿 15 分鐘", 15, 80 },
    {  2, 0, "專注半時", "累計閱讀滿 30 分鐘", 30, 100 },
    {  3, 0, "一小時達人", "累計閱讀滿 1 小時", 60, 120 },
    {  4, 0, "書海漫遊", "累計閱讀滿 3 小時", 180, 180 },
    {  5, 0, "沉醉其中", "累計閱讀滿 10 小時", 600, 250 },
    {  6, 0, "廢寢忘食", "累計閱讀滿 24 小時", 1440, 350 },
    {  7, 0, "半百時光", "累計閱讀滿 50 小時", 3000, 500 },
    {  8, 0, "百時宗師", "累計閱讀滿 100 小時", 6000, 800 },
    {  9, 0, "歲月書神", "累計閱讀滿 365 小時", 21900, 1500 },
    { 10, 1, "翻開第一頁", "累計翻閱 1 頁", 1, 40 },
    { 11, 1, "小有進展", "累計翻閱 25 頁", 25, 60 },
    { 12, 1, "百頁斬", "累計翻閱 100 頁", 100, 100 },
    { 13, 1, "手不釋卷", "累計翻閱 300 頁", 300, 150 },
    { 14, 1, "千頁行者", "累計翻閱 1000 頁", 1000, 250 },
    { 15, 1, "三千世界", "累計翻閱 3000 頁", 3000, 400 },
    { 16, 1, "五千破卷", "累計翻閱 5000 頁", 5000, 550 },
    { 17, 1, "萬頁書聖", "累計翻閱 10000 頁", 10000, 800 },
    { 18, 1, "三萬卷王", "累計翻閱 30000 頁", 30000, 1200 },
    { 19, 1, "無限翻頁神", "累計翻閱 100000 頁", 100000, 2000 },
    { 20, 2, "初日啟程", "累計打卡 1 天", 1, 50 },
    { 21, 2, "三日連勝", "連續閱讀 3 天", 3, 100 },
    { 22, 2, "一週全勤", "連續閱讀 7 天", 7, 180 },
    { 23, 2, "半月堅持", "連續閱讀 14 天", 14, 300 },
    { 24, 2, "滿月書痴", "連續閱讀 30 天", 30, 500 },
    { 25, 2, "百日築基", "連續閱讀 100 天", 100, 1000 },
    { 26, 2, "全年無休", "連續閱讀 365 天", 365, 3000 },
    { 27, 2, "今日達標", "達成每日目標 1 次", 1, 80 },
    { 28, 2, "目標守護者", "累計達標 10 天", 10, 250 },
    { 29, 2, "自律王者", "累計達標 50 天", 50, 700 },
    { 30, 3, "第一本藏書", "書庫收錄 1 本書", 1, 50 },
    { 31, 3, "迷你書架", "書庫收錄 5 本書", 5, 100 },
    { 32, 3, "私人書房", "書庫收錄 10 本書", 10, 180 },
    { 33, 3, "博覽藏書家", "書庫收錄 20 本書", 20, 350 },
    { 34, 3, "初次完食", "標記 1 本書為已完食", 1, 120 },
    { 35, 3, "五本達陣", "累計完食 5 本書", 5, 300 },
    { 36, 3, "十全十美", "累計完食 10 本書", 10, 600 },
    { 37, 3, "系列收藏家", "建立 1 個書架資料夾", 1, 80 },
    { 38, 3, "整理強迫症", "建立 4 個書架資料夾", 4, 200 },
    { 39, 3, "五星書評家", "幫 3 本書評星級", 3, 150 },
    { 40, 4, "PDF 研究員", "匯入並閱讀 PDF 文件", 1, 80 },
    { 41, 4, "EPUB 輕小說迷", "匯入並閱讀 EPUB 電子書", 1, 80 },
    { 42, 4, "純文字情懷", "匯入並閱讀 TXT 小說", 1, 80 },
    { 43, 4, "圖像閱讀者", "匯入 PNG 或 BMP 圖片書", 1, 80 },
    { 44, 4, "書籤獵人", "累計新增 5 個手動書籤", 5, 120 },
    { 45, 4, "全文偵探", "使用全文關鍵字搜尋 3 次", 3, 120 },
    { 46, 4, "懶人自動檔", "使用自動翻頁 10 次", 10, 150 },
    { 47, 4, "文字校對師", "使用 TXT 內建修字器 1 次", 1, 150 },
    { 48, 4, "封面設計師", "幫書籍套用繪製封面 3 次", 3, 150 },
    { 49, 4, "風格玩家", "切換閱讀主題風格 5 次", 5, 120 },
    { 50, 5, "晨光早鳥", "清晨 5~8 點閱讀 1 次", 1, 150 },
    { 51, 5, "深夜夜貓子", "深夜 23~5 點閱讀 1 次", 1, 150 },
    { 52, 5, "週末學霸", "週末閱讀 3 次", 3, 150 },
    { 53, 5, "每日復盤家", "檢視每日總結報告 3 次", 3, 120 },
    { 54, 5, "護眼好寶寶", "完成護眼休息提醒 2 次", 2, 150 },
    { 55, 5, "任務新手", "完成 1 個每週隨機任務", 1, 150 },
    { 56, 5, "任務達人", "累計完成 5 個每週任務", 5, 350 },
    { 57, 5, "賞金獵人", "累計完成 15 個每週任務", 15, 700 },
    { 58, 5, "全格式通吃", "同時擁有 EPUB+PDF+TXT", 3, 250 },
    { 59, 5, "傳奇全收集", "解鎖 40 個以上獎盃！", 40, 1500 },
};
static const char *CAT_NAMES[NUM_ACH_CATS] = { "時長", "翻頁", "打卡", "藏書", "探索", "彩蛋" };
static RpgProfile g_rpg;
const AchievementDef *rpg_get_achievement(int id) { return &ACH_LIST[(id >= 0 && id < NUM_ACHIEVEMENTS) ? id : 0]; }
const char *rpg_cat_name(int cat) { return (cat >= 0 && cat < NUM_ACH_CATS) ? CAT_NAMES[cat] : "全部"; }
RpgProfile *rpg_profile(void) { return &g_rpg; }
void rpg_init(const char *filepath) {
    memset(&g_rpg, 0, sizeof(g_rpg));
    if (!filepath || !filepath[0]) return;
    FILE *f = fopen(filepath, "rb"); if (!f) return;
    uint32_t m = 0; if (fread(&m, sizeof(m), 1, f) == 1 && m == RPG_MAGIC) (void)fread(&g_rpg, sizeof(g_rpg), 1, f);
    fclose(f);
}
void rpg_save(const char *filepath) {
    if (!filepath || !filepath[0]) return;
    FILE *f = fopen(filepath, "wb"); if (!f) return;
    uint32_t m = RPG_MAGIC; fwrite(&m, sizeof(m), 1, f); fwrite(&g_rpg, sizeof(g_rpg), 1, f); fclose(f);
}
static void ensure_week_synced(const RpgSnapshot *snap) {
    int ws = snap->cur_year * 100 + (stats_day_of_year(snap->cur_year, snap->cur_month, snap->cur_day) / 7) + 1;
    if (g_rpg.week_stamp != ws) {
        g_rpg.week_stamp = ws;
        g_rpg.week_base_bmarks = g_rpg.total_bookmarks_added;
        g_rpg.week_base_searches = g_rpg.total_searches;
        g_rpg.week_base_auto = g_rpg.total_auto_turns;
        g_rpg.week_base_edits = g_rpg.total_txt_edits;
        g_rpg.week_quest_done[0] = g_rpg.week_quest_done[1] = g_rpg.week_quest_done[2] = 0;
    }
}
void rpg_get_weekly_quests(const RpgSnapshot *snap, WeeklyQuestView out_q[NUM_WEEKLY_QUESTS]) {
    ensure_week_synced(snap);
    uint32_t seed = (uint32_t)g_rpg.week_stamp * 2654435761u + 137u;
    int chosen[3];
    for (int i = 0; i < 3; i++) {
        seed = seed * 1664525u + 1013904223u;
        int qid = (int)((seed >> 16) % 9u);
        while ((i > 0 && qid == chosen[0]) || (i > 1 && qid == chosen[1])) qid = (qid + 1) % 9;
        chosen[i] = qid;
        WeeklyQuestView *q = &out_q[i]; memset(q, 0, sizeof(*q));
        switch (qid) {
            case 0: q->title="本週累計閱讀 60 分鐘"; q->target=60; q->cur=(int)(snap->week_secs/60u); q->xp=200; break;
            case 1: q->title="本週累計閱讀 120 分鐘"; q->target=120; q->cur=(int)(snap->week_secs/60u); q->xp=350; break;
            case 2: q->title="本週翻閱 80 頁書籍"; q->target=80; q->cur=(int)snap->week_pages; q->xp=180; break;
            case 3: q->title="本週翻閱 200 頁書籍"; q->target=200; q->cur=(int)snap->week_pages; q->xp=320; break;
            case 4: q->title="本週至少打卡閱讀 3 天"; q->target=3; q->cur=snap->week_active_days; q->xp=220; break;
            case 5: q->title="本週達成每日目標 2 天"; q->target=2; q->cur=snap->week_goal_days; q->xp=260; break;
            case 6: q->title="本週新增 2 個手動書籤"; q->target=2; q->cur=(int)(g_rpg.total_bookmarks_added-g_rpg.week_base_bmarks); q->xp=150; break;
            case 7: q->title="本週使用全文搜尋 2 次"; q->target=2; q->cur=(int)(g_rpg.total_searches-g_rpg.week_base_searches); q->xp=150; break;
            default: q->title="本週體驗自動翻頁 5 次"; q->target=5; q->cur=(int)(g_rpg.total_auto_turns-g_rpg.week_base_auto); q->xp=160; break;
        }
        if (q->cur < 0) q->cur = 0;
        q->done = (g_rpg.week_quest_done[i] || q->cur >= q->target) ? 1 : 0;
    }
}
int rpg_count_unlocked(void) { int c = 0; for (int i = 0; i < NUM_ACHIEVEMENTS; i++) if (g_rpg.unlocked[i]) c++; return c; }
int rpg_get_achievement_progress(int id, const RpgSnapshot *s) {
    if (id >= 0 && id <= 9) return (int)(s->lifetime_secs / 60u);
    if (id >= 10 && id <= 19) return (int)s->lifetime_pages;
    switch (id) {
        case 20: return s->lifetime_days;
        case 21: case 22: case 23: case 24: case 25: case 26: return s->streak;
        case 27: case 28: case 29: return s->goal_days;
        case 30: case 31: case 32: case 33: return s->book_count;
        case 34: case 35: case 36: return s->finished_books;
        case 37: case 38: return s->folder_count;
        case 39: return s->rated_books;
        case 40: return s->pdf_count;
        case 41: return s->epub_count;
        case 42: return s->txt_count;
        case 43: return s->img_count;
        case 44: return (int)g_rpg.total_bookmarks_added;
        case 45: return (int)g_rpg.total_searches;
        case 46: return (int)g_rpg.total_auto_turns;
        case 47: return (int)g_rpg.total_txt_edits;
        case 48: return (int)g_rpg.total_covers_styled;
        case 49: return (int)g_rpg.total_themes_tried;
        case 50: return (int)g_rpg.early_bird_reads;
        case 51: return (int)g_rpg.night_owl_reads;
        case 52: return (int)g_rpg.weekend_reads;
        case 53: return (int)g_rpg.total_reports_viewed;
        case 54: return (int)g_rpg.total_eye_breaks;
        case 55: case 56: case 57: return (int)g_rpg.quests_completed;
        case 58: return (s->epub_count>0?1:0)+(s->pdf_count>0?1:0)+(s->txt_count>0?1:0);
        case 59: return rpg_count_unlocked();
        default: return 0;
    }
}
int rpg_evaluate(const RpgSnapshot *snap, char *out_toast, int toast_sz) {
    int newly = 0; WeeklyQuestView wq[NUM_WEEKLY_QUESTS]; rpg_get_weekly_quests(snap, wq);
    for (int i = 0; i < NUM_WEEKLY_QUESTS; i++) {
        if (!g_rpg.week_quest_done[i] && wq[i].cur >= wq[i].target) {
            g_rpg.week_quest_done[i] = 1; g_rpg.quests_completed++; newly++;
            if (out_toast && toast_sz > 0) snprintf(out_toast, (size_t)toast_sz, "完成週任務：%s (+%d XP)", wq[i].title, wq[i].xp);
        }
    }
    for (int i = 0; i < NUM_ACHIEVEMENTS; i++) {
        if (!g_rpg.unlocked[i] && rpg_get_achievement_progress(i, snap) >= ACH_LIST[i].target) {
            g_rpg.unlocked[i] = 1; newly++;
            if (out_toast && toast_sz > 0) snprintf(out_toast, (size_t)toast_sz, "解鎖獎盃【%s】+%d XP！", ACH_LIST[i].name, ACH_LIST[i].xp);
        }
    }
    return newly;
}
uint32_t rpg_calc_total_xp(const RpgSnapshot *snap) {
    uint32_t xp = (snap->lifetime_secs / 60u) * 10u + snap->lifetime_pages * 2u + (uint32_t)snap->finished_books * 200u + g_rpg.quests_completed * 220u;
    for (int i = 0; i < NUM_ACHIEVEMENTS; i++) if (g_rpg.unlocked[i]) xp += (uint32_t)ACH_LIST[i].xp;
    return xp;
}
int rpg_calc_level(uint32_t total_xp, int *out_cur, int *out_need) {
    int lv = 1; uint32_t rem = total_xp;
    while (lv < 99) {
        uint32_t need = (uint32_t)(100 + (lv - 1) * 45);
        if (rem < need) { if (out_cur) *out_cur = (int)rem; if (out_need) *out_need = (int)need; return lv; }
        rem -= need; lv++;
    }
    if (out_cur) *out_cur = 100;
    if (out_need) *out_need = 100;
    return 99;
}
const char *rpg_level_title(int lv) {
    if (lv >= 90) return "永恆傳奇大賢者";
    if (lv >= 75) return "星辰圖書館長";
    if (lv >= 60) return "萬卷黃金宗師";
    if (lv >= 48) return "神域博學者";
    if (lv >= 38) return "翡翠大儒";
    if (lv >= 28) return "藏經閣主";
    if (lv >= 20) return "翰林學士";
    if (lv >= 14) return "博覽群書者";
    if (lv >= 9)  return "青衫書生";
    if (lv >= 5)  return "勤勉學徒";
    if (lv >= 2)  return "初階書友";
    return "見習書童";
}
