/*
 * 电台界面实现。
 *
 * 播放页（240x240）——"仪表卡片 + 文字栈"：
 *
 *   y   3..23   [上海 03/10]           82%  12:34      顶栏：来源·频道 / 电量 / 时钟
 *   y  30..142  ┌────────────────────────────────┐
 *               │ ● 正在播放            [12:34]   │ 状态药丸（左）+ 睡眠定时（右）
 *               │            101.7               │ 大号读数（rubik_bold_64）
 *               │  88 ━━━━━●━━━━━  108           │ 刻度盘 + 已调谐渐变 + 指针
 *               └────────────────────────────────┘
 *   y 148..166  台名（目录原名，超宽循环滚播）
 *   y 168..186  副信息（流派 · 编码 · 码率）
 *   y 200..218  操作提示（暗灰）
 *
 * 子页面（菜单 / 台单 / 城市 / 音量）挂在同一个根对象下，用 HIDDEN 切换。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#define TAG "radio.ui"
#include "lisa_log.h"

#include <lvgl.h>

#include "lisa_ui_fonts.h"
#include "lisa_ui_nav_scr.h"
#include "listen_system.h"
#include "battery/battery.h"

#include "radio_app.h"
#include "radio_player.h"
#include "radio_ui.h"

#define RADIO_UI_SCR_ID 92

/* 刷新周期：指针缓动用它驱动 */
#define RADIO_UI_TICK_MS 100

/* 台单/城市列表一屏显示几行 */
#define RADIO_UI_ROWS 4

/* ==================== 配色 ==================== */

#define C_BG         0x071017
#define C_BG_TOP     0x0B1A22 /* 背景竖向渐变的顶部色 */
#define C_PANEL      0x0D1B23
#define C_PANEL_SOFT 0x11262E
#define C_CARD       0x10212A /* 主读数卡片底色 */
#define C_CARD_TOP   0x17303B /* 卡片竖向渐变顶部 */
#define C_CARD_LINE  0x1E3A46 /* 卡片顶部 1px 高光 */
#define C_GRID       0x24404A
#define C_GRID_DIM   0x1A3038
#define C_TICK       0x345460
#define C_TICK_DIM   0x243C46
#define C_TEXT       0xF4F0DE
#define C_MUTED      0x849BA0
#define C_MUTED_DIM  0x5E7278
#define C_FOOT       0x4A5E66
#define C_AMBER      0xFFB74D
#define C_AMBER_SOFT 0x7F5A29
#define C_GREEN      0x4ED39A
#define C_RED        0xFF5D62

/* ==================== 播放页几何 ==================== */

#define CARD_X 10
#define CARD_Y 30
#define CARD_W 220
#define CARD_H 112
#define CARD_R 14

#define HERO_Y   36
#define NAME_Y   150
#define META_Y   178
#define HINT_Y   206

#define DIAL_X0        26
#define DIAL_X1        214
#define DIAL_TRACK_Y   132
#define DIAL_NEEDLE_W  3
#define DIAL_NEEDLE_TOP 110
#define DIAL_NEEDLE_H  22
#define DIAL_TICK_COUNT 41

/* 列表页几何：一行两项（台名 + 流派/码率） */
#define ROW_Y0 34
#define ROW_H  42
#define ROW_X  10
#define ROW_W  220
#define ROW_BOX_H 40

/* ==================== 控件句柄 ==================== */

static lv_obj_t *s_scr;

/* 播放页 */
static lv_obj_t *s_page_player;
static lv_obj_t *s_lbl_chip;
static lv_obj_t *s_lbl_clock;
static lv_obj_t *s_lbl_batt;
static lv_obj_t *s_lbl_hero;
static lv_obj_t *s_dial_on;
static lv_obj_t *s_dial_off;
static lv_obj_t *s_needle;
static lv_obj_t *s_lbl_name;
static lv_obj_t *s_lbl_meta;
static lv_obj_t *s_lbl_hint;

/* 菜单页 */
static lv_obj_t *s_page_menu;
static lv_obj_t *s_menu_row[RADIO_MENU_COUNT];
static lv_obj_t *s_menu_name[RADIO_MENU_COUNT];
static lv_obj_t *s_menu_value[RADIO_MENU_COUNT];

/* 台单页 */
static lv_obj_t *s_page_list;
static lv_obj_t *s_lbl_list_title;
static lv_obj_t *s_list_row[RADIO_UI_ROWS];
static lv_obj_t *s_list_text[RADIO_UI_ROWS];
static lv_obj_t *s_list_meta[RADIO_UI_ROWS];

/* 城市页 */
static lv_obj_t *s_page_city;
static lv_obj_t *s_city_row[RADIO_UI_ROWS];
static lv_obj_t *s_city_text[RADIO_UI_ROWS];

/* 音量页 */
static lv_obj_t *s_page_volume;
static lv_obj_t *s_lbl_volume_value;
static lv_obj_t *s_volume_bar_bg;
static lv_obj_t *s_volume_bar_fill;

static lv_timer_t *s_timer;

/* 渲染侧缓存 */
static uint8_t s_rendered_page = 0xFF;
static int16_t s_needle_x = (DIAL_X0 + DIAL_X1) / 2;
static int16_t s_needle_target = -1;
static uint8_t s_clock_sec = 0xFF;

/* ==================== 小工具 ==================== */

static lv_obj_t *ui_box(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                        uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    return obj;
}

static lv_obj_t *ui_label(lv_obj_t *parent, const char *text, uint32_t color,
                          const lv_font_t *font)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    return lbl;
}

static void ui_set_text(lv_obj_t *lbl, const char *text)
{
    if (lbl == NULL || text == NULL) {
        return;
    }
    const char *current = lv_label_get_text(lbl);
    if (current != NULL && strcmp(current, text) == 0) {
        return;
    }
    lv_label_set_text(lbl, text);
}

static void ui_set_color(lv_obj_t *obj, uint32_t color)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
}

static void ui_set_bg(lv_obj_t *obj, uint32_t color)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
}

/* 带裁剪的单行标签：必须显式给高度。
 * chinese_16 的 line_height 是 18，但 CJK 字模实际画到 +19/+20，
 * 只给一行高度的话上下会被裁掉 1~2 px（真机上看就是“字被切了”）。 */
static lv_obj_t *ui_clip_label(lv_obj_t *parent, const char *text, uint32_t color,
                               const lv_font_t *font, int width)
{
    lv_obj_t *lbl = ui_label(parent, text, color, font);

    lv_obj_set_width(lbl, width);
    lv_obj_set_height(lbl, 20);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    return lbl;
}

static lv_obj_t *ui_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);

    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, 240, 240);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

/* 子页统一的小标题 */
static void ui_page_title(lv_obj_t *page, const char *text)
{
    lv_obj_t *title = ui_label(page, text, C_AMBER, &lv_font_chinese_16);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 3);
    ui_box(page, 12, 28, 216, 1, 0, C_GRID_DIM);
}

static void ui_page_hint(lv_obj_t *page, const char *text)
{
    lv_obj_t *hint = ui_label(page, text, C_MUTED_DIM, &lv_font_chinese_16);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 206);
}

/* ==================== 页面构建 ==================== */

static void build_player(lv_obj_t *root)
{
    s_page_player = ui_page(root);

    /* 顶栏：只有频道号、电量、时钟（来源城市放到倒数第二排开头） */
    s_lbl_chip = ui_label(s_page_player, "01/06", C_AMBER, &lv_font_chinese_16);
    lv_obj_align(s_lbl_chip, LV_ALIGN_TOP_LEFT, 12, 3);

    s_lbl_clock = ui_label(s_page_player, "--:--", C_TEXT, &lv_font_chinese_16);
    lv_obj_align(s_lbl_clock, LV_ALIGN_TOP_RIGHT, -12, 3);

    s_lbl_batt = ui_label(s_page_player, "--%", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_batt, LV_ALIGN_TOP_RIGHT, -64, 3);

    /* 仪表卡片：竖向微渐变 + 顶部 1px 高光。
     * LVGL 的 VER 渐变从 bg_color（顶）流到 bg_grad_color（底），所以“上亮下暗”。 */
    lv_obj_t *card = ui_box(s_page_player, CARD_X, CARD_Y, CARD_W, CARD_H, CARD_R, C_CARD_TOP);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, LV_PART_MAIN);
    ui_box(card, 14, 1, CARD_W - 28, 1, 0, C_CARD_LINE);

    /* 睡眠定时不单独占位：开了定时就把左上角的频道号换成倒计时（见 refresh_player） */

    /* 主读数：rubik 64 只有数字和 '.'，所以只放频率或频道号。
     * 播放状态只看它的颜色：在播=琥珀 / 连接中=暗琥珀 / 暂停=灰。 */
    s_lbl_hero = ui_label(s_page_player, "01", C_AMBER, &lv_font_rubik_bold_64);
    lv_obj_align(s_lbl_hero, LV_ALIGN_TOP_MID, 0, HERO_Y);

    /* 刻度：41 根，0.5MHz 间隔；每 8 根长刻度（4MHz），每 2 根中刻度（1MHz）。
     * 不摆 88/108 数字：大字号读数已经把频率说清楚了，再塞两个数字会和大数字撞。 */
    for (int i = 0; i < DIAL_TICK_COUNT; ++i) {
        const bool major = (i % 8 == 0);
        const bool medium = (i % 2 == 0);
        if (!medium) {
            continue;
        }
        const int x = DIAL_X0 + i * (DIAL_X1 - DIAL_X0) / (DIAL_TICK_COUNT - 1);
        const int h = major ? 9 : 5;
        ui_box(s_page_player, x - (major ? 1 : 0), DIAL_TRACK_Y - 3 - h, major ? 2 : 1, h, 0,
               major ? C_TICK : C_TICK_DIM);
    }

    /* 轨道：已调谐段（暗琥珀→琥珀渐变）+ 未调谐段（网格），随指针位置改宽度 */
    s_dial_on = ui_box(s_page_player, DIAL_X0, DIAL_TRACK_Y, 1, 2, 1, C_AMBER_SOFT);
    lv_obj_set_style_bg_grad_color(s_dial_on, lv_color_hex(C_AMBER), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_dial_on, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    s_dial_off = ui_box(s_page_player, DIAL_X0 + 1, DIAL_TRACK_Y,
                        DIAL_X1 - DIAL_X0, 2, 1, C_GRID);

    s_needle = ui_box(s_page_player, 0, DIAL_NEEDLE_TOP, DIAL_NEEDLE_W, DIAL_NEEDLE_H, 1,
                      C_AMBER);

    /* 卡片下方：台名 / 来源·流派·码率 / 操作提示，左对齐一条竖线对齐 */
    s_lbl_name = ui_label(s_page_player, "等待网络", C_TEXT, &lv_font_chinese_16);
    lv_obj_set_width(s_lbl_name, CARD_W - 4);
    lv_obj_set_height(s_lbl_name, 20);
    lv_label_set_long_mode(s_lbl_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_lbl_name, LV_ALIGN_TOP_LEFT, CARD_X + 6, NAME_Y);

    s_lbl_meta = ui_clip_label(s_page_player, "网络电台", C_MUTED, &lv_font_chinese_16,
                               CARD_W - 4);
    lv_obj_align(s_lbl_meta, LV_ALIGN_TOP_LEFT, CARD_X + 6, META_Y);

    s_lbl_hint = ui_label(s_page_player, "单击换台 双击暂停 长按设置", C_FOOT,
                          &lv_font_chinese_16);
    lv_obj_align(s_lbl_hint, LV_ALIGN_TOP_LEFT, CARD_X + 6, HINT_Y);
}

static void build_menu(lv_obj_t *root)
{
    s_page_menu = ui_page(root);
    ui_page_title(s_page_menu, "电台设置");
    ui_page_hint(s_page_menu, "单击选择 双击确认 三击返回");

    static const char *const names[RADIO_MENU_COUNT] = {"台单", "城市", "睡眠定时", "音量"};
    for (int i = 0; i < RADIO_MENU_COUNT; ++i) {
        const int y = ROW_Y0 + i * ROW_H;
        s_menu_row[i] = ui_box(s_page_menu, ROW_X, y, ROW_W, ROW_BOX_H, 8,
                               (i == 0) ? C_CARD_TOP : C_PANEL);
        s_menu_name[i] = ui_label(s_menu_row[i], names[i], C_TEXT, &lv_font_chinese_16);
        lv_obj_align(s_menu_name[i], LV_ALIGN_LEFT_MID, 12, 0);
        s_menu_value[i] = ui_label(s_menu_row[i], "", C_MUTED, &lv_font_chinese_16);
        lv_obj_align(s_menu_value[i], LV_ALIGN_RIGHT_MID, -12, 0);
    }
}

static void build_list(lv_obj_t *root)
{
    s_page_list = ui_page(root);
    ui_page_title(s_page_list, "台单");
    ui_page_hint(s_page_list, "单击选择 双击播放 三击返回");

    s_lbl_list_title = ui_label(s_page_list, "", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_list_title, LV_ALIGN_TOP_RIGHT, -12, 3);

    for (int i = 0; i < RADIO_UI_ROWS; ++i) {
        const int y = ROW_Y0 + i * ROW_H;
        s_list_row[i] = ui_box(s_page_list, ROW_X, y, ROW_W, ROW_BOX_H, 8, C_PANEL);

        s_list_text[i] = ui_clip_label(s_list_row[i], "", C_TEXT, &lv_font_chinese_16,
                                       ROW_W - 16);
        lv_obj_align(s_list_text[i], LV_ALIGN_TOP_LEFT, 10, 1);

        s_list_meta[i] = ui_clip_label(s_list_row[i], "", C_MUTED, &lv_font_chinese_16,
                                       ROW_W - 16);
        lv_obj_align(s_list_meta[i], LV_ALIGN_TOP_LEFT, 10, 19);
    }
}

static void build_city(lv_obj_t *root)
{
    s_page_city = ui_page(root);
    ui_page_title(s_page_city, "城市电台");
    ui_page_hint(s_page_city, "单击选择 双击确认 三击返回");

    for (int i = 0; i < RADIO_UI_ROWS; ++i) {
        const int y = ROW_Y0 + i * ROW_H;
        s_city_row[i] = ui_box(s_page_city, ROW_X, y, ROW_W, ROW_BOX_H, 8, C_PANEL);
        s_city_text[i] = ui_label(s_city_row[i], "", C_TEXT, &lv_font_chinese_16);
        lv_obj_align(s_city_text[i], LV_ALIGN_LEFT_MID, 12, 0);
    }
}

static void build_volume(lv_obj_t *root)
{
    s_page_volume = ui_page(root);
    ui_page_title(s_page_volume, "音量");
    ui_page_hint(s_page_volume, "单击加 5 双击返回");

    s_lbl_volume_value = ui_label(s_page_volume, "50", C_TEXT, &lv_font_chinese_16);
    lv_obj_align(s_lbl_volume_value, LV_ALIGN_TOP_MID, 0, 80);

    s_volume_bar_bg = ui_box(s_page_volume, 20, 120, 200, 12, 6, C_PANEL);
    s_volume_bar_fill = ui_box(s_page_volume, 22, 122, 100, 8, 4, C_AMBER);
}

/* ==================== 刷新 ==================== */

static int dial_x_for_station(uint16_t freq_decihz, uint8_t index, uint8_t count)
{
    const int span = DIAL_X1 - DIAL_X0;

    if (freq_decihz > 0) {
        int ratio = ((int)freq_decihz - RADIO_BAND_LOW_DECIHZ) * 1000 /
                    (RADIO_BAND_HIGH_DECIHZ - RADIO_BAND_LOW_DECIHZ);
        if (ratio < 0) {
            ratio = 0;
        } else if (ratio > 1000) {
            ratio = 1000;
        }
        return DIAL_X0 + span * ratio / 1000;
    }
    if (count <= 1) {
        return (DIAL_X0 + DIAL_X1) / 2;
    }
    /* 没有真实频率时按序号铺开，并留出两端余量 */
    const int inset = 6;
    const int lo = DIAL_X0 + inset;
    const int hi = DIAL_X1 - inset;
    return lo + (hi - lo) * (int)index / (int)(count - 1);
}

static void apply_needle(void)
{
    if (s_needle == NULL || s_dial_on == NULL || s_dial_off == NULL) {
        return;
    }
    const int x = s_needle_x;

    lv_obj_set_pos(s_needle, x - DIAL_NEEDLE_W / 2, DIAL_NEEDLE_TOP);

    int on_w = x - DIAL_X0;
    if (on_w < 1) {
        on_w = 1;
    }
    lv_obj_set_size(s_dial_on, on_w, 2);

    int off_x = x + 1;
    int off_w = DIAL_X1 - off_x;
    if (off_w < 0) {
        off_w = 0;
        off_x = DIAL_X1;
    }
    lv_obj_set_pos(s_dial_off, off_x, DIAL_TRACK_Y);
    lv_obj_set_size(s_dial_off, off_w, 2);
}

static void apply_clock(void)
{
    struct tm calendar;

    if (s_lbl_clock == NULL) {
        return;
    }
    if (ls_sys_get_localtime(&calendar) != 0) {
        ui_set_text(s_lbl_clock, "--:--");
        s_clock_sec = 0xFF;
        return;
    }
    if ((uint8_t)calendar.tm_sec == s_clock_sec) {
        return;
    }
    s_clock_sec = (uint8_t)calendar.tm_sec;

    char text[8];
    snprintf(text, sizeof(text), "%02d:%02d", calendar.tm_hour, calendar.tm_min);
    ui_set_text(s_lbl_clock, text);
}

/* 电量：跟时钟一起 1 s 刷一次（ADC 采样值变化没这么快） */
static void apply_battery(void)
{
    if (s_lbl_batt == NULL) {
        return;
    }

    const uint8_t pct = battery_get_pct_raw();
    char text[12];
    snprintf(text, sizeof(text), "%u%%", (unsigned)pct);
    ui_set_text(s_lbl_batt, text);

    uint32_t color = C_TEXT;
    if (battery_get_status() == BATTERY_STATUS_CHARGING) {
        color = C_GREEN; /* 充电中：绿色，不额外占字模 */
    } else if (pct < 20) {
        color = C_RED;
    } else if (pct < 50) {
        color = C_AMBER;
    }
    ui_set_color(s_lbl_batt, color);
}

static uint32_t hero_color(const radio_ui_model_t *m)
{
    switch ((radio_pb_state_t)m->playback) {
    case RADIO_PB_PLAYING:
        return C_AMBER;
    case RADIO_PB_ERROR:
    case RADIO_PB_CONNECTING:
        return C_AMBER_SOFT;
    default:
        return C_MUTED;
    }
}

static void format_mmss(uint32_t ms, char *out, size_t capacity)
{
    const uint32_t total = (ms + 999u) / 1000u;
    snprintf(out, capacity, "%u:%02u", (unsigned)(total / 60u), (unsigned)(total % 60u));
}

/* 刻度条（指针 + 已调谐段）跟大数字同色系：暂停时一并变灰，
 * 否则数字灰了、刻度还亮着，看起来像还在播。 */
static void apply_dial_color(uint32_t hero)
{
    uint32_t needle;
    uint32_t base;
    uint32_t grad;

    if (hero == C_AMBER) {
        needle = C_AMBER;
        base = C_AMBER_SOFT;
        grad = C_AMBER;
    } else if (hero == C_AMBER_SOFT) {
        needle = C_AMBER_SOFT;
        base = C_GRID;
        grad = C_AMBER_SOFT;
    } else {
        needle = C_MUTED_DIM;
        base = C_GRID;
        grad = C_MUTED_DIM;
    }

    ui_set_bg(s_needle, needle);
    if (s_dial_on != NULL) {
        lv_obj_set_style_bg_color(s_dial_on, lv_color_hex(base), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(s_dial_on, lv_color_hex(grad), LV_PART_MAIN);
    }
}

static void refresh_player(const radio_ui_model_t *m)
{
    char text[96];

    /* 左上角：没开定时显示频道号，开了定时换成倒计时（不写“定时”两字） */
    if (m->sleep_left_ms > 0) {
        format_mmss(m->sleep_left_ms, text, sizeof(text));
        ui_set_text(s_lbl_chip, text);
        ui_set_color(s_lbl_chip, C_AMBER);
    } else {
        snprintf(text, sizeof(text), "%02u/%02u", (unsigned)(m->station_index + 1u),
                 (unsigned)(m->station_count == 0 ? 1u : m->station_count));
        ui_set_text(s_lbl_chip, text);
        ui_set_color(s_lbl_chip, C_AMBER);
    }

    /* 台名：优先用目录原名（信息更多，超宽会循环滚播） */
    ui_set_text(s_lbl_name,
                (m->station_full[0] != '\0') ? m->station_full :
                ((m->station_name[0] != '\0') ? m->station_name :
                 (m->network_ok ? "没有可用电台" : "等待网络")));

    /* 倒数第二排：来源城市在前，后面接流派/编码/码率；
     * 搜台/出错时这一排改成提示，用颜色区分（不额外占一行）。 */
    if (m->detail[0] != '\0') {
        ui_set_text(s_lbl_meta, m->detail);
        ui_set_color(s_lbl_meta, C_RED);
    } else if (m->busy) {
        ui_set_text(s_lbl_meta, "正在搜索电台 …");
        ui_set_color(s_lbl_meta, C_AMBER);
    } else {
        const char *source = (m->source[0] != '\0') ? m->source : "内置";
        const char *desc = (m->station_desc[0] != '\0') ? m->station_desc : "网络电台";
        snprintf(text, sizeof(text), "%s · %s", source, desc);
        ui_set_text(s_lbl_meta, text);
        ui_set_color(s_lbl_meta, C_MUTED);
    }

    /* 主读数：有频率显示频率，没有就显示频道号；颜色表达播放状态 */
    char freq[12] = {0};
    radio_catalog_format_frequency(m->freq_decihz, freq, sizeof(freq));
    if (freq[0] != '\0') {
        ui_set_text(s_lbl_hero, freq);
    } else {
        snprintf(text, sizeof(text), "%02u", (unsigned)(m->station_index + 1u));
        ui_set_text(s_lbl_hero, text);
    }
    ui_set_color(s_lbl_hero, hero_color(m));
    apply_dial_color(hero_color(m));

    s_needle_target = (int16_t)dial_x_for_station(m->freq_decihz, m->station_index,
                                                  m->station_count);
}

static void refresh_menu(const radio_ui_model_t *m)
{
    char text[24];

    for (int i = 0; i < RADIO_MENU_COUNT; ++i) {
        const bool selected = ((uint8_t)i == m->menu_index);
        ui_set_bg(s_menu_row[i], selected ? C_CARD_TOP : C_PANEL);
        ui_set_color(s_menu_name[i], selected ? C_AMBER : C_TEXT);
        ui_set_color(s_menu_value[i], selected ? C_AMBER : C_MUTED);
    }

    snprintf(text, sizeof(text), "%u 台", (unsigned)m->station_count);
    ui_set_text(s_menu_value[RADIO_MENU_LIST], text);

    ui_set_text(s_menu_value[RADIO_MENU_CITY], radio_app_city_name(m->city_index));

    ui_set_text(s_menu_value[RADIO_MENU_SLEEP], radio_app_sleep_text(m->sleep_choice));

    snprintf(text, sizeof(text), "%u", (unsigned)m->volume);
    ui_set_text(s_menu_value[RADIO_MENU_VOLUME], text);
}

static void refresh_list(const radio_ui_model_t *m)
{
    char text[64];

    snprintf(text, sizeof(text), "%s", (m->source[0] != '\0') ? m->source : "内置");
    ui_set_text(s_lbl_list_title, text);

    /* 让选中项始终留在可视窗口内 */
    const uint8_t count = m->station_count;
    uint8_t window = (uint8_t)(m->list_index / RADIO_UI_ROWS * RADIO_UI_ROWS);
    if (window + RADIO_UI_ROWS > count && count > RADIO_UI_ROWS) {
        window = (uint8_t)(count - RADIO_UI_ROWS);
    }

    for (int i = 0; i < RADIO_UI_ROWS; ++i) {
        const uint8_t index = (uint8_t)(window + i);
        radio_station_t station;
        if (index >= count || !radio_app_get_station(index, &station)) {
            lv_obj_add_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);

        /* 第一行用目录原名（带“人民广播电台/经典947”这类信息），第二行是流派/码率 */
        snprintf(text, sizeof(text), "%02u  %s", (unsigned)(index + 1u),
                 (station.full[0] != '\0') ? station.full : station.name);
        ui_set_text(s_list_text[i], text);
        ui_set_text(s_list_meta[i], (station.desc[0] != '\0') ? station.desc : "广播电台");

        const bool selected = (index == m->list_index);
        const bool playing = (index == m->station_index) && (m->playback == RADIO_PB_PLAYING);
        ui_set_bg(s_list_row[i], selected ? C_CARD_TOP : C_PANEL);
        ui_set_color(s_list_text[i], selected ? C_AMBER : (playing ? C_GREEN : C_TEXT));
        ui_set_color(s_list_meta[i], selected ? C_AMBER_SOFT : C_MUTED_DIM);
    }
}

static void refresh_city(const radio_ui_model_t *m)
{
    const uint8_t count = radio_app_city_count();
    uint8_t window = (uint8_t)(m->city_index / RADIO_UI_ROWS * RADIO_UI_ROWS);
    if (window + RADIO_UI_ROWS > count && count > RADIO_UI_ROWS) {
        window = (uint8_t)(count - RADIO_UI_ROWS);
    }

    for (int i = 0; i < RADIO_UI_ROWS; ++i) {
        const uint8_t index = (uint8_t)(window + i);
        if (index >= count) {
            lv_obj_add_flag(s_city_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_city_row[i], LV_OBJ_FLAG_HIDDEN);
        ui_set_text(s_city_text[i], radio_app_city_name(index));

        const bool selected = (index == m->city_index);
        ui_set_bg(s_city_row[i], selected ? C_CARD_TOP : C_PANEL);
        ui_set_color(s_city_text[i], selected ? C_AMBER : C_TEXT);
    }
}

static void refresh_volume(const radio_ui_model_t *m)
{
    char text[12];

    snprintf(text, sizeof(text), "%u", (unsigned)m->volume);
    ui_set_text(s_lbl_volume_value, text);

    int width = (int)m->volume * 196 / 100;
    if (width < 4) {
        width = 4;
    }
    lv_obj_set_size(s_volume_bar_fill, width, 8);
}

static void refresh(void)
{
    radio_ui_model_t m;

    if (s_scr == NULL) {
        return;
    }
    if (!radio_app_get_model(&m)) {
        return;
    }

    /* 指针缓动：每帧走 1/3 剩余距离，最少 1px */
    if (s_needle_target >= 0) {
        const int16_t diff = (int16_t)(s_needle_target - s_needle_x);
        if (diff == 0) {
            /* 到位 */
        } else if (diff > -2 && diff < 2) {
            s_needle_x = s_needle_target;
        } else {
            s_needle_x = (int16_t)(s_needle_x + diff / 3);
        }
        apply_needle();
    }

    if (s_rendered_page != m.page) {
        s_rendered_page = m.page;
        lv_obj_add_flag(s_page_player, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_city, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_volume, LV_OBJ_FLAG_HIDDEN);
        switch ((radio_page_t)m.page) {
        case RADIO_PAGE_MENU:
            lv_obj_clear_flag(s_page_menu, LV_OBJ_FLAG_HIDDEN);
            break;
        case RADIO_PAGE_LIST:
            lv_obj_clear_flag(s_page_list, LV_OBJ_FLAG_HIDDEN);
            break;
        case RADIO_PAGE_CITY:
            lv_obj_clear_flag(s_page_city, LV_OBJ_FLAG_HIDDEN);
            break;
        case RADIO_PAGE_VOLUME:
            lv_obj_clear_flag(s_page_volume, LV_OBJ_FLAG_HIDDEN);
            break;
        case RADIO_PAGE_PLAYER:
        default:
            lv_obj_clear_flag(s_page_player, LV_OBJ_FLAG_HIDDEN);
            break;
        }
    }

    apply_clock();
    apply_battery();

    switch ((radio_page_t)m.page) {
    case RADIO_PAGE_MENU:
        refresh_menu(&m);
        break;
    case RADIO_PAGE_LIST:
        refresh_list(&m);
        break;
    case RADIO_PAGE_CITY:
        refresh_city(&m);
        break;
    case RADIO_PAGE_VOLUME:
        refresh_volume(&m);
        break;
    case RADIO_PAGE_PLAYER:
    default:
        refresh_player(&m);
        break;
    }
}

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh();
}

/* ==================== 构建 / 销毁 ==================== */

static void build(void)
{
    /* 与内置页面（home / info / ota …）一致：做成 lv_scr_act() 的子对象，
     * 而不是新建 LVGL screen。理由：导航管理器关闭页面时先 close 再 resume
     * 目标页，如果 close 删掉的是当前 active screen，LVGL 就会拿到 NULL
     * 活动屏幕（真机实测：退出电台页后界面与控制台全部卡死）。 */
    s_scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_pos(s_scr, 0, 0);
    lv_obj_set_size(s_scr, 240, 240);
    /* 竖向渐变底：上亮下暗，比纯色平铺显得有纵深，也不额外占对象 */
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_scr, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);

    build_player(s_scr);
    build_menu(s_scr);
    build_list(s_scr);
    build_city(s_scr);
    build_volume(s_scr);

    s_rendered_page = 0xFF;
    s_needle_target = -1;
    s_clock_sec = 0xFF;
    apply_needle();

    s_timer = lv_timer_create(timer_cb, RADIO_UI_TICK_MS, NULL);
    refresh();
    LOGI("radio screen built");
}

static void destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_scr != NULL) {
        lv_obj_del(s_scr);
        s_scr = NULL;
    }

    s_page_player = NULL;
    s_page_menu = NULL;
    s_page_list = NULL;
    s_page_city = NULL;
    s_page_volume = NULL;
    s_lbl_chip = NULL;
    s_lbl_clock = NULL;
    s_lbl_batt = NULL;
    s_lbl_hero = NULL;
    s_dial_on = NULL;
    s_dial_off = NULL;
    s_needle = NULL;
    s_lbl_name = NULL;
    s_lbl_meta = NULL;
    s_lbl_hint = NULL;
    for (int i = 0; i < RADIO_MENU_COUNT; ++i) {
        s_menu_row[i] = NULL;
        s_menu_name[i] = NULL;
        s_menu_value[i] = NULL;
    }
    for (int i = 0; i < RADIO_UI_ROWS; ++i) {
        s_list_row[i] = NULL;
        s_list_text[i] = NULL;
        s_list_meta[i] = NULL;
        s_city_row[i] = NULL;
        s_city_text[i] = NULL;
    }
    s_lbl_list_title = NULL;
    s_lbl_volume_value = NULL;
    s_volume_bar_bg = NULL;
    s_volume_bar_fill = NULL;
    s_needle_x = (DIAL_X0 + DIAL_X1) / 2;
    s_needle_target = -1;
    s_rendered_page = 0xFF;
}

/* ==================== 导航 ==================== */

static int radio_nav_open(const struct lisa_ui_nav_scr *scr, void **data)
{
    (void)scr;
    if (data != NULL) {
        *data = NULL;
    }
    build();
    return 0;
}

static int radio_nav_show(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    if (s_scr != NULL) {
        lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_scr);
    }
    return 0;
}

static int radio_nav_resume(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    if (s_scr != NULL) {
        lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_scr);
    }
    if (s_timer != NULL) {
        lv_timer_resume(s_timer);
        lv_timer_ready(s_timer);
    }
    return 0;
}

static int radio_nav_pause(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    if (s_scr != NULL) {
        lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_timer != NULL) {
        lv_timer_pause(s_timer);
    }
    return 0;
}

static int radio_nav_close(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    destroy();
    return 0;
}

static const struct lisa_ui_nav_scr s_radio_nav_scr = {
    .unique_id = RADIO_UI_SCR_ID,
    .open = radio_nav_open,
    .show = radio_nav_show,
    .pause = radio_nav_pause,
    .resume = radio_nav_resume,
    .close = radio_nav_close,
};

int radio_ui_init(void)
{
    const int ret = lisa_ui_nav_scr_add(&s_radio_nav_scr);
    if (ret != 0) {
        LOGE("register nav screen failed: %d", ret);
    }
    return ret;
}

void radio_ui_show(void)
{
    (void)lisa_ui_nav_scr_nav_to(RADIO_UI_SCR_ID);
}

void radio_ui_hide(void)
{
    (void)lisa_ui_nav_scr_nav_default();
}

bool radio_ui_is_visible(void)
{
    return lisa_ui_nav_scr_get_top_id() == RADIO_UI_SCR_ID;
}
