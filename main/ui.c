/*
 * 128x64 单色仪表盘。
 *
 * 布局（坐标都以屏幕左上角为原点）：
 *   y=0..16   NOW（8px 标签）+ 当前人数（16px 大数字，右对齐）
 *   y=18      1px 分隔线
 *   y=21..29  PEAK / TOT
 *   y=31..39  CH / RSSI / 帧率
 *   y=41..63  最近 42 秒的柱状图（2px 柱 + 1px 间距）
 *
 * 注意：
 *   - UNSCII 是等宽字体，8px 字号每个字符 8px 宽，一行最多 16 个字符；
 *   - 字库没有中文字形，界面只能全英文；
 *   - 所有 LVGL 调用都发生在 lv_timer 回调里（即 LVGL 任务中），不需要加锁。
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "app_config.h"
#include "people_counter.h"
#include "ui.h"
#include "wifi_sniffer.h"

#define SCR_W       128

#define ROW_LINE_Y  18
#define ROW_PEAK_Y  21
#define ROW_CH_Y    31
#define BAR_TOP     41
#define BAR_BOTTOM  63
#define BAR_W       2
#define BAR_STEP    (BAR_W + 1)
#define TEXT_COLS   16      // 8px 字号一行放得下的字符数

static lv_obj_t *s_count;       // 大数字
static lv_obj_t *s_peak;        // PEAK / TOT
static lv_obj_t *s_chan;        // CH / RSSI / 帧率
static lv_obj_t *s_bar[PC_HIST_LEN];

static uint8_t s_bar_h[PC_HIST_LEN];    // 上一帧每根柱子的高度，避免无谓重绘
static pc_snapshot_t s_prev;
static uint8_t s_prev_ch;

/* ---------------- 小工具 ---------------- */

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, text);
    return l;
}

/* 12345 -> "12.3k"，保证一行不超过 16 个字符 */
static void fmt_num(char *buf, size_t n, uint32_t v)
{
    if (v < 10000) {
        snprintf(buf, n, "%lu", (unsigned long)v);
    } else {
        snprintf(buf, n, "%lu.%luk", (unsigned long)(v / 1000), (unsigned long)((v % 1000) / 100));
    }
}

/* ---------------- 刷新 ---------------- */

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    pc_snapshot_t s;
    people_counter_get(&s);
    uint8_t ch = wifi_sniffer_channel();

    if (s.current != s_prev.current) {
        lv_label_set_text_fmt(s_count, "%u", s.current);
    }

    if (s.peak != s_prev.peak || s.total != s_prev.total) {
        char peak[16], total[16], line[64];
        fmt_num(peak, sizeof(peak), s.peak);
        fmt_num(total, sizeof(total), s.total);
        snprintf(line, sizeof(line), "PEAK %s TOT %s", peak, total);
        if (strlen(line) > TEXT_COLS) {
            snprintf(line, sizeof(line), "PEAK %s", peak);
        }
        lv_label_set_text(s_peak, line);
    }

    if (s.rate != s_prev.rate || s.rssi_avg != s_prev.rssi_avg ||
        s.current != s_prev.current || ch != s_prev_ch) {
        char chbuf[8], rssibuf[8], line[64];
        if (ch != 0) {
            snprintf(chbuf, sizeof(chbuf), "%02u", ch);
        } else {
            strcpy(chbuf, "--");
        }
        if (s.current != 0) {
            snprintf(rssibuf, sizeof(rssibuf), "%d", s.rssi_avg);
        } else {
            strcpy(rssibuf, "---");
        }
        snprintf(line, sizeof(line), "CH%s %3sdB %u/s", chbuf, rssibuf, s.rate);
        lv_label_set_text(s_chan, line);
    }

    /* 柱状图：s.hist[0] 是最新的一秒，画在最右边 */
    uint8_t hmax = s.hist_max ? s.hist_max : 1;
    for (int i = 0; i < PC_HIST_LEN; i++) {
        uint8_t h = (uint8_t)((uint32_t)s.hist[i] * (BAR_BOTTOM - BAR_TOP + 1) / hmax);
        if (s.hist[i] != 0 && h == 0) {
            h = 1;      // 有数据就至少留 1px
        }
        if (h == s_bar_h[i]) {
            continue;
        }
        s_bar_h[i] = h;
        lv_obj_set_pos(s_bar[i], SCR_W - (i + 1) * BAR_STEP, BAR_BOTTOM - h + 1);
        lv_obj_set_size(s_bar[i], BAR_W, h);
    }

    s_prev = s;
    s_prev_ch = ch;
}

/* ---------------- 建界面 ---------------- */

void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    /* 单色屏只有"亮/不亮"，所有前景色都要显式设白，别依赖默认主题 */
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    make_label(scr, "NOW", &lv_font_unscii_8, 0, 2);

    s_count = make_label(scr, "0", &lv_font_unscii_16, 0, 0);
    lv_obj_align(s_count, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_remove_style_all(line);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(line, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_pos(line, 0, ROW_LINE_Y);
    lv_obj_set_size(line, SCR_W, 1);

    s_peak = make_label(scr, "PEAK 0 TOT 0", &lv_font_unscii_8, 0, ROW_PEAK_Y);
    s_chan = make_label(scr, "CH-- ---dB 0/s", &lv_font_unscii_8, 0, ROW_CH_Y);

    /* 柱状图：一次建好 42 个方块，之后只改位置和高度 */
    for (int i = 0; i < PC_HIST_LEN; i++) {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_remove_style_all(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_pos(bar, SCR_W - (i + 1) * BAR_STEP, BAR_BOTTOM);
        lv_obj_set_size(bar, BAR_W, 0);
        s_bar[i] = bar;
    }

    lv_timer_create(refresh_cb, PC_UI_REFRESH_MS, NULL);
}
