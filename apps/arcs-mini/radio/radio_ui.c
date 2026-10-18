/*
 * 电台界面实现。布局（240x240）：
 *
 *   y   2..20   标题「网络电台」（左） + 时钟（右）
 *   y  22       分隔线
 *   y  26..42   "CH 01/06"（琥珀） + 台单来源（右，灰）
 *   y  44..60   电台名（居中）
 *   y  60..126  刻度盘面板：刻度 / 轨道 / 指针 / 88-108 标尺
 *   y 129..175  电平条面板（18 根，播放时波动）
 *   y 178..194  播放图标 + 状态行（频率 / 睡眠定时 / 错误）
 *   y 204..220  操作提示
 *
 * 子页面（菜单 / 台单 / 城市 / 音量）复用同一个屏幕对象，用 HIDDEN 切换。
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

#include "radio_app.h"
#include "radio_player.h"
#include "radio_ui.h"

#define RADIO_UI_SCR_ID 92

/* 刷新周期：指针缓动与电平条动画都靠它驱动 */
#define RADIO_UI_TICK_MS 100

/* 台单/城市列表一屏显示几行 */
#define RADIO_UI_ROWS 5

/* 电平条数量（与 leo-radio 的 kMeterCount 一致） */
#define RADIO_METER_COUNT 18

/* ==================== 配色（与 leo-radio 逐值一致） ==================== */

#define C_BG         0x071017
#define C_PANEL      0x0D1B23
#define C_PANEL_SOFT 0x11262E
#define C_GRID       0x24404A
#define C_TEXT       0xF4F0DE
#define C_MUTED      0x849BA0
#define C_AMBER      0xFFB74D
#define C_AMBER_SOFT 0x7F5A29
#define C_GREEN      0x4ED39A
#define C_RED        0xFF5D62

/* ==================== 几何 ==================== */

#define DIAL_X 13
#define DIAL_Y 60
#define DIAL_W 214
#define DIAL_H 66

#define DIAL_TRACK_LEFT  16
#define DIAL_TRACK_RIGHT 198
#define DIAL_TRACK_Y     40 /* 面板内 y */
#define DIAL_NEEDLE_W    3
#define DIAL_NEEDLE_H    22
#define DIAL_TICK_COUNT  41

#define METER_X 13
#define METER_Y 129
#define METER_W 214
#define METER_H 46
#define METER_BAR_W       6
#define METER_BAR_GAP     11
#define METER_BAR_LEFT    9
#define METER_BAR_BASE    35 /* 面板内 y：柱子的底边 */
#define METER_BAR_MAX_H   32

#define ROW_Y0     36
#define ROW_H      30
#define ROW_X      10
#define ROW_W      220

/* ==================== 控件句柄 ==================== */

static lv_obj_t *s_scr;

static lv_obj_t *s_page_player;
static lv_obj_t *s_lbl_clock;
static lv_obj_t *s_lbl_ch;
static lv_obj_t *s_lbl_source;
static lv_obj_t *s_lbl_name;
static lv_obj_t *s_lbl_freq_lo;
static lv_obj_t *s_lbl_freq_hi;
static lv_obj_t *s_needle;
static lv_obj_t *s_needle_glow;
static lv_obj_t *s_play_icon[3];
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_hint;
static lv_obj_t *s_meter[RADIO_METER_COUNT];

static lv_obj_t *s_page_menu;
static lv_obj_t *s_menu_row[RADIO_MENU_COUNT];
static lv_obj_t *s_menu_name[RADIO_MENU_COUNT];
static lv_obj_t *s_menu_value[RADIO_MENU_COUNT];

static lv_obj_t *s_page_list;
static lv_obj_t *s_lbl_list_title;
static lv_obj_t *s_list_row[RADIO_UI_ROWS];
static lv_obj_t *s_list_text[RADIO_UI_ROWS];

static lv_obj_t *s_page_city;
static lv_obj_t *s_city_row[RADIO_UI_ROWS];
static lv_obj_t *s_city_text[RADIO_UI_ROWS];

static lv_obj_t *s_page_volume;
static lv_obj_t *s_lbl_volume_value;
static lv_obj_t *s_volume_bar_bg;
static lv_obj_t *s_volume_bar_fill;

static lv_timer_t *s_timer;

/* 渲染侧的缓存，用来避免无意义的 set_text / 重排 */
static uint8_t s_rendered_page = 0xFF;
static int16_t s_needle_x = (DIAL_TRACK_LEFT + DIAL_TRACK_RIGHT) / 2;
static int16_t s_needle_target = -1;
static uint16_t s_anim_phase;
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

/* ==================== 页面构建 ==================== */

static void build_player(lv_obj_t *root)
{
    s_page_player = ui_page(root);

    /* 标题 + 时钟 */
    lv_obj_t *title = ui_label(s_page_player, "网络电台", C_AMBER, &lv_font_chinese_16);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 3);

    s_lbl_clock = ui_label(s_page_player, "--:--", C_TEXT, &lv_font_chinese_16);
    lv_obj_align(s_lbl_clock, LV_ALIGN_TOP_RIGHT, -12, 3);

    ui_box(s_page_player, 12, 22, 216, 1, 0, C_GRID);

    /* CH + 来源 */
    s_lbl_ch = ui_label(s_page_player, "CH 01/01", C_AMBER, &lv_font_chinese_16);
    lv_obj_align(s_lbl_ch, LV_ALIGN_TOP_LEFT, 12, 26);

    s_lbl_source = ui_label(s_page_player, "内置", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_source, LV_ALIGN_TOP_RIGHT, -12, 26);

    /* 电台名 */
    s_lbl_name = ui_label(s_page_player, "等待网络", C_TEXT, &lv_font_chinese_16);
    lv_obj_set_width(s_lbl_name, 216);
    lv_label_set_long_mode(s_lbl_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_lbl_name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_lbl_name, LV_ALIGN_TOP_MID, 0, 44);

    /* 刻度盘面板 */
    ui_box(s_page_player, DIAL_X, DIAL_Y, DIAL_W, DIAL_H, 8, C_PANEL);
    ui_box(s_page_player, DIAL_X, DIAL_Y, DIAL_W, 1, 0, C_GRID);
    ui_box(s_page_player, DIAL_X, DIAL_Y + DIAL_H - 1, DIAL_W, 1, 0, C_GRID);
    ui_box(s_page_player, DIAL_X, DIAL_Y, 1, DIAL_H, 0, C_GRID);
    ui_box(s_page_player, DIAL_X + DIAL_W - 1, DIAL_Y, 1, DIAL_H, 0, C_GRID);

    /* 刻度：41 根，0.5MHz 间隔；每 8 根长刻度（4MHz），每 2 根中刻度（1MHz） */
    for (int i = 0; i < DIAL_TICK_COUNT; ++i) {
        const bool major = (i % 8 == 0);
        const bool medium = (i % 2 == 0);
        if (!major && !medium) {
            continue;
        }
        const int x = DIAL_TRACK_LEFT + i * (DIAL_TRACK_RIGHT - DIAL_TRACK_LEFT) /
                                            (DIAL_TICK_COUNT - 1);
        const int h = major ? 11 : 6;
        ui_box(s_page_player, DIAL_X + x, DIAL_Y + DIAL_TRACK_Y - h, major ? 2 : 1, h, 0,
               major ? C_GRID : C_AMBER_SOFT);
    }
    /* 轨道 */
    ui_box(s_page_player, DIAL_X + DIAL_TRACK_LEFT, DIAL_Y + DIAL_TRACK_Y,
           DIAL_TRACK_RIGHT - DIAL_TRACK_LEFT + 1, 2, 0, C_GRID);

    /* 指针：光晕在下、本体在上 */
    s_needle_glow = ui_box(s_page_player, 0, 0, 18, 8, 2, C_AMBER_SOFT);
    s_needle = ui_box(s_page_player, 0, 0, DIAL_NEEDLE_W, DIAL_NEEDLE_H, 1, C_AMBER);

    /* 标尺两端 */
    s_lbl_freq_lo = ui_label(s_page_player, "88", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_freq_lo, LV_ALIGN_TOP_LEFT, DIAL_X + DIAL_TRACK_LEFT - 2, DIAL_Y + 6);
    s_lbl_freq_hi = ui_label(s_page_player, "108", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_freq_hi, LV_ALIGN_TOP_LEFT, DIAL_X + DIAL_TRACK_RIGHT - 18, DIAL_Y + 6);

    /* 电平条面板 */
    ui_box(s_page_player, METER_X, METER_Y, METER_W, METER_H, 6, C_PANEL_SOFT);
    ui_box(s_page_player, METER_X, METER_Y, METER_W, 1, 0, C_GRID);
    ui_box(s_page_player, METER_X, METER_Y + METER_H - 1, METER_W, 1, 0, C_GRID);
    for (int i = 0; i < RADIO_METER_COUNT; ++i) {
        s_meter[i] = ui_box(s_page_player,
                            METER_X + METER_BAR_LEFT + i * METER_BAR_GAP,
                            METER_Y + METER_BAR_BASE - 3, METER_BAR_W, 3, 1, C_GRID);
    }

    /* 播放 / 暂停图标（矩形拼的三角形 / 双竖条，随状态切换） */
    for (int i = 0; i < 3; ++i) {
        s_play_icon[i] = ui_box(s_page_player, 13, 182, 4, 12, 0, C_AMBER);
        lv_obj_add_flag(s_play_icon[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_lbl_status = ui_label(s_page_player, "等待网络", C_MUTED, &lv_font_chinese_16);
    lv_obj_set_width(s_lbl_status, 190);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_LEFT, 34, 179);

    s_lbl_hint = ui_label(s_page_player, "单击换台 双击暂停 长按设置", C_MUTED,
                          &lv_font_chinese_16);
    lv_obj_align(s_lbl_hint, LV_ALIGN_TOP_MID, 0, 204);
}

static void build_menu(lv_obj_t *root)
{
    s_page_menu = ui_page(root);

    lv_obj_t *title = ui_label(s_page_menu, "电台设置", C_AMBER, &lv_font_chinese_16);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 3);

    lv_obj_t *hint = ui_label(s_page_menu, "单击选择 双击确认 三击返回", C_MUTED,
                              &lv_font_chinese_16);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 204);

    static const char *const names[RADIO_MENU_COUNT] = {"台单", "城市", "睡眠定时", "音量"};
    for (int i = 0; i < RADIO_MENU_COUNT; ++i) {
        const int y = ROW_Y0 + i * ROW_H;
        s_menu_row[i] = ui_box(s_page_menu, ROW_X, y, ROW_W, ROW_H - 4, 6, C_PANEL);
        s_menu_name[i] = ui_label(s_menu_row[i], names[i], C_TEXT, &lv_font_chinese_16);
        lv_obj_align(s_menu_name[i], LV_ALIGN_LEFT_MID, 10, 0);
        s_menu_value[i] = ui_label(s_menu_row[i], "", C_MUTED, &lv_font_chinese_16);
        lv_obj_align(s_menu_value[i], LV_ALIGN_RIGHT_MID, -10, 0);
    }
}

static void build_list(lv_obj_t *root)
{
    s_page_list = ui_page(root);

    lv_obj_t *title = ui_label(s_page_list, "台单", C_AMBER, &lv_font_chinese_16);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 3);

    s_lbl_list_title = ui_label(s_page_list, "", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_list_title, LV_ALIGN_TOP_RIGHT, -12, 3);

    lv_obj_t *hint = ui_label(s_page_list, "单击选择 双击播放 三击返回", C_MUTED,
                              &lv_font_chinese_16);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 204);

    for (int i = 0; i < RADIO_UI_ROWS; ++i) {
        const int y = ROW_Y0 + i * ROW_H;
        s_list_row[i] = ui_box(s_page_list, ROW_X, y, ROW_W, ROW_H - 4, 6, C_PANEL);
        s_list_text[i] = ui_label(s_list_row[i], "", C_TEXT, &lv_font_chinese_16);
        lv_obj_set_width(s_list_text[i], ROW_W - 16);
        lv_label_set_long_mode(s_list_text[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(s_list_text[i], LV_ALIGN_LEFT_MID, 10, 0);
    }
}

static void build_city(lv_obj_t *root)
{
    s_page_city = ui_page(root);

    lv_obj_t *title = ui_label(s_page_city, "城市电台", C_AMBER, &lv_font_chinese_16);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 3);

    lv_obj_t *hint = ui_label(s_page_city, "单击选择 双击确认 三击返回", C_MUTED,
                              &lv_font_chinese_16);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 204);

    for (int i = 0; i < RADIO_UI_ROWS; ++i) {
        const int y = ROW_Y0 + i * ROW_H;
        s_city_row[i] = ui_box(s_page_city, ROW_X, y, ROW_W, ROW_H - 4, 6, C_PANEL);
        s_city_text[i] = ui_label(s_city_row[i], "", C_TEXT, &lv_font_chinese_16);
        lv_obj_set_width(s_city_text[i], ROW_W - 16);
        lv_label_set_long_mode(s_city_text[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(s_city_text[i], LV_ALIGN_LEFT_MID, 10, 0);
    }
}

static void build_volume(lv_obj_t *root)
{
    s_page_volume = ui_page(root);

    lv_obj_t *title = ui_label(s_page_volume, "音量", C_AMBER, &lv_font_chinese_16);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 3);

    s_lbl_volume_value = ui_label(s_page_volume, "50", C_TEXT, &lv_font_chinese_16);
    lv_obj_align(s_lbl_volume_value, LV_ALIGN_TOP_MID, 0, 70);

    s_volume_bar_bg = ui_box(s_page_volume, 20, 110, 200, 14, 7, C_PANEL);
    s_volume_bar_fill = ui_box(s_page_volume, 22, 112, 100, 10, 5, C_AMBER);

    lv_obj_t *hint = ui_label(s_page_volume, "单击加 5 双击返回", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 204);
}

/* ==================== 刷新 ==================== */

static int dial_x_for_station(uint16_t freq_decihz, uint8_t index, uint8_t count)
{
    const int span = DIAL_TRACK_RIGHT - DIAL_TRACK_LEFT;

    if (freq_decihz > 0) {
        int ratio = ((int)freq_decihz - RADIO_BAND_LOW_DECIHZ) * 1000 /
                    (RADIO_BAND_HIGH_DECIHZ - RADIO_BAND_LOW_DECIHZ);
        if (ratio < 0) {
            ratio = 0;
        } else if (ratio > 1000) {
            ratio = 1000;
        }
        return DIAL_TRACK_LEFT + span * ratio / 1000;
    }
    if (count <= 1) {
        return (DIAL_TRACK_LEFT + DIAL_TRACK_RIGHT) / 2;
    }
    /* 没有真实频率时按序号铺开，并留出两端余量 */
    const int inset = 6;
    const int lo = DIAL_TRACK_LEFT + inset;
    const int hi = DIAL_TRACK_RIGHT - inset;
    return lo + (hi - lo) * (int)index / (int)(count - 1);
}

static void apply_needle(void)
{
    if (s_needle == NULL || s_needle_glow == NULL) {
        return;
    }
    const int x = DIAL_X + s_needle_x;
    lv_obj_set_pos(s_needle, x - DIAL_NEEDLE_W / 2, DIAL_Y + DIAL_TRACK_Y - 13);
    lv_obj_set_pos(s_needle_glow, x - 9, DIAL_Y + DIAL_TRACK_Y - 7);
}

/* 三角波 0..1，用来做光晕呼吸与电平条波动 */
static uint16_t pulse(void)
{
    uint16_t v = s_anim_phase;
    if (v > 128) {
        v = (uint16_t)(256 - v);
    }
    return v;
}

static void apply_play_icon(bool playing, bool paused)
{
    if (s_play_icon[0] == NULL) {
        return;
    }
    if (playing) {
        /* 播放：一个实心三角 */
        lv_obj_set_size(s_play_icon[0], 4, 12);
        lv_obj_set_pos(s_play_icon[0], 13, 182);
        lv_obj_set_size(s_play_icon[1], 6, 6);
        lv_obj_set_pos(s_play_icon[1], 17, 185);
        lv_obj_set_size(s_play_icon[2], 6, 2);
        lv_obj_set_pos(s_play_icon[2], 23, 187);
        lv_obj_clear_flag(s_play_icon[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_play_icon[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_play_icon[2], LV_OBJ_FLAG_HIDDEN);
    } else if (paused) {
        /* 暂停：两条竖条 */
        lv_obj_set_size(s_play_icon[0], 4, 12);
        lv_obj_set_pos(s_play_icon[0], 14, 182);
        lv_obj_set_size(s_play_icon[1], 4, 12);
        lv_obj_set_pos(s_play_icon[1], 21, 182);
        lv_obj_clear_flag(s_play_icon[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_play_icon[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_play_icon[2], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_play_icon[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_play_icon[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_play_icon[2], LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_meter(bool active)
{
    const uint16_t breath = pulse();

    for (int i = 0; i < RADIO_METER_COUNT; ++i) {
        if (s_meter[i] == NULL) {
            continue;
        }
        if (!active) {
            lv_obj_set_size(s_meter[i], METER_BAR_W, 3);
            lv_obj_set_pos(s_meter[i], METER_X + METER_BAR_LEFT + i * METER_BAR_GAP,
                           METER_Y + METER_BAR_BASE - 3);
            ui_set_bg(s_meter[i], C_GRID);
            continue;
        }
        /* 两路相位不同的三角波叠加，做出"有声音"的波动观感（读不到真实电平） */
        const uint16_t a = (uint16_t)((s_anim_phase * 3 + (uint16_t)(i * 29)) & 0xFF);
        const uint16_t b = (uint16_t)((s_anim_phase * 7 + (uint16_t)(i * 11)) & 0xFF);
        const int wa = (a > 128) ? (256 - (int)a) : (int)a;
        const int wb = (b > 128) ? (256 - (int)b) : (int)b;
        const int height = 3 + (wa * 22 + wb * 12 + breath * 6) / 256;
        lv_obj_set_size(s_meter[i], METER_BAR_W, height);
        lv_obj_set_pos(s_meter[i], METER_X + METER_BAR_LEFT + i * METER_BAR_GAP,
                       METER_Y + METER_BAR_BASE - height);
        ui_set_bg(s_meter[i], (height > 22) ? C_AMBER : C_AMBER_SOFT);
    }
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

static const char *playback_text(const radio_ui_model_t *m)
{
    switch ((radio_pb_state_t)m->playback) {
    case RADIO_PB_CONNECTING:
        return "正在连接";
    case RADIO_PB_PLAYING:
        return "正在播放";
    case RADIO_PB_ERROR:
        return "信号中断 正在重连";
    case RADIO_PB_STOPPED:
    default:
        return "已暂停";
    }
}

static uint32_t playback_color(const radio_ui_model_t *m)
{
    switch ((radio_pb_state_t)m->playback) {
    case RADIO_PB_PLAYING:
        return C_GREEN;
    case RADIO_PB_ERROR:
        return C_AMBER;
    case RADIO_PB_CONNECTING:
        return C_AMBER;
    default:
        return C_MUTED;
    }
}

static void format_mmss(uint32_t ms, char *out, size_t capacity)
{
    const uint32_t total = (ms + 999u) / 1000u;
    snprintf(out, capacity, "%u:%02u", (unsigned)(total / 60u), (unsigned)(total % 60u));
}

static void refresh_player(const radio_ui_model_t *m)
{
    char text[96];

    snprintf(text, sizeof(text), "CH %02u/%02u", (unsigned)(m->station_index + 1u),
             (unsigned)(m->station_count == 0 ? 1u : m->station_count));
    ui_set_text(s_lbl_ch, text);
    ui_set_text(s_lbl_source, (m->source[0] != '\0') ? m->source : "内置");
    ui_set_text(s_lbl_name,
                (m->station_name[0] != '\0') ? m->station_name :
                (m->network_ok ? "没有可用电台" : "等待网络"));

    /* 状态行：频率 / 睡眠定时 / 错误原因 */
    if (m->detail[0] != '\0') {
        ui_set_text(s_lbl_status, m->detail);
        ui_set_color(s_lbl_status, C_RED);
    } else {
        char freq[12] = {0};
        radio_catalog_format_frequency(m->freq_decihz, freq, sizeof(freq));
        snprintf(text, sizeof(text), "%s", playback_text(m));
        if (freq[0] != '\0') {
            size_t used = strlen(text);
            snprintf(text + used, sizeof(text) - used, " · FM%s", freq);
        }
        if (m->sleep_left_ms > 0) {
            char left[12];
            format_mmss(m->sleep_left_ms, left, sizeof(left));
            size_t used = strlen(text);
            snprintf(text + used, sizeof(text) - used, " · 定时 %s", left);
        }
        ui_set_text(s_lbl_status, text);
        ui_set_color(s_lbl_status, playback_color(m));
    }

    const bool playing = (m->playback == RADIO_PB_PLAYING);
    apply_play_icon(playing, m->playback == RADIO_PB_STOPPED && !m->busy);
    apply_meter(playing);

    const int16_t target = (int16_t)dial_x_for_station(m->freq_decihz, m->station_index,
                                                       m->station_count);
    if (target != s_needle_target) {
        s_needle_target = target;
    }
}

static void refresh_menu(const radio_ui_model_t *m)
{
    char sleep_text[16];

    for (int i = 0; i < RADIO_MENU_COUNT; ++i) {
        const bool selected = ((uint8_t)i == m->menu_index);
        ui_set_bg(s_menu_row[i], selected ? C_PANEL_SOFT : C_PANEL);
        ui_set_color(s_menu_name[i], selected ? C_AMBER : C_TEXT);
        ui_set_color(s_menu_value[i], selected ? C_AMBER : C_MUTED);
    }

    char count_text[24];
    snprintf(count_text, sizeof(count_text), "%u 台", (unsigned)m->station_count);
    ui_set_text(s_menu_value[RADIO_MENU_LIST], count_text);

    ui_set_text(s_menu_value[RADIO_MENU_CITY], radio_app_city_name(m->city_index));

    snprintf(sleep_text, sizeof(sleep_text), "%s", radio_app_sleep_text(m->sleep_choice));
    ui_set_text(s_menu_value[RADIO_MENU_SLEEP], sleep_text);

    char volume_text[12];
    snprintf(volume_text, sizeof(volume_text), "%u", (unsigned)m->volume);
    ui_set_text(s_menu_value[RADIO_MENU_VOLUME], volume_text);
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
        if (index >= count) {
            lv_obj_add_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        radio_station_t station;
        if (!radio_app_get_station(index, &station)) {
            lv_obj_add_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);

        char freq[12] = {0};
        radio_catalog_format_frequency(station.freq_decihz, freq, sizeof(freq));
        if (freq[0] != '\0') {
            snprintf(text, sizeof(text), "%02u  %s  FM%s", (unsigned)(index + 1u),
                     station.name, freq);
        } else {
            snprintf(text, sizeof(text), "%02u  %s", (unsigned)(index + 1u), station.name);
        }
        ui_set_text(s_list_text[i], text);

        const bool selected = (index == m->list_index);
        const bool playing = (index == m->station_index) && (m->playback == RADIO_PB_PLAYING);
        ui_set_bg(s_list_row[i], selected ? C_PANEL_SOFT : C_PANEL);
        ui_set_color(s_list_text[i], selected ? C_AMBER : (playing ? C_GREEN : C_TEXT));
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
        ui_set_bg(s_city_row[i], selected ? C_PANEL_SOFT : C_PANEL);
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
    lv_obj_set_size(s_volume_bar_fill, width, 10);
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

    s_anim_phase = (uint16_t)((s_anim_phase + 17u) & 0xFFu);

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
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), LV_PART_MAIN);
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
    s_lbl_clock = NULL;
    s_lbl_ch = NULL;
    s_lbl_source = NULL;
    s_lbl_name = NULL;
    s_lbl_freq_lo = NULL;
    s_lbl_freq_hi = NULL;
    s_needle = NULL;
    s_needle_glow = NULL;
    for (int i = 0; i < 3; ++i) {
        s_play_icon[i] = NULL;
    }
    s_lbl_status = NULL;
    s_lbl_hint = NULL;
    for (int i = 0; i < RADIO_METER_COUNT; ++i) {
        s_meter[i] = NULL;
    }
    for (int i = 0; i < RADIO_MENU_COUNT; ++i) {
        s_menu_row[i] = NULL;
        s_menu_name[i] = NULL;
        s_menu_value[i] = NULL;
    }
    for (int i = 0; i < RADIO_UI_ROWS; ++i) {
        s_list_row[i] = NULL;
        s_list_text[i] = NULL;
        s_city_row[i] = NULL;
        s_city_text[i] = NULL;
    }
    s_lbl_list_title = NULL;
    s_lbl_volume_value = NULL;
    s_volume_bar_bg = NULL;
    s_volume_bar_fill = NULL;
    s_needle_x = (DIAL_TRACK_LEFT + DIAL_TRACK_RIGHT) / 2;
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
