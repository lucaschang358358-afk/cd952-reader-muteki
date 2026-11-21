#include "reader_core.h"
#include "achievements.h"
#include "book_loader.h"
#include "covers.h"
#include "pdf_engine.h"
#include "stats.h"
#include "themes.h"
#include "txt_tools.h"
#include "ui_gfx.h"
#include "v9_almanac.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SCR_HOME = 0,
    SCR_READ_TEXT,
    SCR_READ_PDF,
    SCR_READ_IMG,
    SCR_READ_MENU,
    SCR_TOC,
    SCR_JUMP,
    SCR_BOOKMARKS,
    SCR_SEARCH,
    SCR_NEW_FOLDER,
    SCR_MOVE_FOLDER,
    SCR_NEXT_VOL,
    SCR_BOOK_STYLE,
    SCR_TXT_EDITOR,
    SCR_DAILY_REPORT,
    SCR_EYE_BREAK,
    SCR_FILTER
} ScreenMode;

typedef enum {
    TAB_SHELF = 0,
    TAB_MANAGE,
    TAB_STATS,
    TAB_ACHIEVE,
    TAB_APPEARANCE
} HomeTab;

#define MAX_BOOKS       32
#define MAX_FOLDERS     16
#define MAX_BOOKMARKS   16
#define MAX_SEARCH_HITS 64
#define COVER_CACHE_N    6
#define ILLUST_CACHE_N   2
#define EDIT_MAX_LINES  14

typedef struct {
    char path[384], title[96], author[64], folder[48];
    BookFormat format;
    size_t byte_offset, total_bytes;
    int page_idx, page_count;
    uint32_t read_secs;
    int cover_style, read_status, rating;
} BookEntry;

typedef struct { size_t pos_val; int page_num, pct; char label[72]; } ManualBookmark;
typedef struct { size_t pos_val; int page_num; char snippet[84]; } SearchHit;
typedef struct { char path[384]; int tried, w, h; uint8_t *rgb; unsigned stamp; } CoverCacheSlot;
typedef struct { int img_id, w, h; uint8_t *rgb; unsigned stamp; } IllustCacheSlot;

static ScreenMode g_screen = SCR_HOME, g_return_screen = SCR_HOME;
static HomeTab    g_home_tab = TAB_SHELF;

static int g_theme_idx = 0, g_font_style = 0, g_font_size_idx = 1, g_spacing_idx = 2;
static int g_margin_idx = 1, g_transition_idx = 1, g_auto_sec_idx = 2, g_daily_goal_idx = 3, g_eye_break_idx = 2;

static const int FONT_PX[4]         = { 16, 18, 22, 26 };
static const int FONT_UNIT_W[4]     = {  8,  9, 10, 11 };
static const char *FONT_SIZE_NM[4]  = { "緊湊16", "標準18", "舒適22", "特大26" };
static const char *FONT_STYLE_NM[4] = { "標準正體", "粗體清晰", "柔雅陰影", "高對比" };
static const int SPACING_PX[5]      = { 3, 6, 10, 15, 22 };
static const char *SPACING_NM[5]    = { "極緊", "緊湊", "標準", "寬鬆", "超寬" };
static const int MARGIN_PX[4]       = { 16, 28, 48, 76 };
static const char *MARGIN_NM[4]     = { "極窄", "標準", "舒適", "專注" };
static const char *TRANS_NM[4]      = { "無動畫", "左右滑動", "紙頁掃過", "柔和淡入" };
static const int AUTO_SECS[7]       = { 5, 10, 15, 20, 30, 45, 60 };
static const int GOAL_MINS[8]       = { 10, 15, 20, 30, 45, 60, 90, 120 };
static const int EYE_MINS[5]        = { 0, 20, 30, 45, 60 };
static const char *EYE_NM[5]        = { "關閉", "每20分", "每30分", "每45分", "每60分" };
static const char *STATUS_NM[4]     = { "想看", "閱讀中", "已完食", "暫擱" };
static const char *PDF_ZOOM_NM[5]   = { "適應整頁", "適應寬度", "135%", "175%", "225%" };

static BookEntry g_books[MAX_BOOKS];
static int  g_book_count = 0, g_folder_count = 0, g_filter_fmt = 0, g_filter_status = 0, g_shelf_sel = 0, g_manage_scroll = 0;
static char g_folders[MAX_FOLDERS][48], g_cur_folder[48] = {0}, g_pending_import_folder[48] = {0};

static int g_stats_subtab = 0, g_ach_subtab = 0, g_ach_cat = 0, g_ach_scroll = 0;
static int g_appear_subtab = 0, g_theme_page = 0, g_set_row = 0;
static int g_stats_year = 2026, g_stats_doy = 0, g_report_y = 2026, g_report_m = 11, g_report_d = 21;
static int g_last_clock_min = -1, g_unsaved_secs = 0, g_goal_notified_day = -1;
static int g_auto_active = 0, g_auto_elapsed = 0, g_eye_cont_secs = 0, g_page_secs_cur = 0, g_avg_sec_per_page = 35;

static CoverCacheSlot g_cov_cache[COVER_CACHE_N]; static unsigned g_cov_clock = 0;
static IllustCacheSlot g_ill_cache[ILLUST_CACHE_N]; static unsigned g_ill_clock = 0;

static char g_filepath[384] = {0};
static BookDoc g_doc;
static PdfDoc *g_pdf = NULL;
static size_t *g_page_offsets = NULL;
static int g_page_count = 0, g_current_page = 0, g_ch_chars[BOOK_MAX_CHAPTERS];

static int g_pdf_page = 0, g_pdf_zoom = 0, g_pdf_pan_x = 0, g_pdf_pan_y = 0, g_pdf_last_pw = 800, g_pdf_last_ph = 400, g_pdf_reflow = 0;
static char *g_pdf_reflow_text = NULL; static size_t g_pdf_reflow_len = 0; static int g_pdf_reflow_page_cached = -1;

static ManualBookmark g_bmarks[MAX_BOOKMARKS]; static int g_bmark_count = 0, g_bmark_sel = 0;
static char g_search_query[64] = {0};
static SearchHit g_search_hits[MAX_SEARCH_HITS]; static int g_search_cnt = 0, g_search_sel = 0, g_search_scroll = 0, g_search_ran = 0;
static int g_read_menu_sel = 0, g_toc_sel = 0, g_toc_scroll = 0, g_jump_target = 0;
static char g_new_folder_buf[48] = {0};
static int g_move_book_idx = -1, g_move_folder_sel = 0, g_next_vol_idx = -1, g_style_book_idx = -1, g_style_row = 0;
static char g_toast_msg[128] = {0}; static int g_toast_ttl = 0;

static size_t g_edit_starts[EDIT_MAX_LINES], g_edit_lens[EDIT_MAX_LINES];
static int g_edit_line_cnt = 0, g_edit_sel = 0, g_edit_editing = 0;
static char g_edit_buf[256] = {0};

static int is_image_fmt(BookFormat fmt) { return (fmt == BOOK_FMT_BMP || fmt == BOOK_FMT_PNG); }
static int current_goal_mins(void) { return GOAL_MINS[g_daily_goal_idx]; }
static ScreenMode active_read_screen(void) {
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) return SCR_READ_PDF;
    if (is_image_fmt(g_doc.format)) return SCR_READ_IMG;
    return SCR_READ_TEXT;
}
static void show_toast(const char *msg) { snprintf(g_toast_msg, sizeof(g_toast_msg), "%s", msg ? msg : ""); g_toast_ttl = 5; }

static int utf8_char_info(const unsigned char *s, size_t rem, int *out_u) {
    if (rem == 0) { *out_u = 0; return 0; }
    unsigned char c = s[0];
    if ((c & 0x80) == 0) { *out_u = 1; return 1; }
    if ((c & 0xE0) == 0xC0 && rem >= 2) { *out_u = 2; return 2; }
    if ((c & 0xF0) == 0xE0 && rem >= 3) { *out_u = 2; return 3; }
    if ((c & 0xF8) == 0xF0 && rem >= 4) { *out_u = 2; return 4; }
    *out_u = 1; return 1;
}
static void utf8_ellipsis(const char *src, int max_u, char *dst, size_t dst_sz) {
    if (!src || dst_sz == 0) { if (dst_sz) dst[0] = '\0'; return; }
    size_t slen = strlen(src), pos = 0, out = 0; int u_tot = 0;
    while (pos < slen) {
        int u = 0, clen = utf8_char_info((const unsigned char *)(src + pos), slen - pos, &u);
        if (u_tot + u > max_u) { if (out + 3 < dst_sz) { dst[out++] = '.'; dst[out++] = '.'; } break; }
        if (out + (size_t)clen >= dst_sz) break;
        memcpy(dst + out, src + pos, (size_t)clen); out += (size_t)clen; pos += (size_t)clen; u_tot += u;
    }
    dst[out] = '\0';
}
static const char *get_filename_only(const char *path) {
    const char *p1 = strrchr(path, '\\'), *p2 = strrchr(path, '/'), *p = (p1 > p2) ? p1 : p2;
    return p ? (p + 1) : path;
}
static void draw_str(int x, int y, const char *s, int max_w, uint32_t rgb) {
    if (s && s[0]) hal_draw_line_utf8(x, y, s, (int)strlen(s), max_w, rgb);
}
static void sanitize_field(char *dst, size_t sz, const char *src) {
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 1 < sz; i++) {
        char c = src[i]; if (c == '|' || c == '\r' || c == '\n' || c == '\x01') c = ' '; dst[o++] = c;
    }
    dst[o] = '\0';
}
static void format_duration_zh(uint32_t secs, char *out, size_t sz) {
    if (secs < 60u) snprintf(out, sz, "%lu秒", (unsigned long)secs);
    else if (secs < 3600u) snprintf(out, sz, "%lu分", (unsigned long)(secs / 60u));
    else {
        unsigned long h = (unsigned long)(secs / 3600u), m = (unsigned long)((secs % 3600u) / 60u);
        if (m > 0) snprintf(out, sz, "%lu時%lu分", h, m); else snprintf(out, sz, "%lu小時", h);
    }
}

static void clear_illust_cache(void) {
    for (int i = 0; i < ILLUST_CACHE_N; i++) { free(g_ill_cache[i].rgb); g_ill_cache[i].rgb = NULL; g_ill_cache[i].img_id = -1; }
}
static void clear_cover_cache(void) {
    for (int i = 0; i < COVER_CACHE_N; i++) { free(g_cov_cache[i].rgb); memset(&g_cov_cache[i], 0, sizeof(g_cov_cache[i])); }
}
static void invalidate_cover_cache_for(const char *path) {
    for (int i = 0; i < COVER_CACHE_N; i++)
        if (strcmp(g_cov_cache[i].path, path) == 0) { free(g_cov_cache[i].rgb); memset(&g_cov_cache[i], 0, sizeof(g_cov_cache[i])); }
}
static const uint8_t *get_cached_cover(const char *path, int *out_w, int *out_h) {
    *out_w = 0; *out_h = 0; if (!path || !path[0]) return NULL;
    for (int i = 0; i < COVER_CACHE_N; i++) {
        if (g_cov_cache[i].tried && strcmp(g_cov_cache[i].path, path) == 0) {
            g_cov_cache[i].stamp = ++g_cov_clock; *out_w = g_cov_cache[i].w; *out_h = g_cov_cache[i].h; return g_cov_cache[i].rgb;
        }
    }
    int slot = 0;
    for (int i = 0; i < COVER_CACHE_N; i++) { if (!g_cov_cache[i].tried) { slot = i; break; } if (g_cov_cache[i].stamp < g_cov_cache[slot].stamp) slot = i; }
    free(g_cov_cache[slot].rgb); memset(&g_cov_cache[slot], 0, sizeof(g_cov_cache[slot]));
    snprintf(g_cov_cache[slot].path, sizeof(g_cov_cache[slot].path), "%s", path);
    g_cov_cache[slot].tried = 1; g_cov_cache[slot].stamp = ++g_cov_clock;
    g_cov_cache[slot].rgb = book_load_cover_thumb(path, &g_cov_cache[slot].w, &g_cov_cache[slot].h);
    *out_w = g_cov_cache[slot].w; *out_h = g_cov_cache[slot].h; return g_cov_cache[slot].rgb;
}
static const uint8_t *get_cached_illust(int img_id, int *out_w, int *out_h) {
    *out_w = 0; *out_h = 0; if (img_id < 0 || img_id >= g_doc.image_count) return NULL;
    for (int i = 0; i < ILLUST_CACHE_N; i++) {
        if (g_ill_cache[i].rgb && g_ill_cache[i].img_id == img_id) {
            g_ill_cache[i].stamp = ++g_ill_clock; *out_w = g_ill_cache[i].w; *out_h = g_ill_cache[i].h; return g_ill_cache[i].rgb;
        }
    }
    int slot = 0;
    for (int i = 0; i < ILLUST_CACHE_N; i++) { if (!g_ill_cache[i].rgb) { slot = i; break; } if (g_ill_cache[i].stamp < g_ill_cache[slot].stamp) slot = i; }
    free(g_ill_cache[slot].rgb);
    g_ill_cache[slot].rgb = book_extract_epub_image(g_filepath, g_doc.images[img_id].zip_path, &g_ill_cache[slot].w, &g_ill_cache[slot].h);
    g_ill_cache[slot].img_id = g_ill_cache[slot].rgb ? img_id : -1; g_ill_cache[slot].stamp = ++g_ill_clock;
    *out_w = g_ill_cache[slot].w; *out_h = g_ill_cache[slot].h; return g_ill_cache[slot].rgb;
}

static void build_rpg_snapshot(RpgSnapshot *s) {
    memset(s, 0, sizeof(*s)); int hh = 12, mm = 0, ss = 0;
    hal_get_local_time(&s->cur_year, &s->cur_month, &s->cur_day, &hh, &mm, &ss);
    s->goal_mins = current_goal_mins(); s->lifetime_secs = stats_lifetime_secs();
    s->lifetime_pages = stats_lifetime_pages(); s->lifetime_days = stats_lifetime_active_days();
    s->streak = stats_calc_streak(s->cur_year, s->cur_month, s->cur_day);
    s->goal_days = stats_count_goal_days(s->goal_mins);
    s->book_count = g_book_count; s->folder_count = g_folder_count;
    for (int i = 0; i < g_book_count; i++) {
        if (g_books[i].read_status == 2) s->finished_books++;
        if (g_books[i].rating > 0) s->rated_books++;
        if (g_books[i].format == BOOK_FMT_EPUB) s->epub_count++;
        else if (g_books[i].format == BOOK_FMT_PDF) s->pdf_count++;
        else if (g_books[i].format == BOOK_FMT_TXT) s->txt_count++;
        else if (is_image_fmt(g_books[i].format)) s->img_count++;
    }
    stats_get_week_totals(s->cur_year, s->cur_month, s->cur_day, s->goal_mins,
                          &s->week_secs, &s->week_pages, &s->week_active_days, &s->week_goal_days);
}
static void eval_rpg_and_toast(void) {
    RpgSnapshot snap; build_rpg_snapshot(&snap); char tmsg[128] = {0};
    if (rpg_evaluate(&snap, tmsg, sizeof(tmsg)) > 0) { if (tmsg[0]) show_toast(tmsg); rpg_save(hal_get_rpg_path()); }
}

static void save_library(void) {
    const char *lp = hal_get_library_path(); if (!lp || !lp[0]) return;
    FILE *f = fopen(lp, "wb"); if (!f) return;
    fprintf(f, "#CFG8|%d|%d|%d|%d|%d|%d|%d|%d|%d\n",
            g_theme_idx, g_font_style, g_font_size_idx, g_spacing_idx,
            g_margin_idx, g_transition_idx, g_auto_sec_idx, g_daily_goal_idx, g_eye_break_idx);
    for (int i = 0; i < g_folder_count; i++) { char fn[48]; sanitize_field(fn, sizeof(fn), g_folders[i]); if (fn[0]) fprintf(f, "#FLD|%s\n", fn); }
    for (int i = 0; i < g_book_count; i++) {
        BookEntry *b = &g_books[i]; char t[96], a[64], fld[48];
        sanitize_field(t, sizeof(t), b->title); sanitize_field(a, sizeof(a), b->author); sanitize_field(fld, sizeof(fld), b->folder);
        fprintf(f, "%d|%zu|%zu|%d|%d|%lu|%d|%d|%d|%s|%s|%s|%s\n",
                (int)b->format, b->byte_offset, b->total_bytes, b->page_idx, b->page_count,
                (unsigned long)b->read_secs, b->cover_style, b->read_status, b->rating, t, a, fld, b->path);
    }
    fclose(f);
}

static void load_library(void) {
    g_book_count = 0; g_folder_count = 0;
    const char *lp = hal_get_library_path(); if (!lp || !lp[0]) return;
    FILE *f = fopen(lp, "rb"); if (!f) return;
    char line[800];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        if (strncmp(line, "#CFG8|", 6) == 0) {
            int c[9] = {0};
            if (sscanf(line + 6, "%d|%d|%d|%d|%d|%d|%d|%d|%d", &c[0],&c[1],&c[2],&c[3],&c[4],&c[5],&c[6],&c[7],&c[8]) == 9) {
                if (c[0] >= 0 && c[0] < NUM_THEMES) g_theme_idx = c[0];
                if (c[1] >= 0 && c[1] < 4) g_font_style = c[1];
                if (c[2] >= 0 && c[2] < 4) g_font_size_idx = c[2];
                if (c[3] >= 0 && c[3] < 5) g_spacing_idx = c[3];
                if (c[4] >= 0 && c[4] < 4) g_margin_idx = c[4];
                if (c[5] >= 0 && c[5] < 4) g_transition_idx = c[5];
                if (c[6] >= 0 && c[6] < 7) g_auto_sec_idx = c[6];
                if (c[7] >= 0 && c[7] < 8) g_daily_goal_idx = c[7];
                if (c[8] >= 0 && c[8] < 5) g_eye_break_idx = c[8];
            }
            continue;
        }
        if (strncmp(line, "#CFG", 4) == 0) {
            const char *bar = strchr(line, '|'); int th = 0, sp = 2;
            if (bar && sscanf(bar + 1, "%d|%d", &th, &sp) == 2) {
                if (th >= 0 && th < NUM_THEMES) g_theme_idx = th;
                if (sp >= 0 && sp < 5) g_spacing_idx = sp;
            }
            continue;
        }
        if (strncmp(line, "#FLD|", 5) == 0) {
            if (g_folder_count < MAX_FOLDERS && line[5]) utf8_ellipsis(line + 5, 22, g_folders[g_folder_count++], 48);
            continue;
        }
        if (line[0] == '#' || g_book_count >= MAX_BOOKS) continue;
        char *ps[12], *cur = line; int pcnt = 0;
        while (pcnt < 12) { char *bar = strchr(cur, '|'); if (!bar) break; ps[pcnt++] = bar; cur = bar + 1; }
        if (pcnt < 7) continue;
        BookEntry *be = &g_books[g_book_count++]; memset(be, 0, sizeof(*be));
        be->format = (BookFormat)atoi(line);
        be->byte_offset = (size_t)strtoull(ps[0] + 1, NULL, 10);
        be->total_bytes = (size_t)strtoull(ps[1] + 1, NULL, 10);
        be->page_idx = atoi(ps[2] + 1); be->page_count = atoi(ps[3] + 1);
        char *ts, *te, *as, *ae, *fs = NULL, *fe = NULL, *path_s;
        if (pcnt >= 12) {
            be->read_secs = (uint32_t)strtoul(ps[4] + 1, NULL, 10);
            be->cover_style = atoi(ps[5] + 1); be->read_status = atoi(ps[6] + 1); be->rating = atoi(ps[7] + 1);
            ts = ps[8] + 1; te = ps[9]; as = ps[9] + 1; ae = ps[10]; fs = ps[10] + 1; fe = ps[11]; path_s = ps[11] + 1;
        } else if (pcnt >= 9) {
            be->read_secs = (uint32_t)strtoul(ps[4] + 1, NULL, 10);
            ts = ps[5] + 1; te = ps[6]; as = ps[6] + 1; ae = ps[7]; fs = ps[7] + 1; fe = ps[8]; path_s = ps[8] + 1;
        } else {
            ts = ps[4] + 1; te = ps[5]; as = ps[5] + 1; ae = ps[6]; path_s = ps[6] + 1;
        }
        size_t tl = (size_t)(te - ts); if (tl >= sizeof(be->title)) tl = sizeof(be->title) - 1; memcpy(be->title, ts, tl); be->title[tl] = '\0';
        size_t al = (size_t)(ae - as); if (al >= sizeof(be->author)) al = sizeof(be->author) - 1; memcpy(be->author, as, al); be->author[al] = '\0';
        if (fs && fe && fe >= fs) { size_t fl = (size_t)(fe - fs); if (fl >= sizeof(be->folder)) fl = sizeof(be->folder) - 1; memcpy(be->folder, fs, fl); be->folder[fl] = '\0'; }
        snprintf(be->path, sizeof(be->path), "%s", path_s);
        if (!be->title[0]) utf8_ellipsis(get_filename_only(be->path), 40, be->title, sizeof(be->title));
    }
    fclose(f);
}

static void update_book_in_library(const char *path, BookFormat fmt, const char *title, const char *author, size_t boff, size_t btot, int pidx, int pcnt) {
    if (!path || !path[0]) return;
    int found = -1, prev_style = 0, prev_status = 1, prev_rating = 0; uint32_t prev_secs = 0; char prev_fld[48] = {0};
    for (int i = 0; i < g_book_count; i++) {
        if (strcmp(g_books[i].path, path) == 0) {
            found = i; prev_secs = g_books[i].read_secs; prev_style = g_books[i].cover_style;
            prev_status = g_books[i].read_status; prev_rating = g_books[i].rating;
            snprintf(prev_fld, sizeof(prev_fld), "%s", g_books[i].folder); break;
        }
    }
    int st = (prev_status == 0) ? 1 : prev_status;
    if (pcnt > 1 && pidx + 1 >= pcnt) st = 2;
    BookEntry cur; memset(&cur, 0, sizeof(cur));
    snprintf(cur.path, sizeof(cur.path), "%s", path);
    utf8_ellipsis((title && title[0]) ? title : get_filename_only(path), 44, cur.title, sizeof(cur.title));
    sanitize_field(cur.author, sizeof(cur.author), author ? author : "");
    cur.format = fmt; cur.byte_offset = boff; cur.total_bytes = btot; cur.page_idx = pidx; cur.page_count = pcnt;
    cur.read_secs = prev_secs; cur.cover_style = prev_style; cur.read_status = st; cur.rating = prev_rating;
    if (prev_fld[0]) snprintf(cur.folder, sizeof(cur.folder), "%s", prev_fld);
    else if (g_pending_import_folder[0]) snprintf(cur.folder, sizeof(cur.folder), "%s", g_pending_import_folder);
    if (found >= 0) { for (int i = found; i > 0; i--) g_books[i] = g_books[i - 1]; g_books[0] = cur; }
    else {
        int mv = (g_book_count < MAX_BOOKS) ? g_book_count : (MAX_BOOKS - 1);
        for (int i = mv; i > 0; i--) g_books[i] = g_books[i - 1];
        g_books[0] = cur; if (g_book_count < MAX_BOOKS) g_book_count++;
    }
    g_shelf_sel = 0; g_manage_scroll = 0; save_library();
}

static void remove_book_at(int idx) {
    if (g_book_count <= 0 || idx < 0 || idx >= g_book_count) return;
    invalidate_cover_cache_for(g_books[idx].path);
    for (int i = idx; i + 1 < g_book_count; i++) g_books[i] = g_books[i + 1];
    g_book_count--;
    if (g_shelf_sel >= g_book_count && g_book_count > 0) g_shelf_sel = g_book_count - 1;
    if (g_book_count == 0) { g_shelf_sel = 0; g_manage_scroll = 0; }
    save_library();
}

static int folder_find(const char *name) { for (int i = 0; i < g_folder_count; i++) if (strcmp(g_folders[i], name) == 0) return i; return -1; }
static int folder_create(const char *name) {
    if (!name || !name[0] || g_folder_count >= MAX_FOLDERS) return 0;
    char cl[48]; sanitize_field(cl, sizeof(cl), name);
    if (!cl[0] || folder_find(cl) >= 0) return 0;
    snprintf(g_folders[g_folder_count++], 48, "%s", cl); save_library(); eval_rpg_and_toast(); return 1;
}
static void folder_delete(int fi) {
    if (fi < 0 || fi >= g_folder_count) return;
    char tgt[48]; snprintf(tgt, sizeof(tgt), "%s", g_folders[fi]);
    for (int i = 0; i < g_book_count; i++) if (strcmp(g_books[i].folder, tgt) == 0) g_books[i].folder[0] = '\0';
    for (int i = fi; i + 1 < g_folder_count; i++) snprintf(g_folders[i], 48, "%s", g_folders[i + 1]);
    g_folder_count--; if (strcmp(g_cur_folder, tgt) == 0) g_cur_folder[0] = '\0'; save_library();
}
static int folder_count_books(const char *name) { int c = 0; for (int i = 0; i < g_book_count; i++) if (strcmp(g_books[i].folder, name) == 0) c++; return c; }
static int folder_first_book_idx(const char *name) { for (int i = 0; i < g_book_count; i++) if (strcmp(g_books[i].folder, name) == 0) return i; return -1; }
static int book_matches_filters(const BookEntry *b) {
    if (g_filter_fmt == 1 && b->format != BOOK_FMT_EPUB) return 0;
    if (g_filter_fmt == 2 && b->format != BOOK_FMT_PDF)  return 0;
    if (g_filter_fmt == 3 && b->format != BOOK_FMT_TXT)  return 0;
    if (g_filter_fmt == 4 && !is_image_fmt(b->format))   return 0;
    if (g_filter_status > 0 && b->read_status != (g_filter_status - 1)) return 0;
    return 1;
}
static int shelf_build_grid(int *types, int *idxs, int max_n) {
    int cnt = 0, filtering = (g_filter_fmt != 0 || g_filter_status != 0);
    if (g_cur_folder[0] == '\0') {
        if (!filtering) for (int f = 0; f < g_folder_count && cnt < max_n; f++) { types[cnt] = 1; idxs[cnt] = f; cnt++; }
        for (int b = 0; b < g_book_count && cnt < max_n; b++) {
            if (!book_matches_filters(&g_books[b])) continue;
            if (!filtering && g_books[b].folder[0] != '\0') continue;
            types[cnt] = 0; idxs[cnt] = b; cnt++;
        }
    } else {
        for (int b = 0; b < g_book_count && cnt < max_n; b++)
            if (strcmp(g_books[b].folder, g_cur_folder) == 0 && book_matches_filters(&g_books[b])) { types[cnt] = 0; idxs[cnt] = b; cnt++; }
        for (int i = 0; i < cnt; i++) for (int j = i + 1; j < cnt; j++)
            if (strcmp(g_books[idxs[i]].title, g_books[idxs[j]].title) > 0) { int t = idxs[i]; idxs[i] = idxs[j]; idxs[j] = t; }
    }
    return cnt;
}
static int find_next_volume_in_folder(const char *cur_path) {
    int ci = -1; for (int i = 0; i < g_book_count; i++) if (strcmp(g_books[i].path, cur_path) == 0) { ci = i; break; }
    if (ci < 0 || !g_books[ci].folder[0]) return -1;
    int list[MAX_BOOKS], n = 0;
    for (int i = 0; i < g_book_count; i++) if (strcmp(g_books[i].folder, g_books[ci].folder) == 0) list[n++] = i;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (strcmp(g_books[list[i]].title, g_books[list[j]].title) > 0) { int t = list[i]; list[i] = list[j]; list[j] = t; }
    for (int i = 0; i + 1 < n; i++) if (list[i] == ci) return list[i + 1];
    return -1;
}

static void save_manual_bookmarks(void) {
    if (!g_filepath[0]) return;
    char p[420]; snprintf(p, sizeof(p), "%s.bmk2", g_filepath);
    FILE *f = fopen(p, "wb"); if (!f) return;
    for (int i = 0; i < g_bmark_count; i++) {
        char lbl[72]; sanitize_field(lbl, sizeof(lbl), g_bmarks[i].label);
        fprintf(f, "%zu|%d|%d|%s\n", g_bmarks[i].pos_val, g_bmarks[i].page_num, g_bmarks[i].pct, lbl);
    }
    fclose(f);
}
static void load_manual_bookmarks(const char *path) {
    g_bmark_count = 0; g_bmark_sel = 0; if (!path || !path[0]) return;
    char p[420]; snprintf(p, sizeof(p), "%s.bmk2", path);
    FILE *f = fopen(p, "rb"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_bmark_count < MAX_BOOKMARKS) {
        size_t len = strlen(line); while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        char *p1 = strchr(line, '|'); if (!p1) continue;
        char *p2 = strchr(p1 + 1, '|'); if (!p2) continue;
        char *p3 = strchr(p2 + 1, '|'); if (!p3) continue;
        ManualBookmark *bm = &g_bmarks[g_bmark_count++];
        bm->pos_val = (size_t)strtoull(line, NULL, 10); bm->page_num = atoi(p1 + 1); bm->pct = atoi(p2 + 1);
        snprintf(bm->label, sizeof(bm->label), "%s", p3 + 1);
    }
    fclose(f);
}
static void add_current_manual_bookmark(void) {
    if (g_bmark_count >= MAX_BOOKMARKS) return;
    ManualBookmark *bm = &g_bmarks[g_bmark_count]; memset(bm, 0, sizeof(*bm));
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf); bm->pos_val = (size_t)g_pdf_page; bm->page_num = g_pdf_page + 1;
        bm->pct = tot > 0 ? ((g_pdf_page + 1) * 100) / tot : 0;
        size_t tl = 0; char *pt = pdf_extract_page_text(g_pdf, g_pdf_page, &tl);
        if (pt && tl > 2) { char cl[72]; sanitize_field(cl, sizeof(cl), pt); utf8_ellipsis(cl, 28, bm->label, sizeof(bm->label)); }
        else snprintf(bm->label, sizeof(bm->label), "PDF 第 %d 頁", g_pdf_page + 1);
        free(pt);
    } else if (g_doc.text && g_page_offsets && g_page_count > 0) {
        size_t off = g_page_offsets[g_current_page]; bm->pos_val = off; bm->page_num = g_current_page + 1;
        bm->pct = ((g_current_page + 1) * 100) / g_page_count;
        if (g_doc.text[off] == '\x01') snprintf(bm->label, sizeof(bm->label), "【插圖頁 #%d】", g_current_page + 1);
        else {
            char raw[80], cl[80]; size_t rem = g_doc.text_len - off; if (rem > 70) rem = 70;
            memcpy(raw, g_doc.text + off, rem); raw[rem] = '\0'; sanitize_field(cl, sizeof(cl), raw);
            utf8_ellipsis(cl, 28, bm->label, sizeof(bm->label));
        }
    } else return;
    g_bmark_count++; g_bmark_sel = g_bmark_count - 1;
    rpg_profile()->total_bookmarks_added++; save_manual_bookmarks(); eval_rpg_and_toast();
}

static void save_bookmark(void) {
    if (!g_filepath[0]) return;
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf); char p[420]; snprintf(p, sizeof(p), "%s.bmk", g_filepath);
        FILE *f = fopen(p, "wb"); if (f) { fprintf(f, "%d", g_pdf_page); fclose(f); }
        update_book_in_library(g_filepath, BOOK_FMT_PDF, g_doc.title, g_doc.author, (size_t)g_pdf_page, (size_t)tot, g_pdf_page, tot);
    } else if (is_image_fmt(g_doc.format)) {
        update_book_in_library(g_filepath, g_doc.format, g_doc.title, "", 0, 0, 0, 0);
    } else if (g_page_count > 0 && g_page_offsets) {
        size_t off = g_page_offsets[g_current_page]; char p[420]; snprintf(p, sizeof(p), "%s.bmk", g_filepath);
        FILE *f = fopen(p, "wb"); if (f) { fprintf(f, "%zu", off); fclose(f); }
        update_book_in_library(g_filepath, g_doc.format, g_doc.title, g_doc.author, off, g_doc.text_len, g_current_page, g_page_count);
    }
    stats_save(hal_get_stats_path());
}
static size_t load_bookmark(const char *path) {
    if (!path || !path[0]) return 0;
    char p[420]; snprintf(p, sizeof(p), "%s.bmk", path);
    FILE *f = fopen(p, "rb");
    if (f) { size_t off = 0; if (fscanf(f, "%zu", &off) == 1) { fclose(f); return off; } fclose(f); }
    for (int i = 0; i < g_book_count; i++) if (strcmp(g_books[i].path, path) == 0) return g_books[i].byte_offset;
    return 0;
}

static void record_page_turn_stat(int dir) {
    int y, m, d, hh, mm, ss; hal_get_local_time(&y, &m, &d, &hh, &mm, &ss);
    stats_add_reading(y, m, d, 0, 1);
    if (hh >= 5 && hh < 8) rpg_profile()->early_bird_reads++;
    if (hh >= 23 || hh < 5) rpg_profile()->night_owl_reads++;
    int dow = stats_day_of_week(y, m, d); if (dow == 0 || dow == 6) rpg_profile()->weekend_reads++;
    if (g_page_secs_cur >= 3 && g_page_secs_cur <= 180) {
        g_avg_sec_per_page = (g_avg_sec_per_page * 3 + g_page_secs_cur) / 4;
        if (g_avg_sec_per_page < 5) g_avg_sec_per_page = 5;
    }
    g_page_secs_cur = 0; g_auto_elapsed = 0;
    if (dir != 0 && g_transition_idx > 0) hal_queue_transition(g_transition_idx, dir);
    eval_rpg_and_toast();
}

static void calc_remaining_secs(uint32_t *out_book_secs, uint32_t *out_chap_secs) {
    *out_book_secs = 0; *out_chap_secs = 0;
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        int left = pdf_page_count(g_pdf) - (g_pdf_page + 1); if (left < 0) left = 0;
        *out_book_secs = *out_chap_secs = (uint32_t)left * (uint32_t)g_avg_sec_per_page; return;
    }
    if (g_page_count > 0 && g_page_offsets) {
        int left = g_page_count - (g_current_page + 1); if (left < 0) left = 0;
        *out_book_secs = (uint32_t)left * (uint32_t)g_avg_sec_per_page;
        int chap_end = g_page_count - 1; size_t co = g_page_offsets[g_current_page];
        for (int i = 0; i < g_doc.chapter_count; i++) {
            if (g_doc.chapters[i].offset > co) {
                for (int pg = g_current_page; pg < g_page_count; pg++)
                    if (g_page_offsets[pg] >= g_doc.chapters[i].offset) { chap_end = pg; break; }
                break;
            }
        }
        int cleft = chap_end - g_current_page; if (cleft < 0) cleft = 0;
        *out_chap_secs = (uint32_t)cleft * (uint32_t)g_avg_sec_per_page;
    }
}

static int parse_img_marker(const char *buf, size_t len, size_t pos, int *out_id, size_t *out_step) {
    if (pos + 6 > len || buf[pos] != '\x01' || memcmp(buf + pos + 1, "IMG:", 4) != 0) return 0;
    int id = atoi(buf + pos + 5); size_t p = pos + 5;
    while (p < len && buf[p] != '\n' && buf[p] != '\r') p++;
    if (p < len && buf[p] == '\r') p++;
    if (p < len && buf[p] == '\n') p++;
    if (out_id) *out_id = id;
    if (out_step) *out_step = p - pos;
    return 1;
}
static size_t scan_one_line_buf(const char *buf, size_t len, size_t start_off, int max_units, size_t *out_draw_len) {
    size_t pos = start_off; int units = 0;
    while (pos < len) {
        char ch = buf[pos];
        if (ch == '\r') { *out_draw_len = pos - start_off; pos++; if (pos < len && buf[pos] == '\n') pos++; return pos - start_off; }
        if (ch == '\n') { *out_draw_len = pos - start_off; return (pos - start_off) + 1; }
        int u = 0, clen = utf8_char_info((const unsigned char *)(buf + pos), len - pos, &u);
        if (units + u > max_units && pos > start_off) { *out_draw_len = pos - start_off; return pos - start_off; }
        units += u; pos += (size_t)clen;
    }
    *out_draw_len = pos - start_off; return pos - start_off;
}
static void get_layout_params(int *out_max_units, int *out_lines, int *out_line_h, int *out_pad_x) {
    int w = hal_get_width(), h = hal_get_height(), pad_x = MARGIN_PX[g_margin_idx];
    int line_h = FONT_PX[g_font_size_idx] + SPACING_PX[g_spacing_idx];
    int uw = w - pad_x * 2; if (uw < 160) uw = 160;
    int max_u = uw / FONT_UNIT_W[g_font_size_idx]; if (max_u < 12) max_u = 12;
    int lines = (h - 36 - 38 - 18) / line_h; if (lines < 1) lines = 1;
    *out_max_units = max_u; *out_lines = lines; *out_line_h = line_h; if (out_pad_x) *out_pad_x = pad_x;
}
static void jump_to_offset(size_t target_offset) {
    g_current_page = 0;
    for (int i = 0; i < g_page_count; i++) { if (g_page_offsets[i] <= target_offset) g_current_page = i; else break; }
}
static int find_page_for_offset(size_t off) {
    int pg = 0; for (int i = 0; i < g_page_count; i++) { if (g_page_offsets[i] <= off) pg = i; else break; } return pg;
}
static void compute_chapter_word_counts(void) {
    memset(g_ch_chars, 0, sizeof(g_ch_chars));
    if (!g_doc.text || g_doc.chapter_count <= 0) return;
    for (int i = 0; i < g_doc.chapter_count; i++) {
        size_t s = g_doc.chapters[i].offset, e = (i + 1 < g_doc.chapter_count) ? g_doc.chapters[i + 1].offset : g_doc.text_len;
        if (e > s && e <= g_doc.text_len) g_ch_chars[i] = txt_count_utf8_chars(g_doc.text + s, e - s);
    }
}
static void repaginate(size_t target_offset) {
    if (!g_doc.text) return;
    int max_u, lines, lh, px; get_layout_params(&max_u, &lines, &lh, &px);
    free(g_page_offsets); g_page_offsets = NULL; g_page_count = 0;
    int cap = 128; g_page_offsets = (size_t *)malloc((size_t)cap * sizeof(size_t)); if (!g_page_offsets) return;
    size_t pos = 0;
    while (pos < g_doc.text_len || g_page_count == 0) {
        if (g_page_count >= cap) {
            cap *= 2; size_t *tmp = (size_t *)realloc(g_page_offsets, (size_t)cap * sizeof(size_t));
            if (!tmp) break;
            g_page_offsets = tmp;
        }
        g_page_offsets[g_page_count++] = pos;
        size_t img_step = 0;
        if (parse_img_marker(g_doc.text, g_doc.text_len, pos, NULL, &img_step)) { pos += img_step; if (pos >= g_doc.text_len) break; continue; }
        for (int ln = 0; ln < lines && pos < g_doc.text_len; ln++) {
            if (parse_img_marker(g_doc.text, g_doc.text_len, pos, NULL, NULL)) break;
            size_t dl = 0, step = scan_one_line_buf(g_doc.text, g_doc.text_len, pos, max_u, &dl);
            if (step == 0) break;
            pos += step;
        }
        if (pos >= g_doc.text_len) break;
    }
    jump_to_offset(target_offset); compute_chapter_word_counts();
}

static void open_txt_editor(void) {
    if (g_doc.format != BOOK_FMT_TXT || !g_doc.text || !g_page_offsets || g_page_count <= 0) {
        show_toast("修字器僅支援 .txt 純文字電子書"); return;
    }
    g_auto_active = 0; g_edit_line_cnt = 0; g_edit_sel = 0; g_edit_editing = 0;
    size_t pos = g_page_offsets[g_current_page]; int max_u, lines, lh, px;
    get_layout_params(&max_u, &lines, &lh, &px); if (lines > EDIT_MAX_LINES) lines = EDIT_MAX_LINES;
    for (int i = 0; i < lines && pos < g_doc.text_len; i++) {
        size_t dl = 0, step = scan_one_line_buf(g_doc.text, g_doc.text_len, pos, max_u, &dl);
        g_edit_starts[g_edit_line_cnt] = pos; g_edit_lens[g_edit_line_cnt] = dl; g_edit_line_cnt++;
        if (step == 0) break;
            pos += step;
    }
    g_screen = SCR_TXT_EDITOR;
}
static void start_editing_selected_txt_line(void) {
    if (g_edit_sel < 0 || g_edit_sel >= g_edit_line_cnt) return;
    size_t len = g_edit_lens[g_edit_sel]; if (len >= sizeof(g_edit_buf)) len = sizeof(g_edit_buf) - 1;
    memcpy(g_edit_buf, g_doc.text + g_edit_starts[g_edit_sel], len); g_edit_buf[len] = '\0'; g_edit_editing = 1;
}
static void commit_txt_line_edit(const char *replacement) {
    if (g_edit_sel < 0 || g_edit_sel >= g_edit_line_cnt) return;
    size_t keep_off = g_page_offsets[g_current_page];
    if (txt_replace_line_and_save(g_filepath, &g_doc.text, &g_doc.text_len, g_edit_starts[g_edit_sel], g_edit_lens[g_edit_sel], replacement)) {
        rpg_profile()->total_txt_edits++; repaginate(keep_off); show_toast("已成功修字並存回 TXT！"); eval_rpg_and_toast();
    } else show_toast("儲存 TXT 失敗");
    g_edit_editing = 0; g_screen = SCR_READ_TEXT;
}

static const char *find_ci_str(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle); if (nlen == 0 || nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0; while (j < nlen && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) j++;
        if (j == nlen) return hay + i;
    }
    return NULL;
}
static void run_fulltext_search(void) {
    g_search_cnt = 0; g_search_sel = 0; g_search_scroll = 0; g_search_ran = 1;
    if (!g_search_query[0]) return;
    rpg_profile()->total_searches++; eval_rpg_and_toast();
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf);
        for (int p = 0; p < tot && g_search_cnt < MAX_SEARCH_HITS; p++) {
            size_t tl = 0; char *pt = pdf_extract_page_text(g_pdf, p, &tl);
            if (pt && tl > 0) {
                const char *hit = find_ci_str(pt, tl, g_search_query);
                if (hit) {
                    SearchHit *sh = &g_search_hits[g_search_cnt++]; sh->pos_val = (size_t)p; sh->page_num = p + 1;
                    size_t off = (size_t)(hit - pt), cs = (off > 12) ? (off - 12) : 0;
                    while (cs < off && ((unsigned char)pt[cs] & 0xC0) == 0x80) cs++;
                    char raw[96], cl[96]; size_t rl = tl - cs; if (rl > 78) rl = 78;
                    memcpy(raw, pt + cs, rl); raw[rl] = '\0'; sanitize_field(cl, sizeof(cl), raw);
                    utf8_ellipsis(cl, 36, sh->snippet, sizeof(sh->snippet));
                }
            }
            free(pt);
        }
        return;
    }
    if (g_doc.text && g_doc.text_len > 0) {
        size_t pos = 0, qlen = strlen(g_search_query);
        while (pos < g_doc.text_len && g_search_cnt < MAX_SEARCH_HITS) {
            const char *hit = find_ci_str(g_doc.text + pos, g_doc.text_len - pos, g_search_query);
            if (!hit) break;
            size_t off = (size_t)(hit - g_doc.text);
            SearchHit *sh = &g_search_hits[g_search_cnt++]; sh->pos_val = off; sh->page_num = find_page_for_offset(off) + 1;
            size_t cs = (off > 12) ? (off - 12) : 0;
            while (cs < off && ((unsigned char)g_doc.text[cs] & 0xC0) == 0x80) cs++;
            char raw[96], cl[96]; size_t rl = g_doc.text_len - cs; if (rl > 78) rl = 78;
            memcpy(raw, g_doc.text + cs, rl); raw[rl] = '\0'; sanitize_field(cl, sizeof(cl), raw);
            utf8_ellipsis(cl, 36, sh->snippet, sizeof(sh->snippet));
            pos = off + qlen;
        }
    }
}

static void show_loading_card(const char *filepath) {
    const AppTheme *th = theme_get(g_theme_idx); int w = hal_get_width(), h = hal_get_height();
    BookFormat fmt = book_detect_format(filepath);
    int cw = 390, ch = 126, cx = (w - cw) / 2, cy = (h - ch) / 2;
    hal_clear_text_in_rect(cx - 8, cy - 6, cw + 16, ch + 12);
    ui_card(cx, cy, cw, ch, 12, th->card_bg, th->card_border, th->shadow, 1, ui_format_color(fmt));
    ui_draw_badge(cx + 20, cy + 20, book_format_tag(fmt), ui_format_color(fmt), 0xFFFFFF);
    hal_set_text_bg(th->card_bg);
    char msg[128], fn[80]; utf8_ellipsis(get_filename_only(filepath), 28, fn, sizeof(fn));
    snprintf(msg, sizeof(msg), "正在開啟 %s 引擎...", book_format_tag(fmt));
    draw_str(cx + 76, cy + 18, msg, cw - 92, th->text); draw_str(cx + 20, cy + 54, fn, cw - 40, th->muted);
    ui_progress_bar(cx + 20, cy + 92, cw - 40, 8, 65, th->track, ui_format_color(fmt));
    hal_present();
}

static void clamp_pdf_pan(void) {
    int vw = hal_get_width(), vh = hal_get_height() - 74;
    int mx = g_pdf_last_pw - vw; if (mx < 0) mx = 0;
    int my = g_pdf_last_ph - vh; if (my < 0) my = 0;
    if (g_pdf_pan_x < 0) g_pdf_pan_x = 0;
    if (g_pdf_pan_x > mx) g_pdf_pan_x = mx;
    if (g_pdf_pan_y < 0) g_pdf_pan_y = 0;
    if (g_pdf_pan_y > my) g_pdf_pan_y = my;
}
static void open_daily_report_modal(ScreenMode ret) {
    int hh, mm, ss; hal_get_local_time(&g_report_y, &g_report_m, &g_report_d, &hh, &mm, &ss);
    g_return_screen = ret; g_auto_active = 0; rpg_profile()->total_reports_viewed++; eval_rpg_and_toast(); g_screen = SCR_DAILY_REPORT;
}
static void step_report_day(int delta) {
    int doy = stats_day_of_year(g_report_y, g_report_m, g_report_d) + delta;
    if (doy < 0) { g_report_y--; doy = stats_days_in_year(g_report_y) - 1; }
    else if (doy >= stats_days_in_year(g_report_y)) { g_report_y++; doy = 0; }
    stats_doy_to_ymd(g_report_y, doy, &g_report_m, &g_report_d);
}

void reader_init(void) {
    memset(&g_doc, 0, sizeof(g_doc)); g_pdf = NULL;
    load_library(); stats_init(hal_get_stats_path()); rpg_init(hal_get_rpg_path());
    hal_set_font_style(g_font_style, g_font_size_idx);
    int y, m, d, hh, mm, ss; hal_get_local_time(&y, &m, &d, &hh, &mm, &ss);
    g_stats_year = y; g_stats_doy = stats_day_of_year(y, m, d);
    g_report_y = y; g_report_m = m; g_report_d = d; g_last_clock_min = mm;
    g_screen = SCR_HOME; g_home_tab = TAB_SHELF; eval_rpg_and_toast();
}

void reader_shutdown(void) {
    save_bookmark(); save_manual_bookmarks(); save_library();
    stats_save(hal_get_stats_path()); rpg_save(hal_get_rpg_path());
    clear_illust_cache(); clear_cover_cache();
    pdf_close(g_pdf); g_pdf = NULL; free(g_pdf_reflow_text); g_pdf_reflow_text = NULL;
    book_doc_free(&g_doc); free(g_page_offsets); g_page_offsets = NULL;
}

static int advance_one_page_auto(void) {
    if (g_screen == SCR_READ_TEXT && g_current_page + 1 < g_page_count) {
        g_current_page++; rpg_profile()->total_auto_turns++; record_page_turn_stat(1); save_bookmark(); return 1;
    }
    if (g_screen == SCR_READ_PDF && g_pdf && g_pdf_page + 1 < pdf_page_count(g_pdf)) {
        g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; rpg_profile()->total_auto_turns++; record_page_turn_stat(1); save_bookmark(); return 1;
    }
    if (g_screen == SCR_READ_IMG) {
        char np[384] = {0};
        if (hal_find_sibling_image(g_filepath, 1, np, sizeof(np))) {
            snprintf(g_filepath, sizeof(g_filepath), "%s", np);
            utf8_ellipsis(get_filename_only(np), 44, g_doc.title, sizeof(g_doc.title));
            rpg_profile()->total_auto_turns++; record_page_turn_stat(1); return 1;
        }
    }
    g_auto_active = 0; show_toast("已達末頁，自動翻頁結束"); return 1;
}

int reader_tick_second(void) {
    int y, m, d, hh, mm, ss; hal_get_local_time(&y, &m, &d, &hh, &mm, &ss);
    int need_redraw = 0;
    if (g_toast_ttl > 0) { g_toast_ttl--; if (g_toast_ttl == 0) { g_toast_msg[0] = '\0'; need_redraw = 1; } }
    int in_read = (g_screen == SCR_READ_TEXT || g_screen == SCR_READ_PDF || g_screen == SCR_READ_IMG);
    if (in_read) {
        stats_add_reading(y, m, d, 1, 0);
        if (g_book_count > 0 && strcmp(g_books[0].path, g_filepath) == 0) g_books[0].read_secs++;
        g_page_secs_cur++; g_eye_cont_secs++; g_unsaved_secs++;
        uint32_t ts = 0, tp = 0; stats_get_day(y, m, d, &ts, &tp);
        int today_key = y * 10000 + m * 100 + d;
        if (g_goal_notified_day != today_key && ts >= (uint32_t)current_goal_mins() * 60u) {
            g_goal_notified_day = today_key; show_toast("恭喜達成今日閱讀目標！按 [I] 看今日總結報告");
            eval_rpg_and_toast(); need_redraw = 1;
        }
        int eye_m = EYE_MINS[g_eye_break_idx];
        if (eye_m > 0 && g_eye_cont_secs >= eye_m * 60) {
            g_eye_cont_secs = 0; g_auto_active = 0; g_return_screen = g_screen; g_screen = SCR_EYE_BREAK; return 1;
        }
        if (g_auto_active) {
            g_auto_elapsed++; need_redraw = 1;
            if (g_auto_elapsed >= AUTO_SECS[g_auto_sec_idx]) { g_auto_elapsed = 0; advance_one_page_auto(); }
        }
        if (g_unsaved_secs >= 60) {
            g_unsaved_secs = 0; stats_save(hal_get_stats_path()); rpg_save(hal_get_rpg_path()); save_library(); eval_rpg_and_toast();
        }
    }
    if (mm != g_last_clock_min) { g_last_clock_min = mm; need_redraw = 1; }
    return need_redraw;
}

int reader_open_file(const char *filepath) {
    if (!filepath || !filepath[0]) return 0;
    if (g_screen != SCR_HOME) { save_bookmark(); save_manual_bookmarks(); }
    g_auto_active = 0; g_page_secs_cur = 0; show_loading_card(filepath); clear_illust_cache();
    BookFormat fmt = book_detect_format(filepath);
    if (fmt == BOOK_FMT_PDF) {
        PdfDoc *nd = pdf_open(filepath); if (!nd) { show_toast("無法開啟 PDF 檔案"); return 0; }
        pdf_close(g_pdf); g_pdf = nd; free(g_pdf_reflow_text); g_pdf_reflow_text = NULL; g_pdf_reflow_page_cached = -1;
        book_doc_free(&g_doc); g_doc.format = BOOK_FMT_PDF;
        const char *pt = pdf_get_title(g_pdf);
        utf8_ellipsis((pt && pt[0]) ? pt : get_filename_only(filepath), 44, g_doc.title, sizeof(g_doc.title));
        sanitize_field(g_doc.author, sizeof(g_doc.author), pdf_get_author(g_pdf));
        int pcnt = pdf_page_count(g_pdf), cap = pcnt > BOOK_MAX_CHAPTERS ? BOOK_MAX_CHAPTERS : pcnt;
        for (int i = 0; i < cap; i++) {
            g_doc.chapters[i].offset = (size_t)i;
            snprintf(g_doc.chapters[i].title, sizeof(g_doc.chapters[i].title), "PDF 第 %d 頁", i + 1);
            g_ch_chars[i] = 0;
        }
        g_doc.chapter_count = cap; snprintf(g_filepath, sizeof(g_filepath), "%s", filepath);
        g_pdf_zoom = 0; g_pdf_pan_x = 0; g_pdf_pan_y = 0; g_pdf_reflow = 0;
        size_t spg = load_bookmark(g_filepath); g_pdf_page = ((int)spg >= 0 && (int)spg < pcnt) ? (int)spg : 0;
        load_manual_bookmarks(g_filepath); g_screen = SCR_READ_PDF; save_bookmark(); eval_rpg_and_toast(); return 1;
    }
    BookDoc next_doc; if (!book_load_document(filepath, &next_doc)) { show_toast("無法開啟此電子書檔案"); return 0; }
    pdf_close(g_pdf); g_pdf = NULL; free(g_pdf_reflow_text); g_pdf_reflow_text = NULL; g_pdf_reflow_page_cached = -1;
    book_doc_free(&g_doc); g_doc = next_doc; snprintf(g_filepath, sizeof(g_filepath), "%s", filepath);
    invalidate_cover_cache_for(filepath); load_manual_bookmarks(g_filepath);
    if (is_image_fmt(g_doc.format)) {
        free(g_page_offsets); g_page_offsets = NULL; g_page_count = 0; g_screen = SCR_READ_IMG;
        save_bookmark(); eval_rpg_and_toast(); return 1;
    }
    g_screen = SCR_READ_TEXT; repaginate(load_bookmark(g_filepath)); save_bookmark(); eval_rpg_and_toast(); return 1;
}

void reader_on_resize(void) {
    hal_set_font_style(g_font_style, g_font_size_idx);
    if (g_doc.text && g_page_offsets && g_page_count > 0) repaginate(g_page_offsets[g_current_page]);
}
static void open_new_book_dialog(void) {
    char path[384] = {0}; snprintf(g_pending_import_folder, sizeof(g_pending_import_folder), "%s", g_cur_folder);
    if (hal_open_file_dialog(path, sizeof(path))) reader_open_file(path);
    g_pending_import_folder[0] = '\0';
}
static void open_toc_screen(void) {
    g_auto_active = 0; g_toc_sel = 0;
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) g_toc_sel = (g_pdf_page < g_doc.chapter_count) ? g_pdf_page : 0;
    else if (g_page_offsets && g_page_count > 0) {
        size_t co = g_page_offsets[g_current_page];
        for (int i = 0; i < g_doc.chapter_count; i++) if (g_doc.chapters[i].offset <= co) g_toc_sel = i; else break;
    }
    g_toc_scroll = 0; g_screen = SCR_TOC;
}
static void open_book_style_modal(int book_idx) {
    if (book_idx < 0 || book_idx >= g_book_count) return;
    g_style_book_idx = book_idx; g_style_row = 0; g_screen = SCR_BOOK_STYLE;
}

static void activate_read_menu_item(int idx) {
    switch (idx) {
        case 0: g_screen = active_read_screen(); break;
        case 1: open_toc_screen(); break;
        case 2: g_auto_active = 0; g_screen = SCR_BOOKMARKS; break;
        case 3: g_auto_active = 0; g_search_ran = 0; g_screen = SCR_SEARCH; break;
        case 4:
            g_auto_active = !g_auto_active; g_auto_elapsed = 0; g_screen = active_read_screen();
            show_toast(g_auto_active ? "已開啟自動翻頁 (按任意鍵暫停)" : "已關閉自動翻頁");
            break;
        case 5:
            g_theme_idx = (g_theme_idx + 1) % NUM_THEMES; rpg_profile()->total_themes_tried++;
            save_library(); eval_rpg_and_toast(); break;
        case 6:
            if (g_doc.format == BOOK_FMT_PDF && g_pdf) { g_pdf_zoom = (g_pdf_zoom + 1) % 5; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
            else { g_font_size_idx = (g_font_size_idx + 1) % 4; save_library(); reader_on_resize(); }
            break;
        case 7:
            if (g_doc.format == BOOK_FMT_TXT) open_txt_editor();
            else open_daily_report_modal(active_read_screen());
            break;
        case 8:
            g_auto_active = 0; save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; break;
    }
}

void reader_on_key(ReaderKey key, char raw_char) {
    int is_ok = (key == RKEY_OK || key == RKEY_SPACE);
    if (g_auto_active && key != RKEY_AUTO &&
        (g_screen == SCR_READ_TEXT || g_screen == SCR_READ_PDF || g_screen == SCR_READ_IMG)) g_auto_active = 0;

    if (g_screen == SCR_EYE_BREAK) {
        if (key == RKEY_BACK || is_ok) { rpg_profile()->total_eye_breaks++; g_eye_cont_secs = 0; eval_rpg_and_toast(); g_screen = g_return_screen; }
        else if (key == RKEY_SHELF) { rpg_profile()->total_eye_breaks++; g_eye_cont_secs = 0; save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; }
        return;
    }
    if (g_screen == SCR_DAILY_REPORT) {
        if (key == RKEY_LEFT) step_report_day(-1);
        else if (key == RKEY_RIGHT) step_report_day(1);
        else if (key == RKEY_BACK || is_ok || key == RKEY_REPORT) g_screen = g_return_screen;
        return;
    }
    if (g_screen == SCR_TXT_EDITOR) {
        if (!g_edit_editing) {
            if (key == RKEY_BACK || key == RKEY_EDIT) { g_screen = SCR_READ_TEXT; return; }
            if (key == RKEY_UP && g_edit_sel > 0) g_edit_sel--;
            else if (key == RKEY_DOWN && g_edit_sel + 1 < g_edit_line_cnt) g_edit_sel++;
            else if (key == RKEY_DELETE && g_edit_line_cnt > 0) commit_txt_line_edit("");
            else if (is_ok && g_edit_line_cnt > 0) start_editing_selected_txt_line();
        } else {
            if (key == RKEY_BACK) { g_edit_editing = 0; return; }
            if (key == RKEY_OK) { commit_txt_line_edit(g_edit_buf); return; }
            if (key == RKEY_BACKSPACE) {
                size_t n = strlen(g_edit_buf);
                if (n > 0) { while (n > 0 && ((unsigned char)g_edit_buf[n - 1] & 0xC0) == 0x80) n--; if (n > 0) n--; g_edit_buf[n] = '\0'; }
                return;
            }
            if (raw_char >= 0x20 && raw_char < 0x7F) {
                size_t n = strlen(g_edit_buf);
                if (n + 1 < sizeof(g_edit_buf)) { g_edit_buf[n] = raw_char; g_edit_buf[n + 1] = '\0'; }
            }
        }
        return;
    }
    if (g_screen == SCR_FILTER) {
        if (key == RKEY_BACK || is_ok) { g_screen = SCR_HOME; return; }
        if (key == RKEY_LEFT || key == RKEY_RIGHT) {
            int d = (key == RKEY_RIGHT) ? 1 : -1;
            g_filter_fmt = (g_filter_fmt + 5 + d) % 5; g_shelf_sel = 0;
        } else if (key == RKEY_UP || key == RKEY_DOWN) {
            int d = (key == RKEY_DOWN) ? 1 : -1;
            g_filter_status = (g_filter_status + 5 + d) % 5; g_shelf_sel = 0;
        }
        return;
    }
    if (g_screen == SCR_BOOK_STYLE) {
        if (key == RKEY_BACK || key == RKEY_STYLE) { save_library(); eval_rpg_and_toast(); g_screen = SCR_HOME; return; }
        if (g_style_book_idx < 0 || g_style_book_idx >= g_book_count) { g_screen = SCR_HOME; return; }
        BookEntry *be = &g_books[g_style_book_idx];
        if (key == RKEY_UP) g_style_row = (g_style_row + 3) % 4;
        else if (key == RKEY_DOWN) g_style_row = (g_style_row + 1) % 4;
        else if (key == RKEY_LEFT || key == RKEY_RIGHT) {
            int d = (key == RKEY_LEFT) ? -1 : 1;
            if (g_style_row == 0) { be->cover_style = (be->cover_style + NUM_COVER_STYLES + d) % NUM_COVER_STYLES; rpg_profile()->total_covers_styled++; }
            else if (g_style_row == 1) be->read_status = (be->read_status + 4 + d) % 4;
            else if (g_style_row == 2) be->rating = (be->rating + 6 + d) % 6;
            save_library(); eval_rpg_and_toast();
        } else if (is_ok) {
            if (g_style_row == 3) {
                char t[384]; snprintf(t, sizeof(t), "%s", be->path);
                save_library(); reader_open_file(t);
            } else {
                if (g_style_row == 0) { be->cover_style = (be->cover_style + 1) % NUM_COVER_STYLES; rpg_profile()->total_covers_styled++; }
                else if (g_style_row == 1) be->read_status = (be->read_status + 1) % 4;
                else be->rating = (be->rating + 1) % 6;
                save_library(); eval_rpg_and_toast();
            }
        }
        return;
    }
    if (g_screen == SCR_NEW_FOLDER) {
        if (key == RKEY_BACK) { g_screen = SCR_HOME; return; }
        if (key == RKEY_BACKSPACE) {
            size_t n = strlen(g_new_folder_buf);
            if (n > 0) { while (n > 0 && ((unsigned char)g_new_folder_buf[n - 1] & 0xC0) == 0x80) n--; if (n > 0) n--; g_new_folder_buf[n] = '\0'; }
            return;
        }
        if (!g_new_folder_buf[0] && (key == RKEY_TAB1 || key == RKEY_TAB2 || key == RKEY_TAB3 || key == RKEY_TAB4)) {
            const char *ps[4] = { "鬼滅之刃", "漫畫", "小說", "講義" };
            folder_create(ps[key - RKEY_TAB1]); g_screen = SCR_HOME; return;
        }
        if (key == RKEY_OK) { if (g_new_folder_buf[0]) folder_create(g_new_folder_buf); g_screen = SCR_HOME; return; }
        if (raw_char >= 0x20 && raw_char < 0x7F && raw_char != '|') {
            size_t n = strlen(g_new_folder_buf);
            if (n + 1 < sizeof(g_new_folder_buf)) { g_new_folder_buf[n] = raw_char; g_new_folder_buf[n + 1] = '\0'; }
        }
        return;
    }
    if (g_screen == SCR_MOVE_FOLDER) {
        int tot = g_folder_count + 1;
        if (key == RKEY_BACK || key == RKEY_MENU) { g_screen = SCR_HOME; return; }
        if (key == RKEY_UP || key == RKEY_LEFT) g_move_folder_sel = (g_move_folder_sel + tot - 1) % tot;
        else if (key == RKEY_DOWN || key == RKEY_RIGHT) g_move_folder_sel = (g_move_folder_sel + 1) % tot;
        else if (is_ok) {
            if (g_move_book_idx >= 0 && g_move_book_idx < g_book_count) {
                if (g_move_folder_sel == 0) g_books[g_move_book_idx].folder[0] = '\0';
                else snprintf(g_books[g_move_book_idx].folder, 48, "%s", g_folders[g_move_folder_sel - 1]);
                save_library();
            }
            g_screen = SCR_HOME;
        }
        return;
    }
    if (g_screen == SCR_NEXT_VOL) {
        if (key == RKEY_BACK) { g_screen = active_read_screen(); return; }
        if (is_ok) {
            if (g_next_vol_idx >= 0 && g_next_vol_idx < g_book_count) { char tgt[384]; snprintf(tgt, sizeof(tgt), "%s", g_books[g_next_vol_idx].path); reader_open_file(tgt); }
            else g_screen = active_read_screen();
        }
        return;
    }
    if (g_screen == SCR_SEARCH) {
        if (key == RKEY_BACK) { g_screen = active_read_screen(); return; }
        if (key == RKEY_BACKSPACE) {
            size_t n = strlen(g_search_query);
            if (n > 0) { while (n > 0 && ((unsigned char)g_search_query[n - 1] & 0xC0) == 0x80) n--; if (n > 0) n--; g_search_query[n] = '\0'; g_search_ran = 0; }
            return;
        }
        if (!g_search_query[0] && (key == RKEY_TAB1 || key == RKEY_TAB2 || key == RKEY_TAB3 || key == RKEY_TAB4)) {
            const char *ps[4] = { "第一章", "序章", "結論", "重點" };
            snprintf(g_search_query, sizeof(g_search_query), "%s", ps[key - RKEY_TAB1]); run_fulltext_search(); return;
        }
        if (key == RKEY_UP && g_search_sel > 0) { g_search_sel--; return; }
        if (key == RKEY_DOWN && g_search_sel + 1 < g_search_cnt) { g_search_sel++; return; }
        if (key == RKEY_OK) {
            if (!g_search_ran) run_fulltext_search();
            else if (g_search_cnt > 0) {
                SearchHit *sh = &g_search_hits[g_search_sel];
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) { g_pdf_page = (int)sh->pos_val; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                else jump_to_offset(sh->pos_val);
                save_bookmark(); g_screen = active_read_screen();
            } else run_fulltext_search();
            return;
        }
        if (raw_char >= 0x20 && raw_char < 0x7F) {
            size_t n = strlen(g_search_query);
            if (n + 1 < sizeof(g_search_query)) { g_search_query[n] = raw_char; g_search_query[n + 1] = '\0'; g_search_ran = 0; }
        }
        return;
    }
    if (g_screen == SCR_BOOKMARKS) {
        if (key == RKEY_BACK || key == RKEY_BOOKMARK) { g_screen = active_read_screen(); return; }
        if ((key == RKEY_UP || key == RKEY_LEFT) && g_bmark_sel > 0) { g_bmark_sel--; return; }
        if ((key == RKEY_DOWN || key == RKEY_RIGHT) && g_bmark_sel + 1 < g_bmark_count) { g_bmark_sel++; return; }
        if (key == RKEY_SPACE || key == RKEY_FONT_UP) { add_current_manual_bookmark(); return; }
        if (key == RKEY_DELETE && g_bmark_count > 0) {
            for (int i = g_bmark_sel; i + 1 < g_bmark_count; i++) g_bmarks[i] = g_bmarks[i + 1];
            g_bmark_count--; if (g_bmark_sel >= g_bmark_count && g_bmark_count > 0) g_bmark_sel = g_bmark_count - 1;
            save_manual_bookmarks(); return;
        }
        if (key == RKEY_OK) {
            if (g_bmark_count == 0) add_current_manual_bookmark();
            else {
                ManualBookmark *bm = &g_bmarks[g_bmark_sel];
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) { int p = (int)bm->pos_val; if (p >= 0 && p < pdf_page_count(g_pdf)) { g_pdf_page = p; g_pdf_pan_x = 0; g_pdf_pan_y = 0; } }
                else jump_to_offset(bm->pos_val);
                save_bookmark(); g_screen = active_read_screen();
            }
        }
        return;
    }
    if (g_screen == SCR_HOME) {
        if (key == RKEY_TAB1) { g_home_tab = TAB_SHELF; return; }
        if (key == RKEY_TAB2) { g_home_tab = TAB_MANAGE; return; }
        if (key == RKEY_TAB3) { g_home_tab = TAB_STATS; return; }
        if (key == RKEY_TAB4) { g_home_tab = TAB_ACHIEVE; return; }
        if (key == RKEY_TAB5) { g_home_tab = TAB_APPEARANCE; return; }
        if (key == RKEY_TAB6 || key == RKEY_OPEN) { open_new_book_dialog(); return; }
        if (key == RKEY_REPORT) { open_daily_report_modal(SCR_HOME); return; }
        if (key == RKEY_THEME) { g_theme_idx = (g_theme_idx + 1) % NUM_THEMES; rpg_profile()->total_themes_tried++; save_library(); eval_rpg_and_toast(); return; }
        if (raw_char == 'n' || raw_char == 'N') { g_new_folder_buf[0] = '\0'; g_screen = SCR_NEW_FOLDER; return; }
        if (key == RKEY_BACK) { if (g_home_tab == TAB_SHELF && g_cur_folder[0]) { g_cur_folder[0] = '\0'; g_shelf_sel = 0; } else hal_request_quit(); return; }

        if (g_home_tab == TAB_STATS) {
            if (key == RKEY_SPACE || key == RKEY_OK) { g_stats_subtab = (g_stats_subtab + 1) % 5; return; }
            if (g_stats_subtab == 0) {
                if (key == RKEY_UP) g_stats_doy -= 1;
                else if (key == RKEY_DOWN) g_stats_doy += 1;
                else if (key == RKEY_LEFT) g_stats_doy -= 7;
                else if (key == RKEY_RIGHT) g_stats_doy += 7;
                else if (key == RKEY_PGUP && g_stats_year > 2020) g_stats_year--;
                else if (key == RKEY_PGDN && g_stats_year < 2040) g_stats_year++;
                int max_d = stats_days_in_year(g_stats_year) - 1;
                if (g_stats_doy < 0) g_stats_doy = 0;
                if (g_stats_doy > max_d) g_stats_doy = max_d;
            } else if (g_stats_subtab == 2) {
                if ((key == RKEY_LEFT || key == RKEY_PGUP) && g_stats_year > 2020) g_stats_year--;
                else if ((key == RKEY_RIGHT || key == RKEY_PGDN) && g_stats_year < 2040) g_stats_year++;
            } else if (g_stats_subtab == 4) {
                if (key == RKEY_LEFT) step_report_day(-1);
                else if (key == RKEY_RIGHT) step_report_day(1);
            }
            return;
        }
        if (g_home_tab == TAB_ACHIEVE) {
            if (key == RKEY_SPACE || key == RKEY_OK) { g_ach_subtab = 1 - g_ach_subtab; return; }
            if (g_ach_subtab == 1) {
                if (key == RKEY_LEFT) { g_ach_cat = (g_ach_cat + NUM_ACH_CATS - 1) % NUM_ACH_CATS; g_ach_scroll = 0; }
                else if (key == RKEY_RIGHT) { g_ach_cat = (g_ach_cat + 1) % NUM_ACH_CATS; g_ach_scroll = 0; }
                else if (key == RKEY_UP && g_ach_scroll > 0) g_ach_scroll--;
                else if (key == RKEY_DOWN && g_ach_scroll < 4) g_ach_scroll++;
            }
            return;
        }
        if (g_home_tab == TAB_APPEARANCE) {
            if (key == RKEY_SPACE) { g_appear_subtab = 1 - g_appear_subtab; return; }
            if (g_appear_subtab == 0) {
                if (key == RKEY_LEFT) { g_theme_idx = (g_theme_idx + NUM_THEMES - 1) % NUM_THEMES; g_theme_page = g_theme_idx / 4; rpg_profile()->total_themes_tried++; save_library(); eval_rpg_and_toast(); }
                else if (key == RKEY_RIGHT || key == RKEY_OK) { g_theme_idx = (g_theme_idx + 1) % NUM_THEMES; g_theme_page = g_theme_idx / 4; rpg_profile()->total_themes_tried++; save_library(); eval_rpg_and_toast(); }
                else if (key == RKEY_PGUP || key == RKEY_UP) g_theme_page = (g_theme_page + 2) % 3;
                else if (key == RKEY_PGDN || key == RKEY_DOWN) g_theme_page = (g_theme_page + 1) % 3;
            } else {
                if (key == RKEY_UP) g_set_row = (g_set_row + 7) % 8;
                else if (key == RKEY_DOWN) g_set_row = (g_set_row + 1) % 8;
                else if (key == RKEY_LEFT || key == RKEY_RIGHT || key == RKEY_OK) {
                    int d = (key == RKEY_LEFT) ? -1 : 1;
                    switch (g_set_row) {
                        case 0: g_font_style = (g_font_style + 4 + d) % 4; hal_set_font_style(g_font_style, g_font_size_idx); break;
                        case 1: g_font_size_idx = (g_font_size_idx + 4 + d) % 4; hal_set_font_style(g_font_style, g_font_size_idx); break;
                        case 2: g_spacing_idx = (g_spacing_idx + 5 + d) % 5; break;
                        case 3: g_margin_idx = (g_margin_idx + 4 + d) % 4; break;
                        case 4: g_transition_idx = (g_transition_idx + 4 + d) % 4; break;
                        case 5: g_auto_sec_idx = (g_auto_sec_idx + 7 + d) % 7; break;
                        case 6: g_daily_goal_idx = (g_daily_goal_idx + 8 + d) % 8; break;
                        case 7: g_eye_break_idx = (g_eye_break_idx + 5 + d) % 5; break;
                    }
                    save_library();
                }
            }
            return;
        }
        if (g_home_tab == TAB_SHELF) {
            if (g_cur_folder[0] && (key == RKEY_BACKSPACE || key == RKEY_SHELF)) { g_cur_folder[0] = '\0'; g_shelf_sel = 0; return; }
            if (key == RKEY_SEARCH) { g_filter_fmt = (g_filter_fmt + 1) % 5; g_shelf_sel = 0; return; }
            int gtypes[MAX_BOOKS + MAX_FOLDERS], gidxs[MAX_BOOKS + MAX_FOLDERS];
            int gcnt = shelf_build_grid(gtypes, gidxs, MAX_BOOKS + MAX_FOLDERS);
            int has_hero = (g_cur_folder[0] == '\0' && g_filter_fmt == 0 && g_filter_status == 0 && g_book_count > 0) ? 1 : 0;
            int tot = has_hero + gcnt;
            if (tot == 0) { if (is_ok) open_new_book_dialog(); return; }
            if (g_shelf_sel >= tot) g_shelf_sel = tot - 1;
            if (key == RKEY_LEFT && g_shelf_sel > 0) g_shelf_sel--;
            else if (key == RKEY_RIGHT && g_shelf_sel + 1 < tot) g_shelf_sel++;
            else if (key == RKEY_UP) { if (has_hero && g_shelf_sel >= 4) g_shelf_sel -= 3; else if (has_hero && g_shelf_sel >= 1) g_shelf_sel = 0; else if (!has_hero && g_shelf_sel >= 3) g_shelf_sel -= 3; }
            else if (key == RKEY_DOWN) { if (has_hero && g_shelf_sel == 0 && gcnt > 0) g_shelf_sel = 1; else if (g_shelf_sel + 3 < tot) g_shelf_sel += 3; else if (g_shelf_sel + 1 < tot) g_shelf_sel = tot - 1; }
            else if (key == RKEY_STYLE) {
                int bi = (has_hero && g_shelf_sel == 0) ? 0 : ((g_shelf_sel - has_hero < gcnt && gtypes[g_shelf_sel - has_hero] == 0) ? gidxs[g_shelf_sel - has_hero] : -1);
                if (bi >= 0) open_book_style_modal(bi);
            } else if (key == RKEY_MENU) {
                int bi = (has_hero && g_shelf_sel == 0) ? 0 : ((g_shelf_sel - has_hero < gcnt && gtypes[g_shelf_sel - has_hero] == 0) ? gidxs[g_shelf_sel - has_hero] : -1);
                if (bi >= 0) { g_move_book_idx = bi; g_move_folder_sel = 0; g_screen = SCR_MOVE_FOLDER; }
            } else if (key == RKEY_DELETE) {
                if (has_hero && g_shelf_sel == 0) remove_book_at(0);
                else { int gi = g_shelf_sel - has_hero; if (gi >= 0 && gi < gcnt) { if (gtypes[gi] == 1) folder_delete(gidxs[gi]); else remove_book_at(gidxs[gi]); } }
            } else if (is_ok) {
                if (has_hero && g_shelf_sel == 0) { char t[384]; snprintf(t, sizeof(t), "%s", g_books[0].path); reader_open_file(t); }
                else {
                    int gi = g_shelf_sel - has_hero;
                    if (gi >= 0 && gi < gcnt) {
                        if (gtypes[gi] == 1) { snprintf(g_cur_folder, 48, "%s", g_folders[gidxs[gi]]); g_shelf_sel = 0; }
                        else { char t[384]; snprintf(t, sizeof(t), "%s", g_books[gidxs[gi]].path); reader_open_file(t); }
                    }
                }
            }
            return;
        }
        if (g_home_tab == TAB_MANAGE && g_book_count > 0) {
            if (key == RKEY_UP && g_shelf_sel > 0) g_shelf_sel--;
            else if (key == RKEY_DOWN && g_shelf_sel + 1 < g_book_count) g_shelf_sel++;
            else if (key == RKEY_STYLE) open_book_style_modal(g_shelf_sel);
            else if (key == RKEY_MENU) { g_move_book_idx = g_shelf_sel; g_move_folder_sel = 0; g_screen = SCR_MOVE_FOLDER; }
            else if (key == RKEY_DELETE) remove_book_at(g_shelf_sel);
            else if (is_ok) { char t[384]; snprintf(t, sizeof(t), "%s", g_books[g_shelf_sel].path); reader_open_file(t); }
        }
        return;
    }

    if (g_screen == SCR_READ_MENU) {
        if (key == RKEY_BACK || key == RKEY_MENU) { g_screen = active_read_screen(); return; }
        if (key == RKEY_UP) g_read_menu_sel = (g_read_menu_sel + 8) % 9;
        else if (key == RKEY_DOWN) g_read_menu_sel = (g_read_menu_sel + 1) % 9;
        else if (key == RKEY_LEFT || key == RKEY_RIGHT) {
            int d = (key == RKEY_RIGHT) ? 1 : -1;
            if (g_read_menu_sel == 5) { g_theme_idx = (g_theme_idx + NUM_THEMES + d) % NUM_THEMES; save_library(); }
            else if (g_read_menu_sel == 6) {
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) { g_pdf_zoom = (g_pdf_zoom + 5 + d) % 5; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                else { g_font_size_idx = (g_font_size_idx + 4 + d) % 4; save_library(); reader_on_resize(); }
            }
        } else if (is_ok) activate_read_menu_item(g_read_menu_sel);
        return;
    }
    if (g_screen == SCR_TOC) {
        if (key == RKEY_BACK || key == RKEY_TOC) { g_screen = active_read_screen(); return; }
        if ((key == RKEY_UP || key == RKEY_LEFT) && g_toc_sel > 0) g_toc_sel--;
        else if ((key == RKEY_DOWN || key == RKEY_RIGHT) && g_toc_sel + 1 < g_doc.chapter_count) g_toc_sel++;
        else if (key == RKEY_PGUP) g_toc_sel = (g_toc_sel >= 8) ? (g_toc_sel - 8) : 0;
        else if (key == RKEY_PGDN) { g_toc_sel += 8; if (g_toc_sel >= g_doc.chapter_count) g_toc_sel = g_doc.chapter_count > 0 ? g_doc.chapter_count - 1 : 0; }
        else if (is_ok) {
            if (g_doc.chapter_count > 0) {
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) { g_pdf_page = (int)g_doc.chapters[g_toc_sel].offset; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                else jump_to_offset(g_doc.chapters[g_toc_sel].offset);
                save_bookmark();
            }
            g_screen = active_read_screen();
        }
        return;
    }
    if (g_screen == SCR_JUMP) {
        int tot = (g_doc.format == BOOK_FMT_PDF && g_pdf) ? pdf_page_count(g_pdf) : g_page_count;
        if (key == RKEY_BACK || key == RKEY_JUMP) { g_screen = active_read_screen(); return; }
        if (key == RKEY_LEFT) g_jump_target -= 10;
        else if (key == RKEY_RIGHT) g_jump_target += 10;
        else if (key == RKEY_UP) g_jump_target -= 50;
        else if (key == RKEY_DOWN) g_jump_target += 50;
        else if (key == RKEY_PGUP) g_jump_target -= 1;
        else if (key == RKEY_PGDN) g_jump_target += 1;
        else if (is_ok) {
            if (g_jump_target >= 0 && g_jump_target < tot) {
                if (g_doc.format == BOOK_FMT_PDF && g_pdf) { g_pdf_page = g_jump_target; g_pdf_pan_x = 0; g_pdf_pan_y = 0; }
                else g_current_page = g_jump_target;
                save_bookmark();
            }
            g_screen = active_read_screen(); return;
        }
        if (g_jump_target < 0) g_jump_target = 0;
        if (g_jump_target >= tot) g_jump_target = tot > 0 ? tot - 1 : 0;
        return;
    }

    if (key == RKEY_AUTO) { g_auto_active = !g_auto_active; g_auto_elapsed = 0; show_toast(g_auto_active ? "已開啟自動翻頁" : "已關閉自動翻頁"); return; }
    if (key == RKEY_REPORT) { open_daily_report_modal(g_screen); return; }
    if (key == RKEY_EDIT && g_doc.format == BOOK_FMT_TXT) { open_txt_editor(); return; }

    if (g_screen == SCR_READ_PDF && g_pdf) {
        int tot = pdf_page_count(g_pdf), vw = hal_get_width(), vh = hal_get_height() - 74;
        int cpx = (!g_pdf_reflow && g_pdf_last_pw > vw), cpy = (!g_pdf_reflow && g_pdf_last_ph > vh);
        switch (key) {
            case RKEY_PGUP: if (g_pdf_page > 0) { g_pdf_page--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(-1); save_bookmark(); } break;
            case RKEY_PGDN: case RKEY_SPACE:
                if (g_pdf_page + 1 < tot) { g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(1); save_bookmark(); }
                else { int nv = find_next_volume_in_folder(g_filepath); if (nv >= 0) { g_next_vol_idx = nv; g_screen = SCR_NEXT_VOL; } }
                break;
            case RKEY_LEFT:
                if (cpx && g_pdf_pan_x > 0) { g_pdf_pan_x -= 90; clamp_pdf_pan(); }
                else if (g_pdf_page > 0) { g_pdf_page--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(-1); save_bookmark(); }
                break;
            case RKEY_RIGHT:
                if (cpx && g_pdf_pan_x < g_pdf_last_pw - vw) { g_pdf_pan_x += 90; clamp_pdf_pan(); }
                else if (g_pdf_page + 1 < tot) { g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(1); save_bookmark(); }
                break;
            case RKEY_UP:
                if (cpy && g_pdf_pan_y > 0) { g_pdf_pan_y -= 90; clamp_pdf_pan(); }
                else if (g_pdf_page > 0) { g_pdf_page--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(-1); save_bookmark(); }
                break;
            case RKEY_DOWN:
                if (cpy && g_pdf_pan_y < g_pdf_last_ph - vh) { g_pdf_pan_y += 90; clamp_pdf_pan(); }
                else if (g_pdf_page + 1 < tot) { g_pdf_page++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; record_page_turn_stat(1); save_bookmark(); }
                break;
            case RKEY_FONT_UP: if (g_pdf_zoom < 4) { g_pdf_zoom++; g_pdf_pan_x = 0; g_pdf_pan_y = 0; } break;
            case RKEY_FONT_DOWN: if (g_pdf_zoom > 0) { g_pdf_zoom--; g_pdf_pan_x = 0; g_pdf_pan_y = 0; } break;
            case RKEY_REFLOW: g_pdf_reflow = !g_pdf_reflow; break;
            case RKEY_THEME: g_theme_idx = (g_theme_idx + 1) % NUM_THEMES; save_library(); break;
            case RKEY_TOC: open_toc_screen(); break;
            case RKEY_JUMP: g_jump_target = g_pdf_page; g_screen = SCR_JUMP; break;
            case RKEY_BOOKMARK: g_screen = SCR_BOOKMARKS; break;
            case RKEY_SEARCH: g_search_ran = 0; g_screen = SCR_SEARCH; break;
            case RKEY_OPEN: open_new_book_dialog(); break;
            case RKEY_SHELF: case RKEY_BACKSPACE: save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; break;
            case RKEY_BACK: case RKEY_MENU: g_read_menu_sel = 0; g_screen = SCR_READ_MENU; break;
            default: break;
        }
        return;
    }
    if (g_screen == SCR_READ_IMG) {
        if (key == RKEY_LEFT || key == RKEY_UP || key == RKEY_PGUP) {
            char np[384] = {0}; if (hal_find_sibling_image(g_filepath, -1, np, sizeof(np))) { snprintf(g_filepath, sizeof(g_filepath), "%s", np); utf8_ellipsis(get_filename_only(np), 44, g_doc.title, sizeof(g_doc.title)); record_page_turn_stat(-1); }
        } else if (key == RKEY_RIGHT || key == RKEY_DOWN || key == RKEY_PGDN || key == RKEY_SPACE) {
            char np[384] = {0}; if (hal_find_sibling_image(g_filepath, 1, np, sizeof(np))) { snprintf(g_filepath, sizeof(g_filepath), "%s", np); utf8_ellipsis(get_filename_only(np), 44, g_doc.title, sizeof(g_doc.title)); record_page_turn_stat(1); }
        } else if (key == RKEY_THEME) { g_theme_idx = (g_theme_idx + 1) % NUM_THEMES; save_library(); }
        else if (key == RKEY_OPEN) open_new_book_dialog();
        else if (key == RKEY_SHELF || key == RKEY_BACKSPACE) { save_bookmark(); g_screen = SCR_HOME; }
        else if (key == RKEY_BACK || key == RKEY_MENU) { g_read_menu_sel = 0; g_screen = SCR_READ_MENU; }
        return;
    }
    switch (key) {
        case RKEY_LEFT: case RKEY_UP: case RKEY_PGUP:
            if (g_current_page > 0) { g_current_page--; record_page_turn_stat(-1); save_bookmark(); } break;
        case RKEY_RIGHT: case RKEY_DOWN: case RKEY_PGDN: case RKEY_SPACE:
            if (g_current_page + 1 < g_page_count) { g_current_page++; record_page_turn_stat(1); save_bookmark(); }
            else { int nv = find_next_volume_in_folder(g_filepath); if (nv >= 0) { g_next_vol_idx = nv; g_screen = SCR_NEXT_VOL; } }
            break;
        case RKEY_FONT_UP: if (g_font_size_idx < 3) { g_font_size_idx++; save_library(); reader_on_resize(); } break;
        case RKEY_FONT_DOWN: if (g_font_size_idx > 0) { g_font_size_idx--; save_library(); reader_on_resize(); } break;
        case RKEY_TOC: open_toc_screen(); break;
        case RKEY_JUMP: if (g_page_count > 0) { g_jump_target = g_current_page; g_screen = SCR_JUMP; } break;
        case RKEY_BOOKMARK: g_screen = SCR_BOOKMARKS; break;
        case RKEY_SEARCH: g_search_ran = 0; g_screen = SCR_SEARCH; break;
        case RKEY_THEME: g_theme_idx = (g_theme_idx + 1) % NUM_THEMES; save_library(); break;
        case RKEY_SPACING: g_spacing_idx = (g_spacing_idx + 1) % 5; save_library(); reader_on_resize(); break;
        case RKEY_OPEN: open_new_book_dialog(); break;
        case RKEY_SHELF: case RKEY_BACKSPACE: save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; break;
        case RKEY_BACK: case RKEY_MENU: g_read_menu_sel = 0; g_screen = SCR_READ_MENU; break;
        default: break;
    }
}

void reader_on_touch(int tx, int ty) {
    int w = hal_get_width(), h = hal_get_height();
    if (tx < 0 || ty < 0 || tx >= w || ty >= h) return;
    if (g_auto_active && (g_screen == SCR_READ_TEXT || g_screen == SCR_READ_PDF || g_screen == SCR_READ_IMG)) {
        g_auto_active = 0; show_toast("觸控已暫停自動翻頁"); return;
    }
    if (g_screen == SCR_EYE_BREAK) {
        int bw = 460, bh = 210, bx = (w - bw) / 2, by = (h - bh) / 2;
        if (ty >= by + 148) {
            rpg_profile()->total_eye_breaks++;
            if (tx < bx + bw / 3) { g_eye_cont_secs = 0; g_screen = g_return_screen; }
            else if (tx < bx + (bw * 2) / 3) { g_eye_cont_secs = (EYE_MINS[g_eye_break_idx] - 10) * 60; if (g_eye_cont_secs < 0) g_eye_cont_secs = 0; g_screen = g_return_screen; }
            else { g_eye_cont_secs = 0; save_bookmark(); g_screen = SCR_HOME; }
            eval_rpg_and_toast();
        }
        return;
    }
    if (g_screen == SCR_DAILY_REPORT) {
        int mw = 540, mh = 360, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = g_return_screen; return; }
        if (ty <= my + 46) { if (tx < mx + 110) step_report_day(-1); else if (tx > mx + mw - 110) step_report_day(1); return; }
        if (ty >= my + mh - 46) g_screen = g_return_screen;
        return;
    }
    if (g_screen == SCR_TXT_EDITOR) {
        int mw = 600, mh = 390, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_edit_editing = 0; g_screen = SCR_READ_TEXT; return; }
        if (!g_edit_editing) {
            if (ty >= my + 54 && ty < my + mh - 50) { int row = (ty - (my + 54)) / 24; if (row >= 0 && row < g_edit_line_cnt) { g_edit_sel = row; start_editing_selected_txt_line(); } }
        } else {
            if (ty >= my + 160 && ty <= my + 204) {
                int slot = (tx - (mx + 20)) / 140;
                if (slot == 0) txt_clean_line_buf(g_edit_buf, sizeof(g_edit_buf), 0);
                else if (slot == 1) txt_clean_line_buf(g_edit_buf, sizeof(g_edit_buf), 1);
                else if (slot == 2) g_edit_buf[0] = '\0';
                else g_edit_editing = 0;
                return;
            }
            if (ty >= my + 236) { if (tx < mx + mw / 2) commit_txt_line_edit(g_edit_buf); else g_edit_editing = 0; }
        }
        return;
    }
    if (g_screen == SCR_FILTER) {
        int mw = 480, mh = 270, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = SCR_HOME; return; }
        if (ty >= my + 78 && ty <= my + 116) {
            int fi = (tx - (mx + 20)) / 88;
            if (fi >= 0 && fi < 5) { g_filter_fmt = fi; g_shelf_sel = 0; }
            return;
        }
        if (ty >= my + 152 && ty <= my + 190) {
            int si = (tx - (mx + 20)) / 88;
            if (si >= 0 && si < 5) { g_filter_status = si; g_shelf_sel = 0; }
            return;
        }
        if (ty >= my + 214) {
            if (tx < mx + mw / 2) { g_filter_fmt = 0; g_filter_status = 0; g_shelf_sel = 0; }
            g_screen = SCR_HOME;
        }
        return;
    }
    if (g_screen == SCR_BOOK_STYLE) {
        BookEntry *be = (g_style_book_idx >= 0 && g_style_book_idx < g_book_count) ? &g_books[g_style_book_idx] : NULL;
        if (!be) { g_screen = SCR_HOME; return; }
        if (ty <= 44 && tx <= 116) { save_library(); eval_rpg_and_toast(); g_screen = SCR_HOME; return; }
        /* 左欄：封面模板切換 (< / >) */
        if (tx >= 28 && tx <= 216 && ty >= 316 && ty <= 360) {
            int d = (tx < 122) ? -1 : 1;
            be->cover_style = (be->cover_style + NUM_COVER_STYLES + d) % NUM_COVER_STYLES;
            rpg_profile()->total_covers_styled++; save_library(); eval_rpg_and_toast(); return;
        }
        /* 右欄：5顆星星點擊 */
        if (tx >= 244 && tx <= 520 && ty >= 100 && ty <= 142) {
            int star = (tx - 244) / 36 + 1;
            if (star >= 1 && star <= 5) { be->rating = (be->rating == star) ? 0 : star; save_library(); eval_rpg_and_toast(); }
            return;
        }
        /* 右欄：4個閱讀狀態膠囊 */
        if (tx >= 244 && tx <= 760 && ty >= 170 && ty <= 212) {
            int st = (tx - 244) / 118;
            if (st >= 0 && st < 4) { be->read_status = st; save_library(); eval_rpg_and_toast(); }
            return;
        }
        /* 右欄底部 3 顆大操作鈕：[開始/繼續閱讀] [移資料夾] [移除] */
        if (ty >= 390 && ty <= 442 && tx >= 244) {
            if (tx < 490) {
                char t[384]; snprintf(t, sizeof(t), "%s", be->path);
                save_library(); reader_open_file(t);
            } else if (tx < 632) {
                g_move_book_idx = g_style_book_idx; g_move_folder_sel = 0; g_screen = SCR_MOVE_FOLDER;
            } else {
                remove_book_at(g_style_book_idx); g_screen = SCR_HOME;
            }
        }
        return;
    }
    if (g_screen == SCR_NEW_FOLDER) {
        int mw = 480, mh = 250, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = SCR_HOME; return; }
        if (ty >= my + 114 && ty <= my + 150) {
            const char *ps[4] = { "鬼滅之刃", "漫畫", "小說", "講義" };
            int bw = (mw - 60) / 4, idx = (tx - (mx + 18)) / (bw + 8);
            if (idx >= 0 && idx < 4) { folder_create(ps[idx]); g_screen = SCR_HOME; return; }
        }
        if (ty >= my + 186) { if (tx < mx + mw / 2 && g_new_folder_buf[0]) folder_create(g_new_folder_buf); g_screen = SCR_HOME; }
        return;
    }
    if (g_screen == SCR_MOVE_FOLDER) {
        int mw = 440, mh = 340, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = SCR_HOME; return; }
        if (ty >= my + 56 && ty < my + mh - 30) {
            int start = (g_move_folder_sel >= 6) ? (g_move_folder_sel - 5) : 0, opt = start + (ty - (my + 56)) / 38;
            if (opt >= 0 && opt <= g_folder_count) { g_move_folder_sel = opt; reader_on_key(RKEY_OK, 0); }
        }
        return;
    }
    if (g_screen == SCR_NEXT_VOL) {
        int bw = 460, bh = 170, bx = (w - bw) / 2, by = (h - bh) / 2;
        if (tx < bx || tx > bx + bw || ty < by || ty > by + bh) { g_screen = active_read_screen(); return; }
        if (ty >= by + 110) { if (tx < bx + bw / 2) reader_on_key(RKEY_OK, 0); else g_screen = active_read_screen(); }
        return;
    }
    if (g_screen == SCR_READ_MENU) {
        int mw = 450, mh = 408, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = active_read_screen(); return; }
        if (ty >= my + 48) { int idx = (ty - (my + 48)) / 38; if (idx >= 0 && idx < 9) { g_read_menu_sel = idx; activate_read_menu_item(idx); } }
        return;
    }
    if (g_screen == SCR_BOOKMARKS) {
        int mw = 540, mh = 360, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = active_read_screen(); return; }
        if (ty <= my + 46 && tx >= mx + mw - 140) { add_current_manual_bookmark(); return; }
        if (g_bmark_count == 0) { add_current_manual_bookmark(); return; }
        if (ty >= my + 56 && ty < my + mh - 34) {
            int start = (g_bmark_sel >= 7) ? (g_bmark_sel - 6) : 0, idx = start + (ty - (my + 56)) / 38;
            if (idx >= 0 && idx < g_bmark_count) {
                if (tx >= mx + mw - 64) {
                    for (int i = idx; i + 1 < g_bmark_count; i++) g_bmarks[i] = g_bmarks[i + 1];
                    g_bmark_count--; if (g_bmark_sel >= g_bmark_count && g_bmark_count > 0) g_bmark_sel = g_bmark_count - 1;
                    save_manual_bookmarks();
                } else { g_bmark_sel = idx; reader_on_key(RKEY_OK, 0); }
            }
        }
        return;
    }
    if (g_screen == SCR_SEARCH) {
        int mw = 580, mh = 380, mx = (w - mw) / 2, my = (h - mh) / 2;
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) { g_screen = active_read_screen(); return; }
        if (g_search_ran && g_search_cnt > 0 && ty >= my + 104 && ty < my + mh - 34) {
            int idx = g_search_scroll + (ty - (my + 104)) / 38;
            if (idx >= 0 && idx < g_search_cnt) { g_search_sel = idx; reader_on_key(RKEY_OK, 0); }
        }
        return;
    }
    if (g_screen == SCR_JUMP) {
        int bw = 460, bh = 160, bx = (w - bw) / 2, by = (h - bh) / 2;
        if (tx < bx || tx > bx + bw || ty < by || ty > by + bh) { g_screen = active_read_screen(); return; }
        int tot = (g_doc.format == BOOK_FMT_PDF && g_pdf) ? pdf_page_count(g_pdf) : g_page_count;
        if (ty >= by + 50 && ty <= by + 90 && tot > 1) {
            int sx = bx + 24, sw = bw - 48, rel = tx - sx; if (rel < 0) rel = 0; if (rel > sw) rel = sw;
            g_jump_target = (rel * (tot - 1)) / sw; return;
        }
        if (ty >= by + 104) { if (tx < bx + bw / 2) reader_on_key(RKEY_OK, 0); else g_screen = active_read_screen(); }
        return;
    }
    if (g_screen == SCR_TOC) {
        if (ty <= 44 && tx >= w - 100) { g_screen = active_read_screen(); return; }
        if (ty >= h - 34) { if (tx < w / 3) reader_on_key(RKEY_PGUP, 0); else if (tx < (w * 2) / 3) reader_on_key(RKEY_PGDN, 0); else g_screen = active_read_screen(); return; }
        int row = (ty - 56) / 38, idx = g_toc_scroll + row;
        if (row >= 0 && idx >= 0 && idx < g_doc.chapter_count) { g_toc_sel = idx; reader_on_key(RKEY_OK, 0); }
        return;
    }
    if (g_screen == SCR_HOME) {
        if (tx < 76) {
            for (int i = 0; i < 6; i++) {
                int y0 = 60 + i * 62;
                if (ty >= y0 && ty <= y0 + 54) { if (i < 5) g_home_tab = (HomeTab)i; else open_new_book_dialog(); return; }
            }
            return;
        }
        int mx = 94, mw = w - mx - 18;
        if (g_home_tab == TAB_SHELF) {
            if (ty <= 46) {
                if (g_cur_folder[0] && tx < mx + 92) { g_cur_folder[0] = '\0'; g_shelf_sel = 0; return; }
                if (tx >= w - 134) { open_new_book_dialog(); return; }
                if (!g_cur_folder[0] && tx >= w - 228 && tx < w - 140) { g_new_folder_buf[0] = '\0'; g_screen = SCR_NEW_FOLDER; return; }
                if (!g_cur_folder[0] && tx >= w - 312 && tx < w - 234) { open_daily_report_modal(SCR_HOME); return; }
                if (!g_cur_folder[0] && tx >= w - 392 && tx < w - 318) { g_screen = SCR_FILTER; return; }
                return;
            }
            int gtypes[MAX_BOOKS + MAX_FOLDERS], gidxs[MAX_BOOKS + MAX_FOLDERS];
            int gcnt = shelf_build_grid(gtypes, gidxs, MAX_BOOKS + MAX_FOLDERS);
            int has_hero = (g_cur_folder[0] == '\0' && g_filter_fmt == 0 && g_filter_status == 0 && g_book_count > 0) ? 1 : 0;
            if (has_hero && ty >= 56 && ty <= 172) {
                if (tx >= mx + mw - 126 && ty >= 122) { open_book_style_modal(0); return; }
                char t[384]; snprintf(t, sizeof(t), "%s", g_books[0].path); reader_open_file(t); return;
            }
            int grid_y = has_hero ? 204 : 60, gap = 16, card_w = (mw - gap * 2) / 3;
            if (ty >= grid_y && ty <= h - 26) {
                if (gcnt == 0 && !has_hero) { open_new_book_dialog(); return; }
                int grid_sel = (g_shelf_sel >= has_hero) ? (g_shelf_sel - has_hero) : 0;
                int per_pg = has_hero ? 3 : 6, start_gi = (grid_sel / per_pg) * per_pg;
                int row = has_hero ? 0 : ((ty - grid_y) / (((h - grid_y - 34 - gap) / 2) + gap));
                if (row >= 0 && row < (has_hero ? 1 : 2)) {
                    for (int col = 0; col < 3; col++) {
                        int cx = mx + col * (card_w + gap), gi = start_gi + row * 3 + col;
                        if (gi < gcnt && tx >= cx && tx <= cx + card_w) {
                            g_shelf_sel = gi + has_hero;
                            if (gtypes[gi] == 1) { snprintf(g_cur_folder, 48, "%s", g_folders[gidxs[gi]]); g_shelf_sel = 0; }
                            else {
                                /* 點擊卡片右下 [詳情] 或點擊書本開啟 */
                                if (has_hero && ty >= h - 62 && tx >= cx + card_w - 64) open_book_style_modal(gidxs[gi]);
                                else { char t[384]; snprintf(t, sizeof(t), "%s", g_books[gidxs[gi]].path); reader_open_file(t); }
                            }
                            return;
                        }
                    }
                }
            }
            return;
        }
        if (g_home_tab == TAB_MANAGE) {
            int row_h = 68, top_y = 52, vis = (h - top_y - 34) / row_h;
            if (ty >= top_y) {
                int row = (ty - top_y) / row_h, idx = g_manage_scroll + row;
                if (idx >= 0 && idx < g_book_count && row < vis) {
                    if (tx >= mx + mw - 54) remove_book_at(idx);
                    else if (tx >= mx + mw - 104) { g_move_book_idx = idx; g_move_folder_sel = 0; g_screen = SCR_MOVE_FOLDER; }
                    else if (tx >= mx + mw - 166) open_book_style_modal(idx);
                    else { char t[384]; snprintf(t, sizeof(t), "%s", g_books[idx].path); reader_open_file(t); }
                }
            }
            return;
        }
        if (g_home_tab == TAB_STATS) {
            if (ty <= 46) {
                if (tx < mx + 490) { int st = (tx - mx) / 98; if (st >= 0 && st < 5) g_stats_subtab = st; return; }
                if (tx >= w - 150 && tx < w - 84) { if (g_stats_subtab == 4) step_report_day(-1); else if (g_stats_year > 2020) g_stats_year--; }
                else if (tx >= w - 80) { if (g_stats_subtab == 4) step_report_day(1); else if (g_stats_year < 2040) g_stats_year++; }
                return;
            }
            if (g_stats_subtab == 0) {
                int gx0 = mx + 36, gy0 = 186;
                if (tx >= gx0 && ty >= gy0 && ty < gy0 + 98) {
                    int col = (tx - gx0) / 12, row = (ty - gy0) / 14;
                    if (col >= 0 && col < 53 && row >= 0 && row < 7) {
                        int doy = col * 7 + row - stats_day_of_week(g_stats_year, 1, 1);
                        if (doy >= 0 && doy < stats_days_in_year(g_stats_year)) g_stats_doy = doy;
                    }
                }
            }
            return;
        }
        if (g_home_tab == TAB_ACHIEVE) {
            if (ty <= 46) { if (tx >= mx && tx < mx + 180) g_ach_subtab = 0; else if (tx >= mx + 186 && tx < mx + 380) g_ach_subtab = 1; return; }
            if (g_ach_subtab == 1 && ty >= 54 && ty <= 86) { int c = (tx - mx) / 88; if (c >= 0 && c < NUM_ACH_CATS) { g_ach_cat = c; g_ach_scroll = 0; } }
            return;
        }
        if (g_home_tab == TAB_APPEARANCE) {
            if (ty <= 46) {
                if (tx < mx + 180) g_appear_subtab = 0;
                else if (tx < mx + 380) g_appear_subtab = 1;
                else if (g_appear_subtab == 0 && tx >= w - 160) g_theme_page = (g_theme_page + 1) % 3;
                return;
            }
            if (g_appear_subtab == 0 && ty >= 76 && ty <= 236) {
                int gap = 14, cw = (mw - gap * 3) / 4;
                for (int i = 0; i < 4; i++) {
                    int cx = mx + i * (cw + gap);
                    if (tx >= cx && tx <= cx + cw) { g_theme_idx = g_theme_page * 4 + i; rpg_profile()->total_themes_tried++; save_library(); eval_rpg_and_toast(); return; }
                }
            } else if (g_appear_subtab == 1 && ty >= 58) {
                int row = (ty - 58) / 46;
                if (row >= 0 && row < 8) { g_set_row = row; reader_on_key((tx < mx + mw / 2) ? RKEY_LEFT : RKEY_RIGHT, 0); }
            }
            return;
        }
        return;
    }
    if (g_screen == SCR_READ_PDF || g_screen == SCR_READ_TEXT || g_screen == SCR_READ_IMG) {
        if (ty <= 36) {
            if (tx < 86) { save_bookmark(); save_manual_bookmarks(); g_screen = SCR_HOME; return; }
            if (g_screen == SCR_READ_PDF) {
                if (tx >= w - 252 && tx < w - 208) { reader_on_key(RKEY_FONT_DOWN, '-'); return; }
                if (tx >= w - 204 && tx < w - 160) { reader_on_key(RKEY_FONT_UP, '+'); return; }
                if (tx >= w - 156 && tx < w - 70)  { reader_on_key(RKEY_REFLOW, 'R'); return; }
            } else if (tx >= w - 150 && tx < w - 72) { reader_on_key(RKEY_AUTO, 'A'); return; }
            return;
        }
        if (ty >= h - 38) {
            if (tx >= w - 340) {
                int slot = (tx - (w - 340)) / 68;
                if (slot == 0) open_toc_screen();
                else if (slot == 1) g_screen = SCR_BOOKMARKS;
                else if (slot == 2) { g_search_ran = 0; g_screen = SCR_SEARCH; }
                else if (slot == 3) { g_jump_target = (g_screen == SCR_READ_PDF) ? g_pdf_page : g_current_page; g_screen = SCR_JUMP; }
                else { g_read_menu_sel = 0; g_screen = SCR_READ_MENU; }
            } else { g_read_menu_sel = 0; g_screen = SCR_READ_MENU; }
            return;
        }
        if (tx < (w * 30) / 100) reader_on_key(RKEY_PGUP, 0);
        else if (tx > (w * 70) / 100) reader_on_key(RKEY_PGDN, 0);
        else { g_read_menu_sel = 0; g_screen = SCR_READ_MENU; }
    }
}

static int calc_book_pct(const BookEntry *be) {
    if (be->page_count > 0) return ((be->page_idx + 1) * 100) / be->page_count;
    if (be->total_bytes > 0) return (int)((be->byte_offset * 100) / be->total_bytes);
    return 0;
}
static void draw_clock_and_goal_pill(const AppTheme *th, int rx_right, int y) {
    int y0, m0, d0, hh, mm, ss; hal_get_local_time(&y0, &m0, &d0, &hh, &mm, &ss);
    char clk[12]; snprintf(clk, sizeof(clk), "%02d:%02d", hh, mm);
    int cw = ui_ascii_width(clk, 1) + 14, x = rx_right - cw;
    ui_fill_round_rect(x, y, cw, 20, 8, th->card_bg); ui_draw_round_rect(x, y, cw, 20, 8, th->card_border);
    ui_draw_ascii(x + 7, y + 6, clk, 1, th->text);
    uint32_t ts = 0, tp = 0; stats_get_day(y0, m0, d0, &ts, &tp);
    if (ts >= (uint32_t)current_goal_mins() * 60u) ui_draw_badge(x - 42, y + 2, "GOAL", 0xF59E0B, 0xFFFFFF);
}
static void draw_book_cover_smart(int x, int y, int w, int h, const BookEntry *be, int is_dark) {
    if (be->cover_style == 0 && be->format == BOOK_FMT_EPUB) {
        int cw = 0, ch = 0; const uint8_t *rgb = get_cached_cover(be->path, &cw, &ch);
        if (rgb && cw > 0 && ch > 0) { ui_draw_book_cover_img(x, y, w, h, rgb, cw, ch, be->format, is_dark); return; }
    }
    ui_draw_styled_cover(x, y, w, h, be->title, be->format, be->cover_style, is_dark);
}
static void render_left_rail(const AppTheme *th, int h) {
    int rw = 76; hal_fill_rect(0, 0, rw, h, th->rail_bg); hal_fill_rect(rw, 0, 1, h, th->card_border);
    ui_fill_round_rect(19, 10, 38, 38, 10, th->accent); ui_draw_ascii(32, 20, "R", 2, 0xFFFFFF);
    const char *labels[6] = { "書架", "管理", "統計", "成就", "外觀", "匯入" };
    for (int i = 0; i < 6; i++) {
        int ty = 60 + i * 62, active = (i < 5 && (int)g_home_tab == i);
        uint32_t bg = active ? th->card_bg : th->rail_bg, fg = active ? th->accent : th->muted;
        if (active) { ui_fill_round_rect(8, ty, rw - 16, 54, 10, bg); hal_fill_rect(0, ty + 10, 4, 34, th->accent); }
        if (i == 0) ui_icon_shelf(rw / 2, ty + 16, fg);
        else if (i == 1) ui_icon_list(rw / 2, ty + 16, fg);
        else if (i == 2) ui_icon_stats(rw / 2, ty + 16, fg);
        else if (i == 3) ui_icon_trophy(rw / 2, ty + 16, fg);
        else if (i == 4) ui_icon_palette(rw / 2, ty + 16, fg);
        else ui_icon_import(rw / 2, ty + 16, fg);
        hal_set_text_bg(bg); draw_str(20, ty + 32, labels[i], 44, active ? th->text : th->muted);
        char kb[4]; snprintf(kb, sizeof(kb), "%d", i + 1); ui_draw_ascii(rw - 15, ty + 4, kb, 1, th->muted);
    }
    ui_draw_ascii(18, h - 18, "V9.0", 1, th->muted);
}

static void render_home_shelf(const AppTheme *th, int w, int h) {
    int mx = 96, mw = w - mx - 20;
    RpgSnapshot snap; build_rpg_snapshot(&snap); int cur_xp = 0, need_xp = 100;
    int lv = rpg_calc_level(rpg_calc_total_xp(&snap), &cur_xp, &need_xp);
    uint32_t today_s = 0, today_p = 0; stats_get_day(snap.cur_year, snap.cur_month, snap.cur_day, &today_s, &today_p);
    int gtypes[MAX_BOOKS + MAX_FOLDERS], gidxs[MAX_BOOKS + MAX_FOLDERS];
    int gcnt = shelf_build_grid(gtypes, gidxs, MAX_BOOKS + MAX_FOLDERS);

    /* A. 資料夾內部 (寬鬆 3x2 大卡片) */
    if (g_cur_folder[0] != '\0') {
        ui_fill_round_rect(mx, 12, 86, 30, 10, th->card_bg); ui_draw_round_rect(mx, 12, 86, 30, 10, th->card_border);
        hal_set_text_bg(th->card_bg); draw_str(mx + 12, 18, "< 返回", 68, th->text);
        hal_set_text_bg(th->bg); char fhdr[96]; snprintf(fhdr, sizeof(fhdr), "資料夾：%s (%d本)", g_cur_folder, gcnt);
        draw_str(mx + 100, 18, fhdr, 300, th->text);
        ui_fill_round_rect(w - 134, 12, 118, 30, 15, th->accent);
        hal_set_text_bg(th->accent); draw_str(w - 120, 18, "+ 匯入本書單", 104, 0xFFFFFF);

        int grid_y = 58, gap = 16, card_w = (mw - gap * 2) / 3, card_h = (h - grid_y - 34 - gap) / 2;
        int start_gi = (g_shelf_sel / 6) * 6;
        for (int slot = 0; slot < 6; slot++) {
            int gi = start_gi + slot; if (gi >= gcnt) break;
            int row = slot / 3, col = slot % 3, cx = mx + col * (card_w + gap), cy0 = grid_y + row * (card_h + gap);
            BookEntry *be = &g_books[gidxs[gi]];
            ui_card(cx, cy0, card_w, card_h, 14, th->card_bg, th->card_border, th->shadow, (gi == g_shelf_sel), th->accent);
            int cov_w = 72, cov_h = card_h - 24; if (cov_h > 106) cov_h = 106;
            draw_book_cover_smart(cx + 12, cy0 + 12, cov_w, cov_h, be, th->is_dark);
            int tx = cx + cov_w + 22, tw = card_w - cov_w - 30;
            hal_set_text_bg(th->card_bg); char st[64]; utf8_ellipsis(be->title, tw / 9, st, sizeof(st));
            draw_str(tx, cy0 + 14, st, tw, th->text);
            ui_draw_star_row(tx, cy0 + 42, be->rating, 5, 13, 0xF59E0B, th->track);
            char meta[32]; snprintf(meta, sizeof(meta), "%s · %d%%", STATUS_NM[be->read_status & 3], calc_book_pct(be));
            draw_str(tx, cy0 + 62, meta, tw, th->muted);
            ui_progress_bar(tx, cy0 + card_h - 22, tw, 5, calc_book_pct(be), th->track, ui_format_color(be->format));
        }
        hal_set_text_bg(th->bg); draw_str(mx, h - 22, "[Enter]閱讀  [S]書籍詳情與星星  [M]移出  [D]刪除  [ESC]返回", mw, th->muted);
        return;
    }

    /* B. 極簡現代頂部列 (不擁擠，把10顆篩選膠囊收進 [篩選] 彈窗!) */
    hal_set_text_bg(th->bg); char hdr[128];
    snprintf(hdr, sizeof(hdr), "Lv.%d %s · 今日%lu/%d分", lv, rpg_level_title(lv), (unsigned long)(today_s / 60u), current_goal_mins());
    draw_str(mx, 16, hdr, 300, th->text);

    int has_filter = (g_filter_fmt != 0 || g_filter_status != 0);
    uint32_t fbg = has_filter ? 0x10B981 : th->card_bg;
    ui_fill_round_rect(w - 392, 12, 72, 28, 14, fbg);
    if (!has_filter) ui_draw_round_rect(w - 392, 12, 72, 28, 14, th->card_border);
    hal_set_text_bg(fbg); draw_str(w - 378, 17, has_filter ? "已篩選" : "篩選", 52, has_filter ? 0xFFFFFF : th->text);

    ui_fill_round_rect(w - 312, 12, 76, 28, 14, th->card_bg); ui_draw_round_rect(w - 312, 12, 76, 28, 14, th->card_border);
    hal_set_text_bg(th->card_bg); draw_str(w - 300, 17, "日總結", 56, th->text);

    ui_fill_round_rect(w - 228, 12, 86, 28, 14, th->card_bg); ui_draw_round_rect(w - 228, 12, 86, 28, 14, th->accent);
    draw_str(w - 216, 17, "+ 資料夾", 68, th->accent);

    ui_fill_round_rect(w - 134, 12, 118, 28, 14, th->accent);
    hal_set_text_bg(th->accent); draw_str(w - 118, 17, "+ 匯入新書", 96, 0xFFFFFF);

    int has_hero = (!has_filter && g_book_count > 0) ? 1 : 0;
    if (g_book_count == 0 && g_folder_count == 0) {
        int cw = mw - 12, ch = h - 100, cx = mx + 6, cy0 = 60;
        ui_card(cx, cy0, cw, ch, 16, th->card_bg, th->card_border, th->shadow, 0, th->accent);
        ui_icon_open_book(cx + cw / 2, cy0 + 78, th->accent, th->bg, th->card_border);
        hal_set_text_bg(th->card_bg);
        draw_str(cx + cw / 2 - 148, cy0 + 132, "歡迎來到 CD-952 現代化個人書房 v9", 360, th->text);
        draw_str(cx + 48, cy0 + 166, almanac_daily_quote(snap.cur_year, snap.cur_month, snap.cur_day), cw - 96, th->accent);
        int bw = 240, bh = 42, bx = cx + (cw - bw) / 2, by = cy0 + 224;
        ui_fill_round_rect(bx, by, bw, bh, 21, th->accent); hal_set_text_bg(th->accent);
        draw_str(bx + 26, by + 12, "點擊或按 [Enter] 匯入新書", 200, 0xFFFFFF);
        return;
    }

    /* C. 寬敞現代「繼續閱讀」Hero 大卡片 (帶金色星星 + 詳情按鈕) */
    if (has_hero) {
        BookEntry *hero = &g_books[0]; int hy = 56, hh = 114;
        ui_card(mx, hy, mw, hh, 16, th->card_bg, th->card_border, th->shadow, (g_shelf_sel == 0), th->accent);
        draw_book_cover_smart(mx + 14, hy + 11, 72, 92, hero, th->is_dark);

        int tx = mx + 102, tw = mw - 244; hal_set_text_bg(th->card_bg);
        char ht[96]; utf8_ellipsis(hero->title, tw / 9, ht, sizeof(ht));
        draw_str(tx, hy + 14, ht, tw, th->text);

        /* 金色立體像素星星 + 閱讀狀態 */
        ui_draw_star_row(tx, hy + 42, hero->rating, 5, 14, 0xF59E0B, th->track);
        char sub[96], bdur[24]; format_duration_zh(hero->read_secs, bdur, sizeof(bdur));
        snprintf(sub, sizeof(sub), "%s · 已讀%s", STATUS_NM[hero->read_status & 3], bdur);
        draw_str(tx + 80, hy + 38, sub, tw - 80, th->muted);

        int hpct = calc_book_pct(hero);
        ui_progress_bar(tx, hy + 68, tw - 10, 7, hpct, th->track, th->accent);
        char pinfo[80];
        if (hero->page_count > 0) snprintf(pinfo, sizeof(pinfo), "閱讀進度 %d%% · 第 %d / %d 頁", hpct, hero->page_idx + 1, hero->page_count);
        else snprintf(pinfo, sizeof(pinfo), "圖片書模式 · 點擊開始翻閱");
        draw_str(tx, hy + 84, pinfo, tw, th->muted);

        int cta_x = mx + mw - 126;
        uint32_t cbg = (g_shelf_sel == 0) ? th->accent : ui_blend_color(th->card_bg, th->accent, 85);
        ui_fill_round_rect(cta_x, hy + 18, 110, 36, 18, cbg); hal_set_text_bg(cbg);
        draw_str(cta_x + 19, hy + 27, "繼續閱讀", 76, 0xFFFFFF);

        ui_fill_round_rect(cta_x, hy + 64, 110, 32, 16, th->bg); ui_draw_round_rect(cta_x, hy + 64, 110, 32, 16, th->card_border);
        hal_set_text_bg(th->bg); draw_str(cta_x + 22, hy + 71, "書籍詳情", 76, th->text);
    }

    /* D. 寬鬆 3 欄大卡片網格 (大幅增加留白、更大封面、每本書皆顯示金色星星!) */
    int grid_y = has_hero ? 204 : 60;
    if (has_hero) {
        hal_set_text_bg(th->bg);
        draw_str(mx, 180, g_toast_msg[0] ? g_toast_msg : "我的藏書與資料夾 (點書閱讀 · 按 [S] 或點右下開詳情頁)", mw, g_toast_msg[0] ? th->accent : th->muted);
    }

    int gap = 16, card_w = (mw - gap * 2) / 3, rows_show = has_hero ? 1 : 2;
    int card_h = has_hero ? (h - grid_y - 30) : ((h - grid_y - 34 - gap) / 2);
    if (card_h > 240) card_h = 240;
    int grid_sel = (g_shelf_sel >= has_hero) ? (g_shelf_sel - has_hero) : 0;
    int per_pg = rows_show * 3, start_gi = (grid_sel / per_pg) * per_pg;

    for (int slot = 0; slot < per_pg; slot++) {
        int gi = start_gi + slot; if (gi >= gcnt) break;
        int row = slot / 3, col = slot % 3;
        int cx = mx + col * (card_w + gap), cy0 = grid_y + row * (card_h + gap);
        ui_card(cx, cy0, card_w, card_h, 14, th->card_bg, th->card_border, th->shadow, (g_shelf_sel == gi + has_hero), th->accent);

        if (gtypes[gi] == 1) {
            int cov_h = card_h - 68; if (cov_h < 68) cov_h = 68;
            const char *fn = g_folders[gidxs[gi]]; int fcnt = folder_count_books(fn), ff = folder_first_book_idx(fn), tw = 0, thh = 0;
            const uint8_t *trgb = (ff >= 0 && g_books[ff].format == BOOK_FMT_EPUB) ? get_cached_cover(g_books[ff].path, &tw, &thh) : NULL;
            ui_draw_folder_card(cx + 14, cy0 + 12, card_w - 28, cov_h, trgb, tw, thh, fcnt, th->accent, th->is_dark);
            hal_set_text_bg(th->card_bg); char fs[64]; utf8_ellipsis(fn, (card_w - 28) / 9, fs, sizeof(fs));
            draw_str(cx + 14, cy0 + cov_h + 20, fs, card_w - 28, th->text);
            char fsub[32]; snprintf(fsub, sizeof(fsub), "資料夾 · %d 本書", fcnt);
            draw_str(cx + 14, cy0 + card_h - 24, fsub, card_w - 28, th->muted);
        } else {
            BookEntry *be = &g_books[gidxs[gi]];
            if (has_hero) {
                /* 單排大卡片：上方大精裝封面 + 下方書名 + 金色星星 + 進度條 + 詳情膠囊 */
                int cov_h = card_h - 86; if (cov_h < 90) cov_h = 90;
                draw_book_cover_smart(cx + 18, cy0 + 12, card_w - 36, cov_h, be, th->is_dark);
                hal_set_text_bg(th->card_bg); char st[64]; utf8_ellipsis(be->title, (card_w - 28) / 9, st, sizeof(st));
                draw_str(cx + 14, cy0 + cov_h + 18, st, card_w - 28, th->text);
                ui_draw_star_row(cx + 14, cy0 + cov_h + 44, be->rating, 5, 13, 0xF59E0B, th->track);
                ui_fill_round_rect(cx + card_w - 58, cy0 + cov_h + 40, 46, 20, 7, th->bg);
                ui_draw_round_rect(cx + card_w - 58, cy0 + cov_h + 40, 46, 20, 7, th->card_border);
                hal_set_text_bg(th->bg); draw_str(cx + card_w - 50, cy0 + cov_h + 41, "詳情", 36, th->muted);
                ui_progress_bar(cx + 14, cy0 + card_h - 14, card_w - 28, 5, calc_book_pct(be), th->track, ui_format_color(be->format));
            } else {
                /* 篩選模式下 3x2 橫式大卡片 */
                int cov_w = 72, cov_h = card_h - 24; if (cov_h > 106) cov_h = 106;
                draw_book_cover_smart(cx + 12, cy0 + 12, cov_w, cov_h, be, th->is_dark);
                int tx = cx + cov_w + 22, tw = card_w - cov_w - 30;
                hal_set_text_bg(th->card_bg); char st[64]; utf8_ellipsis(be->title, tw / 9, st, sizeof(st));
                draw_str(tx, cy0 + 14, st, tw, th->text);
                ui_draw_star_row(tx, cy0 + 42, be->rating, 5, 13, 0xF59E0B, th->track);
                char meta[32]; snprintf(meta, sizeof(meta), "%s · %d%%", STATUS_NM[be->read_status & 3], calc_book_pct(be));
                draw_str(tx, cy0 + 62, meta, tw, th->muted);
                ui_progress_bar(tx, cy0 + card_h - 22, tw, 5, calc_book_pct(be), th->track, ui_format_color(be->format));
            }
        }
    }
    hal_set_text_bg(th->bg); draw_str(mx, h - 22, "[1]書架 [2]管理 [3]統計/雷達 [4]成就 [5]外觀 [6/O]匯入 [S]詳情", mw, th->muted);
}

static void render_home_manage(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18; hal_set_text_bg(th->bg);
    draw_str(mx, 16, "書庫管理 (點 [封/狀] 換24款封面與評分 · [移]資料夾 · [刪]移出)", mw, th->text);
    if (g_book_count == 0) { draw_str(mx, 80, "目前沒有藏書，按 [O] 匯入新書。", mw, th->muted); return; }
    int row_h = 68, top_y = 52, vis = (h - top_y - 34) / row_h; if (vis < 1) vis = 1;
    if (g_shelf_sel < g_manage_scroll) g_manage_scroll = g_shelf_sel;
    if (g_shelf_sel >= g_manage_scroll + vis) g_manage_scroll = g_shelf_sel - vis + 1;
    for (int i = g_manage_scroll; i < g_book_count && i < g_manage_scroll + vis; i++) {
        BookEntry *be = &g_books[i]; int ry = top_y + (i - g_manage_scroll) * row_h;
        ui_card(mx, ry, mw, row_h - 8, 10, th->card_bg, th->card_border, th->shadow, (i == g_shelf_sel), th->accent);
        ui_draw_badge(mx + 10, ry + 10, book_format_tag(be->format), ui_format_color(be->format), 0xFFFFFF);
        hal_set_text_bg(th->card_bg); char tbuf[80]; utf8_ellipsis(be->title, (mw - 250) / 9, tbuf, sizeof(tbuf));
        draw_str(mx + 64, ry + 8, tbuf, mw - 250, th->text);
        char sub[140], dstr[24]; format_duration_zh(be->read_secs, dstr, sizeof(dstr));
        ui_draw_star_row(mx + 12, ry + 36, be->rating, 5, 12, 0xF59E0B, th->track);
        snprintf(sub, sizeof(sub), "[%s] · 封面#%02d · 已讀%s", STATUS_NM[be->read_status & 3], be->cover_style, dstr);
        draw_str(mx + 82, ry + 34, sub, mw - 260, th->muted);
        ui_fill_round_rect(mx + mw - 166, ry + 14, 56, 30, 8, 0x059669); hal_set_text_bg(0x059669); draw_str(mx + mw - 156, ry + 20, "詳情", 42, 0xFFFFFF);
        ui_fill_round_rect(mx + mw - 104, ry + 14, 44, 30, 8, th->accent); hal_set_text_bg(th->accent); draw_str(mx + mw - 92, ry + 20, "移", 24, 0xFFFFFF);
        ui_fill_round_rect(mx + mw - 54, ry + 14, 44, 30, 8, 0xEF4444); hal_set_text_bg(0xEF4444); draw_str(mx + mw - 42, ry + 20, "刪", 24, 0xFFFFFF);
    }
    hal_set_text_bg(th->bg); draw_str(mx, h - 22, "[S]封面與狀態 [M]移動資料夾 [D]刪除 [Enter]開啟閱讀", mw, th->muted);
}

static void render_daily_report_body(const AppTheme *th, int x, int y, int w, int h) {
    uint32_t ds = 0, dp = 0; stats_get_day(g_report_y, g_report_m, g_report_d, &ds, &dp);
    int goal_m = current_goal_mins(), mins = (int)(ds / 60u);
    int raw_pct = (goal_m > 0) ? (int)((ds * 100u) / ((uint32_t)goal_m * 60u)) : 0;
    int pct = raw_pct > 100 ? 100 : raw_pct;
    int est_words = (int)dp * 380, speed_wpm = (ds >= 30u) ? (int)(((uint64_t)est_words * 60u) / ds) : 0;
    int xp_today = mins * 10 + (int)dp * 2, streak = stats_calc_streak(g_report_y, g_report_m, g_report_d);
    const char *grade = "C", *comment = "今天還沒翻書喔，現在打開讀 5 分鐘開啟好習慣！"; uint32_t gcol = 0x64748B;
    if (raw_pct >= 150) { grade = "S+"; gcol = 0xF59E0B; comment = "超凡專注！遠超今日目標，傳奇書神正在養成！"; }
    else if (raw_pct >= 100) { grade = "S"; gcol = 0x10B981; comment = "完美達成今日閱讀目標！保持連勝節奏！"; }
    else if (raw_pct >= 60)  { grade = "A"; gcol = 0x3B82F6; comment = "表現很棒！離今日目標只差最後衝刺了！"; }
    else if (ds >= 60u || dp > 0) { grade = "B"; gcol = 0x8B5CF6; comment = "不錯的開始！再讀一會兒就能點亮今日目標！"; }

    ui_card(x + 16, y + 14, 84, 84, 14, gcol, gcol, th->shadow, 0, 0);
    ui_draw_ascii(x + 16 + (84 - ui_ascii_width(grade, 3)) / 2, y + 44, grade, 3, 0xFFFFFF);
    hal_set_text_bg(th->card_bg);
    const char *wd[7] = { "週日", "週一", "週二", "週三", "週四", "週五", "週六" };
    char dtitle[96]; snprintf(dtitle, sizeof(dtitle), "%04d-%02d-%02d (%s) 每日閱讀總結", g_report_y, g_report_m, g_report_d, wd[stats_day_of_week(g_report_y, g_report_m, g_report_d)]);
    draw_str(x + 114, y + 18, dtitle, w - 130, th->text);
    draw_str(x + 114, y + 46, comment, w - 130, th->accent);
    ui_progress_bar(x + 114, y + 78, w - 134, 12, pct, th->track, gcol);

    char v[4][40]; format_duration_zh(ds, v[0], sizeof(v[0])); snprintf(v[1], sizeof(v[1]), "%lu 頁", (unsigned long)dp);
    snprintf(v[2], sizeof(v[2]), "約 %d 字", est_words); snprintf(v[3], sizeof(v[3]), "+%d XP", xp_today);
    const char *l[4] = { "當日閱讀時長", "當日翻閱頁數", "預估閱讀字數", "當日獲得經驗" };
    int kw = (w - 62) / 4;
    for (int i = 0; i < 4; i++) {
        int kx = x + 16 + i * (kw + 10);
        ui_fill_round_rect(kx, y + 114, kw, 70, 10, th->bg); hal_set_text_bg(th->bg);
        draw_str(kx + 10, y + 124, l[i], kw - 16, th->muted); draw_str(kx + 10, y + 152, v[i], kw - 16, th->text);
    }
    ui_fill_round_rect(x + 16, y + 198, w - 32, h - 214, 10, th->bg); hal_set_text_bg(th->bg);
    int fin = 0; for (int i = 0; i < g_book_count; i++) if (g_books[i].read_status == 2) fin++;
    char s1[128], s2[128];
    snprintf(s1, sizeof(s1), "目標達成率：%d%% (目標 %d 分鐘)   ·   連續打卡：%d 天", raw_pct, goal_m, streak);
    snprintf(s2, sizeof(s2), "平均閱讀速度：%d 字/分   ·   累計完食：%d / %d 本", speed_wpm, fin, g_book_count);
    draw_str(x + 28, y + 210, s1, w - 56, th->text); draw_str(x + 28, y + 240, s2, w - 56, th->muted);
}

static void render_home_stats(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    const char *stabs[5] = { "365熱力", "7天柱狀", "12月趨勢", "五維雷達", "每日總結" };
    for (int i = 0; i < 5; i++) {
        int tx = mx + i * 98; uint32_t bg = (g_stats_subtab == i) ? th->accent : th->card_bg;
        ui_fill_round_rect(tx, 10, 92, 30, 8, bg); if (g_stats_subtab != i) ui_draw_round_rect(tx, 10, 92, 30, 8, th->card_border);
        hal_set_text_bg(bg); draw_str(tx + 10, 16, stabs[i], 76, (g_stats_subtab == i) ? 0xFFFFFF : th->text);
    }
    ui_fill_round_rect(w - 148, 10, 64, 30, 8, th->card_bg); ui_draw_round_rect(w - 148, 10, 64, 30, 8, th->card_border);
    hal_set_text_bg(th->card_bg); draw_str(w - 140, 16, g_stats_subtab == 4 ? "<前日" : "<去年", 52, th->text);
    ui_fill_round_rect(w - 80, 10, 64, 30, 8, th->card_bg); ui_draw_round_rect(w - 80, 10, 64, 30, 8, th->card_border);
    draw_str(w - 72, 16, g_stats_subtab == 4 ? "後日>" : "明年>", 52, th->text);
    int cy, cm, cd, hh, mm, ss; hal_get_local_time(&cy, &cm, &cd, &hh, &mm, &ss);
    if (g_stats_subtab == 0) {
        uint32_t ts = 0, tp = 0; stats_get_day(cy, cm, cd, &ts, &tp);
        int streak = stats_calc_streak(cy, cm, cd), yr_d = stats_year_active_days(g_stats_year), tot_d = stats_days_in_year(g_stats_year);
        uint32_t yr_s = stats_year_total_secs(g_stats_year);
        char kv[4][36]; format_duration_zh(ts, kv[0], sizeof(kv[0])); snprintf(kv[1], sizeof(kv[1]), "%d 天", streak);
        format_duration_zh(yr_s, kv[2], sizeof(kv[2])); snprintf(kv[3], sizeof(kv[3]), "%d/%d天", yr_d, tot_d);
        const char *kl[4] = { "今日閱讀", "連續打卡", "今年累計", "打卡天數" };
        int kw = (mw - 36) / 4;
        for (int i = 0; i < 4; i++) {
            int kx = mx + i * (kw + 12); ui_card(kx, 52, kw, 66, 12, th->card_bg, th->card_border, th->shadow, 0, th->accent);
            hal_set_text_bg(th->card_bg); draw_str(kx + 12, 60, kl[i], kw - 20, th->muted); draw_str(kx + 12, 86, kv[i], kw - 20, (i == 1 && streak > 0) ? th->accent : th->text);
        }
        int hm_y = 132, hm_h = 204; ui_card(mx, hm_y, mw, hm_h, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent);
        hal_set_text_bg(th->card_bg); char ht[64]; snprintf(ht, sizeof(ht), "%d 年 365 天閱讀熱力圖 (點格查當天)", g_stats_year);
        draw_str(mx + 14, hm_y + 10, ht, mw - 40, th->text);
        uint32_t lv_c[5] = { th->track, ui_blend_color(th->track, th->accent, 30), ui_blend_color(th->track, th->accent, 55), ui_blend_color(th->track, th->accent, 78), th->accent };
        int gx0 = mx + 36, gy0 = 186, jan1 = stats_day_of_week(g_stats_year, 1, 1);
        for (int m = 1; m <= 12; m++) { int col = (stats_day_of_year(g_stats_year, m, 1) + jan1) / 7; char ml[8]; snprintf(ml, sizeof(ml), "%dM", m); ui_draw_ascii(gx0 + col * 12, gy0 - 13, ml, 1, th->muted); }
        for (int doy = 0; doy < tot_d; doy++) {
            int sl = doy + jan1, col = sl / 7, row = sl % 7, cx = gx0 + col * 12, cyc = gy0 + row * 14;
            if (cx + 10 > mx + mw - 8) break;
            int dm = 1, dd = 1; stats_doy_to_ymd(g_stats_year, doy, &dm, &dd);
            uint32_t ds = 0, dp = 0; stats_get_day(g_stats_year, dm, dd, &ds, &dp);
            int lv = (ds >= 3600u) ? 4 : (ds >= 1800u ? 3 : (ds >= 900u ? 2 : (ds >= 10u || dp > 0 ? 1 : 0)));
            hal_fill_rect(cx, cyc, 9, 10, lv_c[lv]);
            if (doy == g_stats_doy) hal_draw_rect(cx - 1, cyc - 1, 11, 12, th->is_dark ? 0xFFFFFF : 0x111111);
        }
        int sm = 1, sd = 1; stats_doy_to_ymd(g_stats_year, g_stats_doy, &sm, &sd);
        uint32_t ss0 = 0, sp0 = 0; stats_get_day(g_stats_year, sm, sd, &ss0, &sp0);
        ui_fill_round_rect(mx + 14, hm_y + hm_h - 40, mw - 28, 28, 8, th->bg); hal_set_text_bg(th->bg);
        char det[128], sdur[24]; format_duration_zh(ss0, sdur, sizeof(sdur));
        snprintf(det, sizeof(det), "選中：%04d-%02d-%02d · 閱讀 %s · 翻頁 %lu 頁", g_stats_year, sm, sd, sdur, (unsigned long)sp0);
        draw_str(mx + 24, hm_y + hm_h - 34, det, mw - 48, th->text);
    } else if (g_stats_subtab == 1) {
        uint32_t dsec[7], dpg[7]; char dlbl[7][8]; stats_get_last_7_days(cy, cm, cd, dsec, dpg, dlbl);
        int goal_m = current_goal_mins(), max_m = goal_m; uint32_t sum_s = 0, sum_p = 0;
        for (int i = 0; i < 7; i++) { int m = (int)(dsec[i] / 60u); if (m > max_m) max_m = m; sum_s += dsec[i]; sum_p += dpg[i]; }
        if (max_m < 10) max_m = 10;
        ui_card(mx, 52, mw, h - 84, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent); hal_set_text_bg(th->card_bg);
        char title[128], sd[24]; format_duration_zh(sum_s, sd, sizeof(sd));
        snprintf(title, sizeof(title), "近 7 天閱讀柱狀圖 · 7日合計 %s (%lu頁) · 目標 %d分/天", sd, (unsigned long)sum_p, goal_m);
        draw_str(mx + 18, 66, title, mw - 36, th->text);
        int base_y = h - 76, chart_h = 220; hal_fill_rect(mx + 30, base_y, mw - 60, 2, th->card_border);
        int gy = base_y - (goal_m * chart_h) / max_m;
        for (int gx = mx + 30; gx < mx + mw - 36; gx += 12) hal_fill_rect(gx, gy, 7, 2, 0xF59E0B);
        ui_draw_ascii(mx + mw - 76, gy - 10, "GOAL", 1, 0xF59E0B);
        int col_w = (mw - 80) / 7;
        for (int i = 0; i < 7; i++) {
            int mins = (int)(dsec[i] / 60u), bh = (mins * chart_h) / max_m; if (dsec[i] > 0 && bh < 6) bh = 6;
            int bx = mx + 44 + i * col_w, bw = col_w - 28; if (bw < 24) bw = 24;
            if (bh > 0) ui_fill_round_rect(bx, base_y - bh, bw, bh, 6, (mins >= goal_m) ? 0x10B981 : th->accent);
            char mv[12]; snprintf(mv, sizeof(mv), "%dM", mins);
            ui_draw_ascii(bx + (bw - ui_ascii_width(mv, 1)) / 2, base_y - bh - 12, mv, 1, th->text);
            ui_draw_ascii(bx + (bw - ui_ascii_width(dlbl[i], 1)) / 2, base_y + 10, dlbl[i], 1, th->muted);
        }
    } else if (g_stats_subtab == 2) {
        uint32_t msec[12], mpg[12]; stats_get_12_months(g_stats_year, msec, mpg);
        int max_m = 30; uint32_t yr_s = 0;
        for (int i = 0; i < 12; i++) { int m = (int)(msec[i] / 60u); if (m > max_m) max_m = m; yr_s += msec[i]; }
        ui_card(mx, 52, mw, h - 84, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent); hal_set_text_bg(th->card_bg);
        char title[128], sd[24]; format_duration_zh(yr_s, sd, sizeof(sd));
        snprintf(title, sizeof(title), "%d 年 12 個月閱讀趨勢圖 · 全年總計 %s", g_stats_year, sd);
        draw_str(mx + 18, 66, title, mw - 36, th->text);
        int base_y = h - 76, chart_h = 220; hal_fill_rect(mx + 26, base_y, mw - 52, 2, th->card_border);
        int step_x = (mw - 68) / 12, prev_cx = -1, prev_cy = -1;
        for (int i = 0; i < 12; i++) {
            int mins = (int)(msec[i] / 60u), bh = (mins * chart_h) / max_m; if (msec[i] > 0 && bh < 5) bh = 5;
            int bx = mx + 36 + i * step_x, bw = step_x - 12; if (bw < 14) bw = 14;
            int cx = bx + bw / 2, cy_pt = base_y - bh;
            if (bh > 0) ui_fill_round_rect(bx, cy_pt, bw, bh, 4, ui_blend_color(th->track, th->accent, 65));
            if (prev_cx >= 0) hal_draw_line(prev_cx, prev_cy, cx, cy_pt, 0xF59E0B);
            hal_fill_circle(cx, cy_pt, 4, 0xF59E0B); prev_cx = cx; prev_cy = cy_pt;
            char ml[8]; snprintf(ml, sizeof(ml), "%dM", i + 1); ui_draw_ascii(cx - ui_ascii_width(ml, 1) / 2, base_y + 10, ml, 1, th->muted);
            if (mins > 0) { char mv[12]; snprintf(mv, sizeof(mv), "%d", mins); ui_draw_ascii(cx - ui_ascii_width(mv, 1) / 2, cy_pt - 14, mv, 1, th->text); }
        }
    } else if (g_stats_subtab == 3) {
        /* 五維閱讀能力雷達圖 (專注時長 / 翻頁速度 / 打卡毅力 / 藏書廣度 / 完食率) */
        RpgSnapshot snap; build_rpg_snapshot(&snap);
        int s[5];
        int mins = (int)(snap.lifetime_secs / 60u);
        s[0] = (mins * 100) / 300; if (snap.lifetime_secs > 0 && s[0] < 15) s[0] = 15; if (s[0] > 100) s[0] = 100;
        s[1] = (int)((snap.lifetime_pages * 100u) / 500u); if (snap.lifetime_pages > 0 && s[1] < 15) s[1] = 15; if (s[1] > 100) s[1] = 100;
        s[2] = snap.streak * 12 + snap.lifetime_days * 4; if (snap.lifetime_days > 0 && s[2] < 15) s[2] = 15; if (s[2] > 100) s[2] = 100;
        int fmts = (snap.epub_count > 0) + (snap.pdf_count > 0) + (snap.txt_count > 0) + (snap.img_count > 0);
        s[3] = snap.book_count * 7 + fmts * 12; if (snap.book_count > 0 && s[3] < 18) s[3] = 18; if (s[3] > 100) s[3] = 100;
        s[4] = (snap.book_count > 0 ? (snap.finished_books * 85) / snap.book_count : 0) + rpg_count_unlocked() * 2;
        if (s[4] > 100) s[4] = 100;
        if (s[4] < 12 && (snap.book_count > 0 || rpg_count_unlocked() > 0)) s[4] = 12;

        int avg = (s[0] + s[1] + s[2] + s[3] + s[4]) / 5;
        const char *rank = (avg >= 85) ? "S+" : (avg >= 70 ? "S" : (avg >= 50 ? "A" : (avg >= 30 ? "B" : "C")));
        uint32_t rcol = (avg >= 70) ? 0xF59E0B : (avg >= 50 ? 0x10B981 : th->accent);

        ui_card(mx, 52, mw, h - 84, 16, th->card_bg, th->card_border, th->shadow, 0, th->accent);

        /* 左半部：五邊形雷達圖 + 5 軸標籤 */
        int rcx = mx + 178, rcy = 246, rad = 102;
        ui_draw_radar_5(rcx, rcy, rad, s, th->card_border, th->accent, th->accent, th->card_bg);

        hal_set_text_bg(th->card_bg);
        draw_str(rcx - 32, rcy - rad - 24, "專注時長", 76, th->text);
        draw_str(rcx + rad - 10, rcy - 42, "翻頁速度", 76, th->text);
        draw_str(rcx + 36, rcy + rad - 10, "打卡毅力", 76, th->text);
        draw_str(rcx - 106, rcy + rad - 10, "藏書廣度", 76, th->text);
        draw_str(rcx - rad - 68, rcy - 42, "完食成就", 76, th->text);

        /* 右半部：閱讀人格稱號 + 綜合評級 + 五維進度條 */
        int rx = mx + 364, rw = mw - 380;
        ui_fill_round_rect(rx, 68, rw, 78, 12, th->bg);
        ui_fill_round_rect(rx + 12, 80, 54, 54, 12, rcol);
        ui_draw_ascii(rx + 12 + (54 - ui_ascii_width(rank, 2)) / 2, 100, rank, 2, 0xFFFFFF);
        hal_set_text_bg(th->bg);
        draw_str(rx + 78, 82, almanac_radar_archetype(s), rw - 88, th->text);
        char csc[48]; snprintf(csc, sizeof(csc), "五維綜合戰力：%d / 100", avg);
        draw_str(rx + 78, 110, csc, rw - 88, th->accent);

        const char *dims[5] = { "1. 專注時長", "2. 翻頁速度", "3. 打卡毅力", "4. 藏書廣度", "5. 完食成就" };
        uint32_t dcols[5] = { 0x3B82F6, 0x10B981, 0xF59E0B, 0x8B5CF6, 0xEC4899 };
        hal_set_text_bg(th->card_bg);
        for (int i = 0; i < 5; i++) {
            int dy = 164 + i * 48;
            char dl[48]; snprintf(dl, sizeof(dl), "%s  (%d分)", dims[i], s[i]);
            draw_str(rx, dy, dl, rw, th->text);
            ui_progress_bar(rx, dy + 24, rw - 8, 9, s[i], th->track, dcols[i]);
        }
    } else {
        ui_card(mx, 52, mw, h - 84, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent);
        render_daily_report_body(th, mx, 52, mw, h - 84);
    }
    hal_set_text_bg(th->bg); draw_str(mx, h - 22, "[空白/點擊] 切換圖表與每日報告  ·  [←→] 切換日期/年份", mw, th->muted);
}

static void render_home_achieve(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    RpgSnapshot snap; build_rpg_snapshot(&snap); int cur_xp = 0, need_xp = 100;
    uint32_t tot_xp = rpg_calc_total_xp(&snap); int lv = rpg_calc_level(tot_xp, &cur_xp, &need_xp), unl = rpg_count_unlocked();
    const char *atabs[2] = { "等級 & 每週隨機任務", "60 成就獎盃牆" };
    for (int i = 0; i < 2; i++) {
        int tx = mx + i * 188; uint32_t bg = (g_ach_subtab == i) ? th->accent : th->card_bg;
        ui_fill_round_rect(tx, 10, 180, 30, 8, bg); if (g_ach_subtab != i) ui_draw_round_rect(tx, 10, 180, 30, 8, th->card_border);
        hal_set_text_bg(bg); draw_str(tx + 18, 16, atabs[i], 150, (g_ach_subtab == i) ? 0xFFFFFF : th->text);
    }
    hal_set_text_bg(th->bg); char cnt_s[48]; snprintf(cnt_s, sizeof(cnt_s), "已解鎖 %d / %d 獎盃", unl, NUM_ACHIEVEMENTS);
    draw_str(w - 190, 16, cnt_s, 176, th->accent);

    if (g_ach_subtab == 0) {
        ui_card(mx, 52, mw, 118, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
        ui_fill_round_rect(mx + 16, 68, 86, 86, 16, th->accent);
        char lv_s[16]; snprintf(lv_s, sizeof(lv_s), "LV%d", lv);
        ui_draw_ascii(mx + 16 + (86 - ui_ascii_width(lv_s, 2)) / 2, 102, lv_s, 2, 0xFFFFFF);
        hal_set_text_bg(th->card_bg);
        char t1[96], t2[96];
        snprintf(t1, sizeof(t1), "閱讀者稱號：%s (Lv.%d)", rpg_level_title(lv), lv);
        snprintf(t2, sizeof(t2), "總經驗值 %lu XP · 距離下一級還需 %d XP", (unsigned long)tot_xp, need_xp - cur_xp);
        draw_str(mx + 118, 66, t1, mw - 134, th->text); draw_str(mx + 118, 94, t2, mw - 134, th->muted);
        ui_progress_bar(mx + 118, 128, mw - 140, 12, need_xp > 0 ? (cur_xp * 100) / need_xp : 100, th->track, th->accent);

        hal_set_text_bg(th->bg); draw_str(mx, 182, "本週隨機閱讀任務 (每週自動刷新 3 項 · 完成獲額外 XP)：", mw, th->text);
        WeeklyQuestView wq[NUM_WEEKLY_QUESTS]; rpg_get_weekly_quests(&snap, wq);
        for (int i = 0; i < NUM_WEEKLY_QUESTS; i++) {
            int qy = 208 + i * 78;
            ui_card(mx, qy, mw, 68, 12, th->card_bg, th->card_border, th->shadow, wq[i].done, 0x10B981);
            hal_set_text_bg(th->card_bg);
            char qt[128]; snprintf(qt, sizeof(qt), "%d. %s (%d/%d) · +%d XP", i + 1, wq[i].title, wq[i].cur > wq[i].target ? wq[i].target : wq[i].cur, wq[i].target, wq[i].xp);
            draw_str(mx + 16, qy + 12, qt, mw - 110, th->text);
            int qpct = wq[i].target > 0 ? (wq[i].cur * 100) / wq[i].target : 0; if (qpct > 100) qpct = 100;
            ui_progress_bar(mx + 16, qy + 44, mw - 110, 8, qpct, th->track, wq[i].done ? 0x10B981 : th->accent);
            ui_draw_badge(mx + mw - 78, qy + 24, wq[i].done ? "DONE" : "GO!", wq[i].done ? 0x10B981 : th->accent, 0xFFFFFF);
        }
    } else {
        for (int c = 0; c < NUM_ACH_CATS; c++) {
            int cx = mx + c * 88; uint32_t bg = (g_ach_cat == c) ? th->accent : th->card_bg;
            ui_fill_round_rect(cx, 52, 80, 28, 8, bg); if (g_ach_cat != c) ui_draw_round_rect(cx, 52, 80, 28, 8, th->card_border);
            hal_set_text_bg(bg); draw_str(cx + 18, 57, rpg_cat_name(c), 56, (g_ach_cat == c) ? 0xFFFFFF : th->text);
        }
        int base_id = g_ach_cat * 10, col_w = (mw - 12) / 2, row_h = 68;
        for (int slot = 0; slot < 10; slot++) {
            int r = slot / 2, c = slot % 2; if (r < g_ach_scroll || r >= g_ach_scroll + 5) continue;
            int id = base_id + slot; const AchievementDef *ad = rpg_get_achievement(id);
            int is_unl = rpg_profile()->unlocked[id], cur = rpg_get_achievement_progress(id, &snap);
            if (cur > ad->target) cur = ad->target;
            int ax = mx + c * (col_w + 12), ay = 90 + (r - g_ach_scroll) * row_h;
            ui_card(ax, ay, col_w, row_h - 6, 10, th->card_bg, th->card_border, th->shadow, is_unl, 0xF59E0B);
            ui_icon_trophy(ax + 22, ay + 28, is_unl ? 0xF59E0B : th->muted);
            hal_set_text_bg(th->card_bg); char nm[64], ds[96];
            snprintf(nm, sizeof(nm), "%s (+%dXP)", ad->name, ad->xp);
            snprintf(ds, sizeof(ds), "%s [%d/%d]", ad->desc, cur, ad->target);
            draw_str(ax + 42, ay + 8, nm, col_w - 50, is_unl ? th->text : th->muted);
            draw_str(ax + 42, ay + 34, ds, col_w - 50, is_unl ? th->accent : th->muted);
        }
    }
    hal_set_text_bg(th->bg); draw_str(mx, h - 22, "[空白]切分頁  [←→]切獎盃分類  [1]書架  [3]統計  [5]外觀", mw, th->muted);
}

static void render_home_appearance(const AppTheme *th, int w, int h) {
    int mx = 94, mw = w - mx - 18;
    const char *tabs[2] = { "12 套現代主題", "字體 / 排版 / 目標 / 護眼" };
    for (int i = 0; i < 2; i++) {
        int tx = mx + i * 188; uint32_t bg = (g_appear_subtab == i) ? th->accent : th->card_bg;
        ui_fill_round_rect(tx, 10, 180, 30, 8, bg); if (g_appear_subtab != i) ui_draw_round_rect(tx, 10, 180, 30, 8, th->card_border);
        hal_set_text_bg(bg); draw_str(tx + 12, 16, tabs[i], 160, (g_appear_subtab == i) ? 0xFFFFFF : th->text);
    }
    if (g_appear_subtab == 0) {
        ui_fill_round_rect(w - 156, 10, 140, 30, 8, th->card_bg); ui_draw_round_rect(w - 156, 10, 140, 30, 8, th->accent);
        hal_set_text_bg(th->card_bg); char pg[32]; snprintf(pg, sizeof(pg), "換頁 (%d/3) >", g_theme_page + 1);
        draw_str(w - 142, 16, pg, 120, th->accent);
        int gap = 14, cw = (mw - gap * 3) / 4, ch = 156, cy = 76;
        for (int i = 0; i < 4; i++) {
            int tid = g_theme_page * 4 + i; const AppTheme *t = theme_get(tid);
            int cx = mx + i * (cw + gap), is_cur = (tid == g_theme_idx);
            ui_card(cx, cy, cw, ch, 12, t->bg, t->card_border, th->shadow, is_cur, t->accent);
            ui_fill_round_rect(cx + 12, cy + 14, cw - 24, 10, 4, t->bar_bg);
            for (int ln = 0; ln < 4; ln++) hal_fill_rect(cx + 12, cy + 34 + ln * 12, (ln == 3) ? ((cw - 24) * 6 / 10) : (cw - 24), 4, ui_blend_color(t->bg, t->text, 65));
            ui_progress_bar(cx + 12, cy + 88, cw - 24, 5, 68, t->track, t->accent);
            hal_set_text_bg(t->bg); draw_str(cx + 12, cy + 106, t->name, cw - 24, t->text);
            if (is_cur) ui_draw_badge(cx + 12, cy + 130, "ACTIVE", t->accent, 0xFFFFFF);
            else ui_draw_ascii(cx + 12, cy + 133, t->eng, 1, t->muted);
        }
        int py = 252, ph = h - py - 34; ui_card(mx, py, mw, ph, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent);
        hal_set_text_bg(th->card_bg); char pv[128];
        snprintf(pv, sizeof(pv), "當前主題：%s · 字體：%s (%s) · 行距：%s · 翻頁：%s", th->name, FONT_STYLE_NM[g_font_style], FONT_SIZE_NM[g_font_size_idx], SPACING_NM[g_spacing_idx], TRANS_NM[g_transition_idx]);
        draw_str(mx + 18, py + 18, pv, mw - 36, th->accent);
        draw_str(mx + 18, py + 54, "排版示範：春風吹過書頁，12 套現代護眼主題任你切換，", mw - 36, th->text);
        draw_str(mx + 18, py + 86, "支援字體風格、字號大小、5段行距、4段邊距與平滑翻頁動畫！", mw - 36, th->text);
    } else {
        char rows[8][96];
        snprintf(rows[0], sizeof(rows[0]), "1. 字體渲染風格 ：< %s >", FONT_STYLE_NM[g_font_style]);
        snprintf(rows[1], sizeof(rows[1]), "2. 正文字體大小 ：< %s >", FONT_SIZE_NM[g_font_size_idx]);
        snprintf(rows[2], sizeof(rows[2]), "3. 正文行距設定 ：< %s (%dpx) >", SPACING_NM[g_spacing_idx], SPACING_PX[g_spacing_idx]);
        snprintf(rows[3], sizeof(rows[3]), "4. 左右頁面邊距 ：< %s (%dpx) >", MARGIN_NM[g_margin_idx], MARGIN_PX[g_margin_idx]);
        snprintf(rows[4], sizeof(rows[4]), "5. 翻頁視覺動畫 ：< %s >", TRANS_NM[g_transition_idx]);
        snprintf(rows[5], sizeof(rows[5]), "6. 自動翻頁間隔 ：< 每 %d 秒自動翻頁 >", AUTO_SECS[g_auto_sec_idx]);
        snprintf(rows[6], sizeof(rows[6]), "7. 每日閱讀目標 ：< 每天 %d 分鐘 >", GOAL_MINS[g_daily_goal_idx]);
        snprintf(rows[7], sizeof(rows[7]), "8. 定期護眼提醒 ：< %s >", EYE_NM[g_eye_break_idx]);
        for (int i = 0; i < 8; i++) {
            int ry = 58 + i * 46, sel = (i == g_set_row); uint32_t bg = sel ? th->accent : th->card_bg;
            ui_card(mx, ry, mw, 38, 10, bg, th->card_border, th->shadow, sel, th->accent);
            hal_set_text_bg(bg); draw_str(mx + 18, ry + 10, rows[i], mw - 36, sel ? 0xFFFFFF : th->text);
        }
    }
    hal_set_text_bg(th->bg); draw_str(mx, h - 22, "[空白]切子頁  [↑↓←→/點擊左右]即時調整  所有設定自動存檔", mw, th->muted);
}

static void draw_bottom_touch_pills(const AppTheme *th, int w, int by) {
    const char *btns[5] = { "目錄", "書籤", "搜尋", "跳頁", "選單" }; int start_x = w - 340;
    for (int i = 0; i < 5; i++) {
        int bx = start_x + i * 66; uint32_t bg = (i == 4) ? th->accent : th->card_bg;
        ui_fill_round_rect(bx, by + 6, 58, 26, 7, bg); if (i != 4) ui_draw_round_rect(bx, by + 6, 58, 26, 7, th->card_border);
        hal_set_text_bg(bg); draw_str(bx + 11, by + 10, btns[i], 42, (i == 4) ? 0xFFFFFF : th->text);
    }
}

static void render_reading_base(const AppTheme *th, int w, int h) {
    int top_bar = 36, bot_bar = 38; hal_clear(th->bg);
    hal_fill_rect(0, 0, w, top_bar, th->bar_bg); hal_fill_rect(0, top_bar, w, 1, th->card_border);
    ui_fill_round_rect(10, 6, 68, 24, 7, th->card_bg); ui_draw_round_rect(10, 6, 68, 24, 7, th->card_border);
    hal_set_text_bg(th->card_bg); draw_str(18, 9, "< 書架", 56, th->text);
    ui_draw_badge(86, 10, book_format_tag(g_doc.format), ui_format_color(g_doc.format), 0xFFFFFF);
    hal_set_text_bg(th->bar_bg); char tbuf[72]; utf8_ellipsis(g_doc.title[0] ? g_doc.title : get_filename_only(g_filepath), 22, tbuf, sizeof(tbuf));
    draw_str(138, 9, tbuf, 220, th->text);

    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        ui_fill_round_rect(w - 252, 6, 40, 24, 7, th->card_bg); ui_draw_round_rect(w - 252, 6, 40, 24, 7, th->card_border); ui_draw_ascii(w - 235, 14, "-", 1, th->text);
        ui_fill_round_rect(w - 204, 6, 40, 24, 7, th->card_bg); ui_draw_round_rect(w - 204, 6, 40, 24, 7, th->card_border); ui_draw_ascii(w - 187, 14, "+", 1, th->text);
        uint32_t rbg = g_pdf_reflow ? th->accent : th->card_bg;
        ui_fill_round_rect(w - 156, 6, 82, 24, 7, rbg); if (!g_pdf_reflow) ui_draw_round_rect(w - 156, 6, 82, 24, 7, th->card_border);
        hal_set_text_bg(rbg); draw_str(w - 148, 9, g_pdf_reflow ? "原版視圖" : "重排檢視", 72, g_pdf_reflow ? 0xFFFFFF : th->text);
    } else {
        uint32_t abg = g_auto_active ? 0x10B981 : th->card_bg;
        ui_fill_round_rect(w - 150, 6, 76, 24, 7, abg); if (!g_auto_active) ui_draw_round_rect(w - 150, 6, 76, 24, 7, th->card_border);
        hal_set_text_bg(abg); draw_str(w - 140, 9, g_auto_active ? "自動中" : "自動翻", 60, g_auto_active ? 0xFFFFFF : th->text);
    }
    draw_clock_and_goal_pill(th, w - 10, 8);

    int by = h - bot_bar; hal_fill_rect(0, by, w, bot_bar, th->bar_bg);
    int pct = (g_doc.format == BOOK_FMT_PDF && g_pdf) ? (pdf_page_count(g_pdf) > 0 ? ((g_pdf_page + 1) * 100) / pdf_page_count(g_pdf) : 0)
                                                      : (g_page_count > 0 ? ((g_current_page + 1) * 100) / g_page_count : 0);
    hal_fill_rect(0, by, w, 3, th->track); if (pct > 0) hal_fill_rect(0, by, (w * pct) / 100, 3, th->accent);
    if (g_auto_active) hal_fill_rect(0, h - 3, (w * ((g_auto_elapsed * 100) / AUTO_SECS[g_auto_sec_idx])) / 100, 3, 0x10B981);

    uint32_t rem_book = 0, rem_chap = 0; calc_remaining_secs(&rem_book, &rem_chap);
    char rstr[24], foot_l[112]; format_duration_zh(rem_book, rstr, sizeof(rstr));
    hal_set_text_bg(th->bar_bg);
    if (g_toast_msg[0]) draw_str(14, by + 11, g_toast_msg, 430, th->accent);
    else if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        snprintf(foot_l, sizeof(foot_l), "P.%d/%d (%d%%) · 剩約%s · %s", g_pdf_page + 1, pdf_page_count(g_pdf), pct, rstr, PDF_ZOOM_NM[g_pdf_zoom]);
        draw_str(14, by + 11, foot_l, 420, th->text);
    } else if (is_image_fmt(g_doc.format)) draw_str(14, by + 11, "圖片書模式 · 點左右翻頁", 300, th->text);
    else if (g_page_count > 0) {
        snprintf(foot_l, sizeof(foot_l), "第%d/%d頁 (%d%%) · 全書剩約%s", g_current_page + 1, g_page_count, pct, rstr);
        draw_str(14, by + 11, foot_l, 420, th->text);
    }
    draw_bottom_touch_pills(th, w, by);

    if (g_doc.format == BOOK_FMT_PDF && g_pdf) {
        if (!g_pdf_reflow) {
            pdf_render_page(g_pdf, g_pdf_page, 0, top_bar + 1, w, h - top_bar - bot_bar - 1, g_pdf_zoom, g_pdf_pan_x, g_pdf_pan_y,
                            th->is_dark ? 0x0C0F14 : 0xDDD8CE, th->is_dark, &g_pdf_last_pw, &g_pdf_last_ph);
        } else {
            if (g_pdf_reflow_page_cached != g_pdf_page) { free(g_pdf_reflow_text); g_pdf_reflow_text = pdf_extract_page_text(g_pdf, g_pdf_page, &g_pdf_reflow_len); g_pdf_reflow_page_cached = g_pdf_page; }
            int max_u, lines, lh, px; get_layout_params(&max_u, &lines, &lh, &px); hal_set_text_bg(th->bg);
            if (!g_pdf_reflow_text || g_pdf_reflow_len == 0) draw_str(32, top_bar + 32, "此頁為圖像/掃描 PDF，請點右上 [原版視圖]！", w - 64, th->accent);
            else {
                size_t pos = 0; int y = top_bar + 12;
                for (int ln = 0; ln < lines && pos < g_pdf_reflow_len; ln++) {
                    size_t dl = 0, step = scan_one_line_buf(g_pdf_reflow_text, g_pdf_reflow_len, pos, max_u, &dl);
                    if (dl > 0) hal_draw_line_utf8(px, y, g_pdf_reflow_text + pos, (int)dl, w - px * 2, th->text);
                    y += lh; if (step == 0) break;
            pos += step;
                }
            }
        }
        return;
    }
    if (is_image_fmt(g_doc.format)) { hal_draw_image_file(g_filepath, 0, top_bar + 1, w, h - top_bar - bot_bar - 1); return; }
    if (g_doc.text && g_page_offsets && g_page_count > 0) {
        size_t pos = g_page_offsets[g_current_page]; int img_id = -1;
        if (parse_img_marker(g_doc.text, g_doc.text_len, pos, &img_id, NULL)) {
            int iw = 0, ih = 0; const uint8_t *rgb = get_cached_illust(img_id, &iw, &ih);
            int ay = top_bar + 12, ah = h - top_bar - bot_bar - 44, ax = 24, aw = w - 48;
            if (rgb && iw > 0 && ih > 0) {
                int dw = aw, dh = ah; if (iw * ah > aw * ih) dh = (aw * ih) / iw; else dw = (ah * iw) / ih;
                int dx = ax + (aw - dw) / 2, dy = ay + (ah - dh) / 2;
                ui_card(dx - 6, dy - 6, dw + 12, dh + 12, 10, th->card_bg, th->card_border, th->shadow, 0, th->accent);
                hal_blit_rgb888_scaled(rgb, iw, ih, dx, dy, dw, dh, dx, dy, dw, dh, 0);
            }
            return;
        }
        int max_u, lines, lh, px; get_layout_params(&max_u, &lines, &lh, &px); hal_set_text_bg(th->bg);
        int y = top_bar + 12;
        for (int ln = 0; ln < lines && pos < g_doc.text_len; ln++) {
            if (parse_img_marker(g_doc.text, g_doc.text_len, pos, NULL, NULL)) break;
            size_t dl = 0, step = scan_one_line_buf(g_doc.text, g_doc.text_len, pos, max_u, &dl);
            if (dl > 0) hal_draw_line_utf8(px, y, g_doc.text + pos, (int)dl, w - px * 2, th->text);
            y += lh; if (step == 0) break;
            pos += step;
        }
    }
}

static void begin_modal(int w, int h, int mx, int my, int mw, int mh) { hal_clear_text_in_rect(mx - 8, my - 6, mw + 16, mh + 12); hal_dim_rect(0, 0, w, h); }

static void render_read_menu_overlay(const AppTheme *th, int w, int h) {
    int mw = 450, mh = 408, mx = (w - mw) / 2, my = (h - mh) / 2; begin_modal(w, h, mx, my, mw, mh);
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 40, 12, th->bar_bg); hal_set_text_bg(th->bar_bg);
    uint32_t rb = 0, rc = 0; calc_remaining_secs(&rb, &rc);
    char hdr[96], rcs[20], rbs[20]; format_duration_zh(rc, rcs, sizeof(rcs)); format_duration_zh(rb, rbs, sizeof(rbs));
    snprintf(hdr, sizeof(hdr), "控制面板 · 本章剩%s / 全書剩%s", rcs, rbs); draw_str(mx + 18, my + 11, hdr, mw - 36, th->text);
    char items[9][64];
    snprintf(items[0], sizeof(items[0]), "繼續閱讀  (ESC)");
    snprintf(items[1], sizeof(items[1]), "章節目錄 (含字數與時長 · %d項)", g_doc.chapter_count);
    snprintf(items[2], sizeof(items[2]), "手動書籤管理 (已存 %d 筆 · [K])", g_bmark_count);
    snprintf(items[3], sizeof(items[3]), "全文關鍵字搜尋 ([F])");
    snprintf(items[4], sizeof(items[4]), "自動翻頁：%s (每%d秒 · [A])", g_auto_active ? "運作中" : "已關閉", AUTO_SECS[g_auto_sec_idx]);
    snprintf(items[5], sizeof(items[5]), "切換主題 (%d/12)：%s", g_theme_idx + 1, th->name);
    if (g_doc.format == BOOK_FMT_PDF && g_pdf) snprintf(items[6], sizeof(items[6]), "PDF 縮放：%s", PDF_ZOOM_NM[g_pdf_zoom]);
    else snprintf(items[6], sizeof(items[6]), "字體大小：%s (%s)", FONT_SIZE_NM[g_font_size_idx], FONT_STYLE_NM[g_font_style]);
    if (g_doc.format == BOOK_FMT_TXT) snprintf(items[7], sizeof(items[7]), "TXT 內建簡易修字器 ([E])");
    else snprintf(items[7], sizeof(items[7]), "檢視今日閱讀總結報告 ([I])");
    snprintf(items[8], sizeof(items[8]), "存檔並返回現代書架");
    for (int i = 0; i < 9; i++) {
        int ry = my + 48 + i * 38, sel = (i == g_read_menu_sel); uint32_t bg = sel ? th->accent : th->card_bg;
        if (sel) ui_fill_round_rect(mx + 14, ry, mw - 28, 32, 8, bg);
        hal_set_text_bg(bg); draw_str(mx + 26, ry + 7, items[i], mw - 52, sel ? 0xFFFFFF : th->text);
    }
}

static void render_filter_overlay(const AppTheme *th, int w, int h) {
    int mw = 480, mh = 270, mx = (w - mw) / 2, my = (h - mh) / 2;
    begin_modal(w, h, mx, my, mw, mh);
    ui_card(mx, my, mw, mh, 16, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 42, 14, th->bar_bg);
    hal_set_text_bg(th->bar_bg); draw_str(mx + 20, my + 12, "書架篩選器 (點擊過濾 · 保持書架清爽)", mw - 40, th->text);

    hal_set_text_bg(th->card_bg); draw_str(mx + 20, my + 54, "按檔案格式：", 200, th->muted);
    const char *fp[5] = { "全部", "EPUB", "PDF", "TXT", "圖片" };
    for (int i = 0; i < 5; i++) {
        int px = mx + 20 + i * 88; uint32_t bg = (g_filter_fmt == i) ? th->accent : th->bg;
        ui_fill_round_rect(px, my + 80, 80, 34, 9, bg);
        if (g_filter_fmt != i) ui_draw_round_rect(px, my + 80, 80, 34, 9, th->card_border);
        hal_set_text_bg(bg); draw_str(px + 20, my + 88, fp[i], 50, (g_filter_fmt == i) ? 0xFFFFFF : th->text);
    }

    hal_set_text_bg(th->card_bg); draw_str(mx + 20, my + 128, "按閱讀狀態：", 200, th->muted);
    const char *sp[5] = { "全狀態", "想看", "閱讀中", "已完食", "暫擱" };
    for (int i = 0; i < 5; i++) {
        int px = mx + 20 + i * 88; uint32_t bg = (g_filter_status == i) ? 0x10B981 : th->bg;
        ui_fill_round_rect(px, my + 154, 80, 34, 9, bg);
        if (g_filter_status != i) ui_draw_round_rect(px, my + 154, 80, 34, 9, th->card_border);
        hal_set_text_bg(bg); draw_str(px + 14, my + 162, sp[i], 60, (g_filter_status == i) ? 0xFFFFFF : th->text);
    }

    ui_fill_round_rect(mx + 20, my + 214, 212, 38, 10, th->bg); ui_draw_round_rect(mx + 20, my + 214, 212, 38, 10, th->card_border);
    hal_set_text_bg(th->bg); draw_str(mx + 82, my + 224, "重設全部", 100, th->text);
    ui_fill_round_rect(mx + 248, my + 214, 212, 38, 10, th->accent);
    hal_set_text_bg(th->accent); draw_str(mx + 312, my + 224, "套用篩選", 100, 0xFFFFFF);
}

/* 全螢幕現代「獨立書籍詳情頁」 (24款封面切換 / 5星評分 / 4狀態 / 里程碑 / 快捷操作) */
static void render_book_detail_page(const AppTheme *th, int w, int h) {
    (void)h;
    if (g_style_book_idx < 0 || g_style_book_idx >= g_book_count) return;
    BookEntry *be = &g_books[g_style_book_idx];
    hal_clear(th->bg);

    /* 頂部極簡導覽列 */
    hal_fill_rect(0, 0, w, 46, th->bar_bg); hal_fill_rect(0, 46, w, 1, th->card_border);
    ui_fill_round_rect(16, 9, 96, 28, 10, th->card_bg); ui_draw_round_rect(16, 9, 96, 28, 10, th->card_border);
    hal_set_text_bg(th->card_bg); draw_str(26, 14, "< 返回書架", 80, th->text);

    ui_draw_badge(126, 16, book_format_tag(be->format), ui_format_color(be->format), 0xFFFFFF);
    hal_set_text_bg(th->bar_bg); char thdr[96]; utf8_ellipsis(be->title, 34, thdr, sizeof(thdr));
    draw_str(182, 14, thdr, w - 200, th->text);

    /* 左欄：大尺寸精裝書封預覽 + 24 款模板切換器 */
    ui_card(20, 62, 204, 392, 16, th->card_bg, th->card_border, th->shadow, 0, th->accent);
    draw_book_cover_smart(44, 78, 156, 216, be, th->is_dark);

    ui_fill_round_rect(32, 316, 86, 36, 9, th->bg); ui_draw_round_rect(32, 316, 86, 36, 9, th->card_border);
    hal_set_text_bg(th->bg); draw_str(44, 325, "< 上款", 64, th->text);

    ui_fill_round_rect(126, 316, 86, 36, 9, th->accent);
    hal_set_text_bg(th->accent); draw_str(140, 325, "下款 >", 64, 0xFFFFFF);

    hal_set_text_bg(th->card_bg);
    char cname[48]; utf8_ellipsis(cover_style_name(be->cover_style), 19, cname, sizeof(cname));
    draw_str(32, 368, "精裝封面 (24款)：", 180, th->muted);
    draw_str(32, 394, cname, 180, th->text);
    draw_str(32, 424, "左右點按即時預覽", 180, th->muted);

    /* 右欄：書本詳情、星星評分、狀態、進度里程碑與大按鈕 */
    int rx = 242, rw = w - rx - 20;
    ui_card(rx, 62, rw, 312, 16, th->card_bg, th->card_border, th->shadow, 0, th->accent);
    hal_set_text_bg(th->card_bg);

    char full_t[96]; utf8_ellipsis(be->title, 30, full_t, sizeof(full_t));
    draw_str(rx + 20, 74, full_t, rw - 40, th->text);

    /* 互動 5 顆金色星星 */
    ui_draw_star_row(rx + 20, 114, be->rating, 5, 36, 0xF59E0B, th->track);
    char rtxt[48]; snprintf(rtxt, sizeof(rtxt), "%d / 5 顆星 (點星星評分)", be->rating);
    draw_str(rx + 216, 110, rtxt, rw - 230, th->muted);

    /* 閱讀狀態 4 顆現代膠囊 */
    draw_str(rx + 20, 146, "閱讀狀態：", 120, th->muted);
    for (int s = 0; s < 4; s++) {
        int sx = rx + 20 + s * 118;
        uint32_t bg = (be->read_status == s) ? 0x10B981 : th->bg;
        ui_fill_round_rect(sx, 172, 108, 36, 10, bg);
        if (be->read_status != s) ui_draw_round_rect(sx, 172, 108, 36, 10, th->card_border);
        hal_set_text_bg(bg); draw_str(sx + 26, 181, STATUS_NM[s], 68, (be->read_status == s) ? 0xFFFFFF : th->text);
    }

    /* 單書閱讀進度與 4 大里程碑印章 (25% / 50% / 75% / 100%) */
    int pct = calc_book_pct(be);
    ui_fill_round_rect(rx + 20, 224, rw - 40, 134, 12, th->bg);
    hal_set_text_bg(th->bg);
    char dstr[24], s1[128], s2[128]; format_duration_zh(be->read_secs, dstr, sizeof(dstr));
    snprintf(s1, sizeof(s1), "進度：%d%% (第 %d / %d 頁)  ·  累計閱讀：%s",
             pct, be->page_idx + 1, be->page_count > 0 ? be->page_count : 1, dstr);
    snprintf(s2, sizeof(s2), "資料夾：%s  ·  作者：%s",
             be->folder[0] ? be->folder : "外層主書架", be->author[0] ? be->author : "未標註");
    draw_str(rx + 34, 236, s1, rw - 68, th->text);
    draw_str(rx + 34, 264, s2, rw - 68, th->muted);
    ui_progress_bar(rx + 34, 294, rw - 68, 9, pct, th->track, th->accent);

    const char *mlbl[4] = { "25% 起步", "50% 過半", "75% 衝刺", "100% 完食" };
    int mvals[4] = { 25, 50, 75, 100 };
    for (int i = 0; i < 4; i++) {
        int mx0 = rx + 34 + i * 116;
        int hit = (pct >= mvals[i] || (i == 3 && be->read_status == 2));
        uint32_t mbg = hit ? 0xF59E0B : th->card_bg;
        ui_fill_round_rect(mx0, 316, 106, 28, 8, mbg);
        if (!hit) ui_draw_round_rect(mx0, 316, 106, 28, 8, th->card_border);
        hal_set_text_bg(mbg); draw_str(mx0 + 12, 321, mlbl[i], 88, hit ? 0xFFFFFF : th->muted);
    }

    /* 底部 3 顆現代大按鈕：[開始 / 繼續閱讀] [移至資料夾] [移出書架] */
    ui_card(rx, 392, 240, 48, 12, th->accent, th->accent, th->shadow, 1, th->accent);
    hal_set_text_bg(th->accent); draw_str(rx + 66, 406, pct > 0 ? "繼續閱讀本書" : "開始閱讀本書", 140, 0xFFFFFF);

    ui_card(rx + 252, 392, 134, 48, 12, th->card_bg, th->card_border, th->shadow, 0, th->accent);
    hal_set_text_bg(th->card_bg); draw_str(rx + 274, 406, "移至資料夾", 100, th->text);

    ui_card(rx + 398, 392, rw - 398, 48, 12, 0xEF4444, 0xEF4444, th->shadow, 0, 0xEF4444);
    hal_set_text_bg(0xEF4444); draw_str(rx + 422, 406, "移出書架", 96, 0xFFFFFF);
}

static void render_txt_editor_overlay(const AppTheme *th, int w, int h) {
    int mw = 600, mh = 390, mx = (w - mw) / 2, my = (h - mh) / 2; begin_modal(w, h, mx, my, mw, mh);
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 42, 12, th->bar_bg); hal_set_text_bg(th->bar_bg);
    draw_str(mx + 18, my + 12, g_edit_editing ? "TXT 修字器 — 編輯本行 (鍵盤打字/退格 · Enter儲存)" : "TXT 修字器 — 點選要修改的句子 (ESC 返回)", mw - 36, th->text);
    if (!g_edit_editing) {
        for (int i = 0; i < g_edit_line_cnt && i < 12; i++) {
            int ry = my + 54 + i * 24, sel = (i == g_edit_sel); uint32_t bg = sel ? th->accent : th->bg;
            ui_fill_round_rect(mx + 14, ry, mw - 28, 22, 5, bg);
            char raw[96], line[120]; size_t rl = g_edit_lens[i] < 90 ? g_edit_lens[i] : 90;
            memcpy(raw, g_doc.text + g_edit_starts[i], rl); raw[rl] = '\0'; snprintf(line, sizeof(line), "%02d| %s", i + 1, raw);
            hal_set_text_bg(bg); draw_str(mx + 20, ry + 2, line, mw - 40, sel ? 0xFFFFFF : th->text);
        }
        hal_set_text_bg(th->card_bg); draw_str(mx + 18, my + mh - 32, "點擊或按 [Enter] 編輯該行 · [D] 刪除此行 · [ESC] 返回", mw - 36, th->muted);
    } else {
        ui_fill_round_rect(mx + 18, my + 60, mw - 36, 82, 10, th->bg); ui_draw_round_rect(mx + 18, my + 60, mw - 36, 82, 10, th->accent);
        hal_set_text_bg(th->bg); char disp[280]; snprintf(disp, sizeof(disp), "%s_", g_edit_buf); draw_str(mx + 28, my + 74, disp, mw - 56, th->text);
        const char *qbtns[4] = { "清首尾空白", "修全形標點", "清空此行", "重選別行" };
        for (int i = 0; i < 4; i++) {
            int bx = mx + 20 + i * 140; ui_fill_round_rect(bx, my + 164, 130, 36, 8, th->bar_bg); ui_draw_round_rect(bx, my + 164, 130, 36, 8, th->card_border);
            hal_set_text_bg(th->bar_bg); draw_str(bx + 18, my + 173, qbtns[i], 104, th->text);
        }
        ui_fill_round_rect(mx + 20, my + 240, 270, 44, 10, 0x10B981); hal_set_text_bg(0x10B981); draw_str(mx + 82, my + 252, "儲存寫回 TXT [Enter]", 180, 0xFFFFFF);
        ui_fill_round_rect(mx + 310, my + 240, 270, 44, 10, th->bar_bg); ui_draw_round_rect(mx + 310, my + 240, 270, 44, 10, th->card_border);
        hal_set_text_bg(th->bar_bg); draw_str(mx + 410, my + 252, "取消 [ESC]", 100, th->text);
    }
}

static void render_daily_report_modal(const AppTheme *th, int w, int h) {
    int mw = 540, mh = 360, mx = (w - mw) / 2, my = (h - mh) / 2; begin_modal(w, h, mx, my, mw, mh);
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 42, 12, th->bar_bg); hal_set_text_bg(th->bar_bg);
    draw_str(mx + 18, my + 12, "< 前一天", 84, th->accent); draw_str(mx + 188, my + 12, "每日閱讀總結報告", 180, th->text); draw_str(mx + mw - 92, my + 12, "後一天 >", 84, th->accent);
    render_daily_report_body(th, mx, my + 42, mw, mh - 84);
    ui_fill_round_rect(mx + 180, my + mh - 42, 180, 32, 10, th->accent); hal_set_text_bg(th->accent); draw_str(mx + 242, my + mh - 35, "太棒了", 80, 0xFFFFFF);
}

static void render_eye_break_modal(const AppTheme *th, int w, int h) {
    int bw = 460, bh = 210, bx = (w - bw) / 2, by = (h - bh) / 2; begin_modal(w, h, bx, by, bw, bh);
    ui_card(bx, by, bw, bh, 14, th->card_bg, 0x10B981, th->shadow, 1, 0x10B981); hal_set_text_bg(th->card_bg);
    char t1[96]; snprintf(t1, sizeof(t1), "【護眼休息時間到！】已連續閱讀 %d 分鐘", EYE_MINS[g_eye_break_idx]);
    draw_str(bx + 24, by + 24, t1, bw - 48, 0x10B981);
    draw_str(bx + 24, by + 62, "20-20-20 護眼法則：請抬頭看看 6 公尺外遠方 20 秒，", bw - 48, th->text);
    draw_str(bx + 24, by + 92, "眨眨眼睛放鬆睫狀肌，保護視力再繼續看書！", bw - 48, th->muted);
    ui_fill_round_rect(bx + 18, by + 148, 134, 38, 10, 0x10B981); hal_set_text_bg(0x10B981); draw_str(bx + 42, by + 158, "繼續閱讀", 90, 0xFFFFFF);
    ui_fill_round_rect(bx + 162, by + 148, 134, 38, 10, th->accent); hal_set_text_bg(th->accent); draw_str(bx + 184, by + 158, "延後10分", 90, 0xFFFFFF);
    ui_fill_round_rect(bx + 306, by + 148, 136, 38, 10, th->bar_bg); ui_draw_round_rect(bx + 306, by + 148, 136, 38, 10, th->card_border);
    hal_set_text_bg(th->bar_bg); draw_str(bx + 328, by + 158, "回書架休息", 96, th->text);
}

static void render_toc_page(const AppTheme *th, int w, int h) {
    hal_clear(th->bg); int top_bar = 44, bot_bar = 34;
    hal_fill_rect(0, 0, w, top_bar, th->bar_bg); hal_fill_rect(0, top_bar, w, 1, th->card_border); hal_set_text_bg(th->bar_bg);
    char hdr[192]; snprintf(hdr, sizeof(hdr), "目錄 (含章節字數與預估時長) · %s (%d項)", g_doc.title, g_doc.chapter_count);
    draw_str(22, 12, hdr, w - 130, th->text);
    ui_fill_round_rect(w - 92, 8, 76, 28, 8, th->card_bg); ui_draw_round_rect(w - 92, 8, 76, 28, 8, th->card_border);
    hal_set_text_bg(th->card_bg); draw_str(w - 76, 13, "返回", 50, th->text);
    hal_fill_rect(0, h - bot_bar, w, bot_bar, th->bar_bg); hal_set_text_bg(th->bar_bg);
    draw_str(22, h - bot_bar + 8, "點擊章節跳轉 · 底部 [左]上捲 [中]下捲 [右]返回", w - 44, th->muted);
    if (g_doc.chapter_count == 0) { ui_card(80, 110, w - 160, 120, 14, th->card_bg, th->card_border, th->shadow, 0, th->accent); hal_set_text_bg(th->card_bg); draw_str(110, 154, "本書未偵測到章節標題，請按 [G] 使用跳頁滑桿。", w - 220, th->text); return; }
    int row_h = 38, vis = (h - top_bar - bot_bar - 20) / row_h; if (vis < 1) vis = 1;
    if (g_toc_sel < g_toc_scroll) g_toc_scroll = g_toc_sel;
    if (g_toc_sel >= g_toc_scroll + vis) g_toc_scroll = g_toc_sel - vis + 1;
    for (int i = g_toc_scroll; i < g_doc.chapter_count && i < g_toc_scroll + vis; i++) {
        int ry = top_bar + 12 + (i - g_toc_scroll) * row_h, sel = (i == g_toc_sel); uint32_t bg = sel ? th->accent : th->card_bg;
        ui_card(24, ry, w - 48, row_h - 6, 8, bg, th->card_border, th->shadow, sel, th->accent); hal_set_text_bg(bg);
        char line[120], ct[64]; utf8_ellipsis(g_doc.chapters[i].title, 26, ct, sizeof(ct)); snprintf(line, sizeof(line), "%02d. %s", i + 1, ct);
        draw_str(38, ry + 7, line, w - 240, sel ? 0xFFFFFF : th->text);
        if (g_ch_chars[i] > 0) { char meta[48]; snprintf(meta, sizeof(meta), "%d字 · 約%d分", g_ch_chars[i], txt_estimate_read_mins(g_ch_chars[i])); draw_str(w - 196, ry + 7, meta, 160, sel ? 0xFEF08A : th->muted); }
    }
}

static void render_bookmarks_overlay(const AppTheme *th, int w, int h) {
    int mw = 540, mh = 360, mx = (w - mw) / 2, my = (h - mh) / 2; begin_modal(w, h, mx, my, mw, mh);
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 44, 12, th->bar_bg); hal_set_text_bg(th->bar_bg);
    char hdr[64]; snprintf(hdr, sizeof(hdr), "我的書籤 (%d / %d)", g_bmark_count, MAX_BOOKMARKS); draw_str(mx + 18, my + 13, hdr, 240, th->text);
    ui_fill_round_rect(mx + mw - 132, my + 8, 118, 28, 14, th->accent); hal_set_text_bg(th->accent); draw_str(mx + mw - 120, my + 13, "+ 新增本頁", 96, 0xFFFFFF);
    if (g_bmark_count == 0) { hal_set_text_bg(th->card_bg); draw_str(mx + 32, my + 120, "尚無書籤，點右上 [+ 新增本頁] 或按空白鍵加入！", mw - 64, th->text); }
    else {
        int start = (g_bmark_sel >= 7) ? (g_bmark_sel - 6) : 0;
        for (int i = start; i < g_bmark_count && i < start + 7; i++) {
            int ry = my + 56 + (i - start) * 38, sel = (i == g_bmark_sel); uint32_t bg = sel ? th->accent : th->bg;
            ui_fill_round_rect(mx + 16, ry, mw - 78, 32, 8, bg); hal_set_text_bg(bg);
            char ln[128]; snprintf(ln, sizeof(ln), "P.%d (%d%%) %s", g_bmarks[i].page_num, g_bmarks[i].pct, g_bmarks[i].label);
            draw_str(mx + 26, ry + 7, ln, mw - 98, sel ? 0xFFFFFF : th->text);
            ui_fill_round_rect(mx + mw - 56, ry + 2, 40, 28, 7, 0xEF4444); hal_set_text_bg(0xEF4444); draw_str(mx + mw - 45, ry + 7, "刪", 24, 0xFFFFFF);
        }
    }
}

static void render_search_overlay(const AppTheme *th, int w, int h) {
    int mw = 580, mh = 380, mx = (w - mw) / 2, my = (h - mh) / 2; begin_modal(w, h, mx, my, mw, mh);
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 44, 12, th->bar_bg); hal_set_text_bg(th->bar_bg); draw_str(mx + 18, my + 13, "全文關鍵字搜尋 (打字+Enter · 點結果跳轉)", mw - 36, th->text);
    ui_fill_round_rect(mx + 18, my + 56, mw - 36, 36, 10, th->bg); ui_draw_round_rect(mx + 18, my + 56, mw - 36, 36, 10, th->accent);
    hal_set_text_bg(th->bg); char qd[96];
    if (g_search_query[0]) snprintf(qd, sizeof(qd), "搜尋：%s_", g_search_query); else snprintf(qd, sizeof(qd), "輸入關鍵字，或按 [1]第一章 [2]序章 [3]結論 [4]重點");
    draw_str(mx + 30, my + 65, qd, mw - 60, g_search_query[0] ? th->text : th->muted);
    if (g_search_ran && g_search_cnt > 0) {
        if (g_search_sel < g_search_scroll) g_search_scroll = g_search_sel;
        if (g_search_sel >= g_search_scroll + 6) g_search_scroll = g_search_sel - 5;
        for (int i = g_search_scroll; i < g_search_cnt && i < g_search_scroll + 6; i++) {
            int ry = my + 104 + (i - g_search_scroll) * 38, sel = (i == g_search_sel); uint32_t bg = sel ? th->accent : th->bg;
            ui_fill_round_rect(mx + 18, ry, mw - 36, 32, 8, bg); hal_set_text_bg(bg);
            char ln[128]; snprintf(ln, sizeof(ln), "[P.%d] %s", g_search_hits[i].page_num, g_search_hits[i].snippet);
            draw_str(mx + 28, ry + 7, ln, mw - 56, sel ? 0xFFFFFF : th->text);
        }
    } else if (g_search_ran) { hal_set_text_bg(th->card_bg); draw_str(mx + 26, my + 124, "找不到符合的內容", 200, 0xEF4444); }
}

static void render_jump_overlay(const AppTheme *th, int w, int h) {
    int tot = (g_doc.format == BOOK_FMT_PDF && g_pdf) ? pdf_page_count(g_pdf) : g_page_count;
    int bw = 460, bh = 160, bx = (w - bw) / 2, by = (h - bh) / 2; begin_modal(w, h, bx, by, bw, bh);
    ui_card(bx, by, bw, bh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent); hal_set_text_bg(th->card_bg);
    int pct = (tot > 0) ? (((g_jump_target + 1) * 100) / tot) : 0;
    char t1[96]; snprintf(t1, sizeof(t1), "快速跳頁：第 %d / %d 頁 (%d%%)", g_jump_target + 1, tot, pct);
    draw_str(bx + 24, by + 20, t1, bw - 48, th->text);
    ui_progress_bar(bx + 24, by + 64, bw - 48, 10, pct, th->track, th->accent);
    hal_fill_circle(bx + 24 + ((bw - 48) * pct) / 100, by + 69, 10, th->accent);
    ui_fill_round_rect(bx + 24, by + 106, 196, 36, 10, th->accent); hal_set_text_bg(th->accent); draw_str(bx + 76, by + 115, "確認跳轉", 100, 0xFFFFFF);
    ui_fill_round_rect(bx + 240, by + 106, 196, 36, 10, th->bar_bg); hal_set_text_bg(th->bar_bg); draw_str(bx + 314, by + 115, "取消", 60, th->text);
}

static void render_new_folder_overlay(const AppTheme *th, int w, int h) {
    int mw = 480, mh = 250, mx = (w - mw) / 2, my = (h - mh) / 2; begin_modal(w, h, mx, my, mw, mh);
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 42, 12, th->bar_bg); hal_set_text_bg(th->bar_bg); draw_str(mx + 18, my + 12, "新增書架資料夾", mw - 36, th->text);
    ui_fill_round_rect(mx + 18, my + 58, mw - 36, 38, 10, th->bg); ui_draw_round_rect(mx + 18, my + 58, mw - 36, 38, 10, th->accent);
    hal_set_text_bg(th->bg); char disp[96];
    if (g_new_folder_buf[0]) snprintf(disp, sizeof(disp), "名稱：%s_", g_new_folder_buf); else snprintf(disp, sizeof(disp), "輸入名稱或點/按 [1~4] 快捷建立");
    draw_str(mx + 28, my + 68, disp, mw - 56, g_new_folder_buf[0] ? th->text : th->muted);
    const char *ps[4] = { "1.鬼滅之刃", "2.漫畫", "3.小說", "4.講義" }; int bw = (mw - 60) / 4;
    for (int i = 0; i < 4; i++) { int px = mx + 18 + i * (bw + 8); ui_fill_round_rect(px, my + 114, bw, 34, 8, th->bar_bg); hal_set_text_bg(th->bar_bg); draw_str(px + 8, my + 122, ps[i], bw - 12, th->text); }
    ui_fill_round_rect(mx + 24, my + 188, 200, 38, 10, th->accent); hal_set_text_bg(th->accent); draw_str(mx + 82, my + 198, "確認建立", 90, 0xFFFFFF);
    ui_fill_round_rect(mx + 256, my + 188, 200, 38, 10, th->bar_bg); hal_set_text_bg(th->bar_bg); draw_str(mx + 332, my + 198, "取消", 60, th->text);
}

static void render_move_folder_overlay(const AppTheme *th, int w, int h) {
    int mw = 440, mh = 340, mx = (w - mw) / 2, my = (h - mh) / 2; begin_modal(w, h, mx, my, mw, mh);
    ui_card(mx, my, mw, mh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent);
    ui_fill_round_rect(mx + 2, my + 2, mw - 4, 44, 12, th->bar_bg); hal_set_text_bg(th->bar_bg); draw_str(mx + 18, my + 13, "移動至資料夾", mw - 36, th->text);
    int tot = g_folder_count + 1, start = (g_move_folder_sel >= 6) ? (g_move_folder_sel - 5) : 0;
    for (int i = start; i < tot && i < start + 6; i++) {
        int ry = my + 56 + (i - start) * 38, sel = (i == g_move_folder_sel); uint32_t bg = sel ? th->accent : th->bg;
        ui_fill_round_rect(mx + 16, ry, mw - 32, 32, 8, bg); hal_set_text_bg(bg);
        if (i == 0) draw_str(mx + 28, ry + 7, "移出資料夾 (放回外層)", mw - 56, sel ? 0xFFFFFF : th->text);
        else { char ln[96]; snprintf(ln, sizeof(ln), "%d. %s (%d本)", i, g_folders[i - 1], folder_count_books(g_folders[i - 1])); draw_str(mx + 28, ry + 7, ln, mw - 56, sel ? 0xFFFFFF : th->text); }
    }
}

static void render_next_vol_overlay(const AppTheme *th, int w, int h) {
    int bw = 460, bh = 170, bx = (w - bw) / 2, by = (h - bh) / 2; begin_modal(w, h, bx, by, bw, bh);
    ui_card(bx, by, bw, bh, 14, th->card_bg, th->card_border, th->shadow, 1, th->accent); hal_set_text_bg(th->card_bg);
    draw_str(bx + 24, by + 22, "本卷已讀完！是否接著閱讀同資料夾下一卷？", bw - 48, th->text);
    if (g_next_vol_idx >= 0 && g_next_vol_idx < g_book_count) draw_str(bx + 24, by + 58, g_books[g_next_vol_idx].title, bw - 48, th->accent);
    ui_fill_round_rect(bx + 24, by + 112, 196, 38, 10, th->accent); hal_set_text_bg(th->accent); draw_str(bx + 62, by + 122, "閱讀下一卷", 120, 0xFFFFFF);
    ui_fill_round_rect(bx + 240, by + 112, 196, 38, 10, th->bar_bg); hal_set_text_bg(th->bar_bg); draw_str(bx + 300, by + 122, "留在本頁", 80, th->text);
}

void reader_render(void) {
    const AppTheme *th = theme_get(g_theme_idx); int w = hal_get_width(), h = hal_get_height();
    if (g_screen == SCR_BOOK_STYLE) { render_book_detail_page(th, w, h); hal_present(); return; }
    if (g_screen == SCR_HOME || g_screen == SCR_NEW_FOLDER || g_screen == SCR_MOVE_FOLDER ||
        g_screen == SCR_FILTER || (g_screen == SCR_DAILY_REPORT && g_return_screen == SCR_HOME)) {
        hal_clear(th->bg); render_left_rail(th, h);
        if (g_home_tab == TAB_SHELF) render_home_shelf(th, w, h);
        else if (g_home_tab == TAB_MANAGE) render_home_manage(th, w, h);
        else if (g_home_tab == TAB_STATS) render_home_stats(th, w, h);
        else if (g_home_tab == TAB_ACHIEVE) render_home_achieve(th, w, h);
        else render_home_appearance(th, w, h);
        if (g_screen == SCR_NEW_FOLDER) render_new_folder_overlay(th, w, h);
        else if (g_screen == SCR_MOVE_FOLDER) render_move_folder_overlay(th, w, h);
        else if (g_screen == SCR_FILTER) render_filter_overlay(th, w, h);
        else if (g_screen == SCR_DAILY_REPORT) render_daily_report_modal(th, w, h);
        hal_present(); return;
    }
    if (g_screen == SCR_TOC) { render_toc_page(th, w, h); hal_present(); return; }
    render_reading_base(th, w, h);
    if (g_screen == SCR_READ_MENU) render_read_menu_overlay(th, w, h);
    else if (g_screen == SCR_JUMP) render_jump_overlay(th, w, h);
    else if (g_screen == SCR_BOOKMARKS) render_bookmarks_overlay(th, w, h);
    else if (g_screen == SCR_SEARCH) render_search_overlay(th, w, h);
    else if (g_screen == SCR_NEXT_VOL) render_next_vol_overlay(th, w, h);
    else if (g_screen == SCR_TXT_EDITOR) render_txt_editor_overlay(th, w, h);
    else if (g_screen == SCR_DAILY_REPORT) render_daily_report_modal(th, w, h);
    else if (g_screen == SCR_EYE_BREAK) render_eye_break_modal(th, w, h);
    hal_present();
}
