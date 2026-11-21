#include "covers.h"
#include "ui_gfx.h"
#include "hal.h"
#include <stdlib.h>

static const char *STYLE_NAMES[NUM_COVER_STYLES] = {
    "自動 (優先真封面)", "01. 典雅深海精裝", "02. 線裝中華古籍", "03. 燙金典藏黑金",
    "04. 日系極簡文庫", "05. 賽博龐克霓虹", "06. 星空銀河幻境", "07. 森系植物手帳",
    "08. 復古牛皮札記", "09. 浮世繪海浪紋", "10. 包豪斯幾何派", "11. 水墨山水意境",
    "12. 櫻花粉韻物語", "13. 暗黑哥德城堡", "14. 英倫學院格紋", "15. 波普藝術撞色",
    "16. 琥珀日落漸層", "17. 翡翠玉雕典雅", "18. 皇家紅絨徽章", "19. 工業拉絲金屬",
    "20. 熱血漫畫分鏡", "21. 科幻星際矩陣", "22. 極簡北歐純白", "23. 典雅大理石紋",
    "24. 暖陽咖啡手札"
};

const char *cover_style_name(int style_id) {
    if (style_id < 0 || style_id >= NUM_COVER_STYLES) style_id = 0;
    return STYLE_NAMES[style_id];
}

static unsigned hash_str(const char *s) {
    unsigned h = 5381u;
    if (!s) return 1u;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}

void ui_draw_styled_cover(int x, int y, int w, int h, const char *title, BookFormat fmt, int style_id, int is_dark) {
    if (w < 24 || h < 24) return;
    int eff = (style_id > 0 && style_id < NUM_COVER_STYLES) ? style_id : (1 + (int)(hash_str(title) % 24u));
    uint32_t shcol = is_dark ? 0x07090D : 0xC2BCB0;
    ui_fill_round_rect(x + 4, y + 4, w, h, 7, shcol);
    ui_fill_round_rect(x + 2, y + 2, w, h - 2, 6, 0xEDEBE4);
    int cw = w - 2, ch = h - 2, cx = x + cw / 2, cy = y + ch / 2;

    switch (eff) {
        case 1:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x1E3A5F);
            hal_fill_rect(x, y + 3, 8, ch - 6, 0x132640);
            hal_fill_rect(x + 8, y + 2, 2, ch - 4, 0xD4AF37);
            ui_draw_round_rect(x + 13, y + 8, cw - 19, ch - 16, 4, 0xD4AF37);
            hal_fill_circle(cx + 3, cy, ch / 6, 0x2B5482);
            hal_draw_circle(cx + 3, cy, ch / 6 + 3, 0xD4AF37);
            break;
        case 2: {
            ui_fill_round_rect(x, y, cw, ch, 5, 0x2B4C7E);
            hal_fill_rect(x, y + 2, 12, ch - 4, 0x1B355C);
            for (int i = 0; i < 4; i++) {
                int sy = y + 12 + (i * (ch - 26)) / 3;
                hal_fill_rect(x + 1, sy, 11, 2, 0xF8F5EE);
                hal_fill_circle(x + 10, sy + 1, 2, 0xF8F5EE);
            }
            int sw = cw / 3; if (sw < 16) sw = 16;
            hal_fill_rect(x + cw - sw - 8, y + 10, sw, (ch * 55) / 100, 0xFAF6EB);
            hal_draw_rect(x + cw - sw - 8, y + 10, sw, (ch * 55) / 100, 0xB91C1C);
            hal_draw_rect(x + cw - sw - 6, y + 12, sw - 4, (ch * 55) / 100 - 4, 0xDC2626);
            break;
        }
        case 3: {
            ui_fill_round_rect(x, y, cw, ch, 6, 0x141416);
            hal_fill_rect(x, y + 3, 7, ch - 6, 0x09090B);
            ui_draw_round_rect(x + 10, y + 7, cw - 15, ch - 14, 3, 0xEAB308);
            ui_draw_round_rect(x + 13, y + 10, cw - 21, ch - 20, 2, 0xA16207);
            int dr = ch / 7; if (dr < 8) dr = 8;
            for (int dy = -dr; dy <= dr; dy++) {
                int hw = dr - abs(dy);
                hal_fill_rect(cx + 2 - hw, cy + dy, hw * 2 + 1, 1, (abs(dy) < dr - 2) ? 0x1E1E22 : 0xFACC15);
            }
            break;
        }
        case 4: {
            ui_fill_round_rect(x, y, cw, ch, 6, 0xFAF9F6);
            ui_draw_round_rect(x, y, cw, ch, 6, 0xD6D3CD);
            int obi_h = (ch * 36) / 100;
            hal_fill_rect(x + 1, y + ch - obi_h - 1, cw - 2, obi_h, 0x2563EB);
            hal_fill_rect(x + 1, y + ch - obi_h - 4, cw - 2, 2, 0x93C5FD);
            hal_fill_circle(cx, y + (ch - obi_h) / 2, ch / 8, 0xEF4444);
            break;
        }
        case 5:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x0F0B24);
            for (int gy = y + 14; gy < y + ch - 8; gy += 12) hal_fill_rect(x + 6, gy, cw - 12, 1, 0x2A1B54);
            hal_fill_rect(x + 8, y + ch / 3, cw - 16, 4, 0x06B6D4);
            hal_fill_rect(x + 14, y + ch / 3 + 8, cw - 28, 3, 0xEC4899);
            hal_fill_rect(x + 8, y + ch - 20, cw - 24, 3, 0xA855F7);
            ui_draw_round_rect(x, y, cw, ch, 6, 0x06B6D4);
            break;
        case 6: {
            ui_fill_round_rect(x, y, cw, ch, 6, 0x131836);
            hal_fill_rect(x + 2, y + ch / 2, cw - 4, ch / 2 - 2, 0x1E254D);
            hal_fill_circle(cx + 8, y + ch / 3, ch / 7, 0xFEF08A);
            hal_fill_circle(cx + 13, y + ch / 3 - 3, ch / 7, 0x131836);
            static const int SX[7] = { -18, 20, -10, 14, -22, 6, -4 };
            static const int SY[7] = { -20, -14, 10, 18, 22, -26, 26 };
            for (int i = 0; i < 7; i++) {
                int px = cx + SX[i], py = cy + SY[i];
                if (px > x + 4 && px < x + cw - 4 && py > y + 4 && py < y + ch - 4)
                    hal_fill_rect(px, py, 2, 2, (i & 1) ? 0xFFFFFF : 0x7DD3FC);
            }
            break;
        }
        case 7:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x2F5D50);
            hal_fill_rect(x, y + 2, 9, ch - 4, 0x1E3F35);
            ui_fill_round_rect(x + 14, y + 12, cw - 22, ch - 24, 5, 0xDFEEEA);
            hal_fill_rect(cx + 2, y + 22, 2, ch - 44, 0x2F5D50);
            hal_fill_circle(cx - 4, cy - 6, 6, 0x5E8B7E);
            hal_fill_circle(cx + 8, cy + 4, 6, 0x3A6B5C);
            hal_fill_circle(cx - 4, cy + 12, 5, 0x5E8B7E);
            break;
        case 8:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x7C4A2D);
            hal_fill_rect(x, y + 3, 10, ch - 6, 0x59321C);
            hal_fill_rect(x + cw - 12, y + 2, 10, 4, 0xD97706);
            hal_fill_rect(x + cw - 6,  y + 2, 4, 10, 0xD97706);
            hal_fill_rect(x + cw - 12, y + ch - 6, 10, 4, 0xD97706);
            hal_fill_rect(x + cw - 6,  y + ch - 12, 4, 10, 0xD97706);
            hal_fill_rect(x + cw - 20, y + 2, 6, ch - 4, 0x451A03);
            hal_fill_rect(x + cw - 22, cy - 5, 10, 10, 0xF59E0B);
            break;
        case 9:
            ui_fill_round_rect(x, y, cw, ch, 6, 0xF5EBD6);
            hal_fill_circle(cx, y + ch / 3, ch / 6, 0xDC2626);
            hal_fill_rect(x + 1, cy + 4, cw - 2, ch - (cy - y) - 5, 0x1D4ED8);
            for (int wx = x + 6; wx < x + cw - 6; wx += 14) {
                hal_fill_circle(wx, cy + 6, 7, 0x2563EB);
                hal_fill_circle(wx + 4, cy + 18, 7, 0x1E3A8A);
                hal_draw_circle(wx, cy + 6, 7, 0xE0F2FE);
            }
            break;
        case 10:
            ui_fill_round_rect(x, y, cw, ch, 6, 0xF4F1EA);
            hal_fill_rect(x + 2, y + 2, cw / 2, ch / 2, 0xDC2626);
            hal_fill_rect(cx, cy, cw / 2 - 2, ch / 2 - 2, 0x1D4ED8);
            hal_fill_circle(cx, cy, ch / 5, 0xFACC15);
            hal_fill_rect(x + 8, y + ch - 16, cw - 16, 4, 0x111827);
            break;
        case 11:
            ui_fill_round_rect(x, y, cw, ch, 6, 0xEFEFEF);
            for (int dy = 0; dy < ch / 3; dy++) {
                int hw = (dy * 3) / 2, x0 = cx - 10 - hw, x1 = cx - 10 + hw;
                if (x0 < x + 1) x0 = x + 1;
                if (x1 > x + cw - 2) x1 = x + cw - 2;
                hal_fill_rect(x0, y + ch - ch / 3 + dy, x1 - x0, 1, 0x9CA3AF);
            }
            for (int dy = 0; dy < ch / 4; dy++) {
                int hw = dy * 2, x0 = cx + 10 - hw, x1 = cx + 10 + hw;
                if (x0 < x + 1) x0 = x + 1;
                if (x1 > x + cw - 2) x1 = x + cw - 2;
                hal_fill_rect(x0, y + ch - ch / 4 + dy, x1 - x0, 1, 0x374151);
            }
            hal_fill_rect(x + 12, y + 14, 10, 12, 0xB91C1C);
            hal_draw_rect(x + 14, y + 16, 6, 8, 0xFEE2E2);
            break;
        case 12:
            ui_fill_round_rect(x, y, cw, ch, 6, 0xFCE7F3);
            hal_fill_rect(x, y + 2, 8, ch - 4, 0xF472B6);
            hal_fill_rect(x + 8, y + ch - 24, cw - 10, 14, 0xDB2777);
            hal_fill_circle(cx - 8, cy - 10, 6, 0xF9A8D4);
            hal_fill_circle(cx + 12, cy - 18, 5, 0xF472B6);
            hal_fill_circle(cx + 6, cy + 6, 7, 0xF9A8D4);
            hal_fill_circle(cx - 12, cy + 12, 4, 0xEC4899);
            break;
        case 13:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x1F1218);
            hal_fill_rect(x, y + 2, 8, ch - 4, 0x4C0519);
            ui_draw_round_rect(x + 12, y + 10, cw - 18, ch - 20, 10, 0x9F1239);
            hal_fill_rect(cx + 1, y + 20, 4, ch - 42, 0xBE123C);
            hal_fill_rect(cx - 10, y + ch / 3 + 4, 26, 4, 0xBE123C);
            break;
        case 14:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x14532D);
            for (int gx = x + 10; gx < x + cw - 4; gx += 16) hal_fill_rect(gx, y + 2, 3, ch - 4, 0x1E3A8A);
            for (int gy = y + 10; gy < y + ch - 4; gy += 16) hal_fill_rect(x + 2, gy, cw - 4, 2, 0xB91C1C);
            ui_fill_round_rect(cx - 14, cy - 16, 28, 32, 6, 0xFEF3C7);
            ui_draw_round_rect(cx - 14, cy - 16, 28, 32, 6, 0xD97706);
            break;
        case 15:
            ui_fill_round_rect(x, y, cw, ch, 6, 0xFACC15);
            hal_fill_rect(x + 2, y + 2, cw - 4, ch / 3, 0x06B6D4);
            hal_fill_circle(cx, cy + 6, ch / 4, 0xEC4899);
            hal_draw_circle(cx, cy + 6, ch / 4 + 3, 0x111827);
            break;
        case 16:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x7C2D12);
            for (int dy = 2; dy < ch - 2; dy++) {
                int p = (dy * 100) / ch;
                uint32_t rc = (p < 50) ? ui_blend_color(0x312E81, 0xEA580C, p * 2) : ui_blend_color(0xEA580C, 0xFACC15, (p - 50) * 2);
                hal_fill_rect(x + 2, y + dy, cw - 4, 1, rc);
            }
            hal_fill_circle(cx, cy + 6, ch / 6, 0xFEF9C3);
            break;
        case 17:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x064E3B);
            hal_fill_rect(x, y + 3, 8, ch - 6, 0x022C22);
            ui_draw_round_rect(x + 12, y + 8, cw - 18, ch - 16, 4, 0x34D399);
            hal_fill_circle(cx + 3, cy, ch / 6, 0x10B981);
            hal_fill_circle(cx + 3, cy, ch / 10, 0x064E3B);
            break;
        case 18:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x7F1D1D);
            hal_fill_rect(x, y + 3, 9, ch - 6, 0x450A0A);
            ui_draw_round_rect(x + 12, y + 8, cw - 18, ch - 16, 4, 0xF59E0B);
            hal_fill_rect(cx - 10, cy + 2, 24, 6, 0xFACC15);
            hal_fill_rect(cx - 10, cy - 6, 4, 8, 0xFACC15);
            hal_fill_rect(cx,      cy - 9, 4, 11, 0xFACC15);
            hal_fill_rect(cx + 10, cy - 6, 4, 8, 0xFACC15);
            break;
        case 19:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x334155);
            for (int gy = y + 4; gy < y + ch - 4; gy += 4) hal_fill_rect(x + 2, gy, cw - 4, 1, 0x475569);
            hal_fill_circle(x + 8, y + 8, 2, 0xCBD5E1);
            hal_fill_circle(x + cw - 8, y + 8, 2, 0xCBD5E1);
            hal_fill_circle(x + 8, y + ch - 8, 2, 0xCBD5E1);
            hal_fill_circle(x + cw - 8, y + ch - 8, 2, 0xCBD5E1);
            hal_fill_rect(x + 10, cy - 6, cw - 20, 12, 0x0F172A);
            hal_draw_rect(x + 10, cy - 6, cw - 20, 12, 0x38BDF8);
            break;
        case 20:
            ui_fill_round_rect(x, y, cw, ch, 6, 0xFFFFFF);
            ui_draw_round_rect(x, y, cw, ch, 6, 0x111827);
            hal_fill_rect(x + 5, y + 5, cw - 10, ch / 3, 0xEF4444);
            hal_draw_rect(x + 5, y + 5, cw - 10, ch / 3, 0x111827);
            hal_fill_rect(x + 5, y + ch / 3 + 9, (cw - 14) / 2, ch / 2, 0xFACC15);
            hal_draw_rect(x + 5, y + ch / 3 + 9, (cw - 14) / 2, ch / 2, 0x111827);
            hal_fill_rect(x + 9 + (cw - 14) / 2, y + ch / 3 + 9, (cw - 14) / 2, ch / 2, 0x38BDF8);
            hal_draw_rect(x + 9 + (cw - 14) / 2, y + ch / 3 + 9, (cw - 14) / 2, ch / 2, 0x111827);
            break;
        case 21:
            ui_fill_round_rect(x, y, cw, ch, 6, 0x05140C);
            for (int gx = x + 10; gx < x + cw - 6; gx += 10) {
                int len = 18 + ((gx * 7) % (ch / 2));
                hal_fill_rect(gx, y + 8, 2, len, 0x16A34A);
                hal_fill_rect(gx, y + 8 + len, 2, 4, 0x86EFAC);
            }
            ui_draw_round_rect(x, y, cw, ch, 6, 0x22C55E);
            break;
        case 22:
            ui_fill_round_rect(x, y, cw, ch, 6, 0xF8FAFC);
            ui_draw_round_rect(x, y, cw, ch, 6, 0xCBD5E1);
            hal_fill_rect(x + 12, y + 18, cw - 24, 3, 0xEA580C);
            hal_fill_rect(x + 12, y + 26, (cw - 24) / 2, 2, 0x94A3B8);
            hal_fill_circle(cx, y + ch - 24, 10, 0xE2E8F0);
            break;
        case 23:
            ui_fill_round_rect(x, y, cw, ch, 6, 0xE5E7EB);
            hal_draw_line(x + 4, y + 16, x + cw - 6, y + ch / 2, 0xD1D5DB);
            hal_draw_line(x + 6, y + ch / 2, x + cw - 4, y + ch - 12, 0x9CA3AF);
            hal_draw_line(x + 12, y + 4, x + cw - 10, y + ch - 8, 0xD4AF37);
            ui_draw_round_rect(x + 7, y + 7, cw - 14, ch - 14, 3, 0xD4AF37);
            break;
        default:
            ui_fill_round_rect(x, y, cw, ch, 6, 0xD6C3A5);
            hal_fill_rect(x, y + 2, 10, ch - 4, 0x6F4E37);
            hal_fill_rect(x + 14, y + 14, cw - 22, ch / 3, 0x4B3621);
            hal_fill_rect(x + 14, y + 14 + ch / 3 + 6, cw - 28, 3, 0x8B5A2B);
            hal_fill_rect(x + 14, y + 14 + ch / 3 + 14, cw - 36, 2, 0x8B5A2B);
            break;
    }

    const char *tag = book_format_tag(fmt);
    int bw = ui_ascii_width(tag, 1) + 12;
    ui_draw_badge(x + w - bw - 6, y + 6, tag, ui_format_color(fmt), 0xFFFFFF);
}
