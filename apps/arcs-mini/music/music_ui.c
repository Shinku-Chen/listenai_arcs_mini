/*
 * 网易云音乐界面：独立 LVGL 子对象（挂在 lv_scr_act() 下），导航 id 93。
 *
 * 六个页面同属一个 nav screen，用 HIDDEN 切换；主菜单、歌单和设置复用菜单页。
 * 不要 lv_scr_load 独立 screen：
 * 导航管理器 close 时删掉 active screen 会让 LVGL 拿到 NULL 活动屏幕而卡死）。
 *
 * 240x240 布局（与 radio 页同一套配色与几何）：
 *
 *   y   3..21    在线  60                  12:34      顶栏：网络/音量 + 时钟
 *   y  26..126   ┌────────┐  歌名（两行）
 *                │ 封面   │  歌手
 *                └────────┘  专辑
 *   y 132..136   ━━━━━━━━━━━━━━━━━━  进度
 *   y 140..158   0:12 / 3:45
 *   y 162..182   当前歌词（居中）
 *   y 206..224   单击下一首 双击暂停 三击菜单
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#define TAG "music.ui"
#include "lisa_log.h"

#include <lvgl.h>

#include "lisa_ui_fonts.h"
#include "lisa_ui_assets.h" /* 配网二维码（与助手固件用的是同一张图） */
#include "lisa_ui_nav_scr.h"
#include "sysutils.h"
#include "sys_wifi.h"
#include "listen_system.h"

#include "music_app.h"
#include "music_ui.h"

#define MUSIC_UI_SCR_ID 93
#define MUSIC_UI_TICK_MS 100

/* 封面 JPEG 缓冲（LVGL 自己解码用，从 LVGL 的 PSRAM 堆里分） */
#define MUSIC_COVER_BUF (28 * 1024)

/* ==================== 配色（与 radio 页一致） ==================== */

#define C_BG         0x071017
#define C_BG_TOP     0x0B1A22
#define C_PANEL      0x0D1B23
#define C_CARD_TOP   0x17303B
#define C_GRID_DIM   0x1A3038
#define C_TEXT       0xF4F0DE
#define C_MUTED      0x849BA0
#define C_MUTED_DIM  0x5E7278
#define C_FOOT       0x4A5E66
#define C_AMBER      0xFFB74D
#define C_AMBER_SOFT 0x7F5A29
#define C_GREEN      0x4ED39A
#define C_RED        0xFF5D62

/* ==================== 几何 ==================== */

#define ROW_Y0    32
#define ROW_H     34
#define ROW_X     10
#define ROW_W     220
#define ROW_BOX_H 32

#define COVER_X 10
#define COVER_Y 26
#define COVER_SZ 100

#define TEXT_X 118
#define TEXT_W 114

#define BAR_X 10
#define BAR_Y 132
#define BAR_W 220
#define BAR_H 4

/* ==================== 控件 ==================== */

static lv_obj_t *s_scr;

/* 播放页 */
static lv_obj_t *s_page_player;
static lv_obj_t *s_lbl_net;
static lv_obj_t *s_lbl_vol;
static lv_obj_t *s_lbl_clock;
static lv_obj_t *s_cover_box;
static lv_obj_t *s_cover_img;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_artist;
static lv_obj_t *s_lbl_album;
static lv_obj_t *s_bar_bg;
static lv_obj_t *s_bar_fill;
static lv_obj_t *s_lbl_time;
static lv_obj_t *s_lbl_lyric;
static lv_obj_t *s_lbl_detail;
static lv_obj_t *s_lbl_hint;

/* 主菜单 / 歌单 / 设置：复用同一组列表控件 */
static lv_obj_t *s_page_menu;
static lv_obj_t *s_lbl_menu_title;
static lv_obj_t *s_lbl_menu_pos;
static lv_obj_t *s_lbl_menu_hint;
static lv_obj_t *s_menu_row[MUSIC_UI_ROWS];
static lv_obj_t *s_menu_name[MUSIC_UI_ROWS];
static lv_obj_t *s_menu_value[MUSIC_UI_ROWS];

/* 歌曲列表页 */
static lv_obj_t *s_page_list;
static lv_obj_t *s_lbl_list_title;
static lv_obj_t *s_lbl_list_pos;
static lv_obj_t *s_list_row[MUSIC_UI_ROWS];
static lv_obj_t *s_list_text[MUSIC_UI_ROWS];
static lv_obj_t *s_list_meta[MUSIC_UI_ROWS];

/* 歌词页 */
static lv_obj_t *s_page_lyric;
static lv_obj_t *s_lyric_line[MUSIC_UI_ROWS];

/* 登录页 */
static lv_obj_t *s_page_login;
static lv_obj_t *s_qr;
static lv_obj_t *s_lbl_qr_hint;
static lv_obj_t *s_lbl_logon;

/* 配网页（单应用固件自带，进出同样用 HIDDEN 切换） */
static lv_obj_t *s_page_netcfg;
static lv_obj_t *s_lbl_netcfg_state;
static lv_obj_t *s_lbl_netcfg_hint;

static lv_timer_t *s_timer;

/* 渲染侧缓存 */
static uint8_t s_rendered_page = 0xFF;
static uint8_t s_clock_sec = 0xFF;
static uint32_t s_art_gen;
static uint32_t s_qr_gen;
static uint8_t *s_cover_buf;
static lv_img_dsc_t s_cover_dsc;
static music_ui_model_t s_model __psram_bss__;

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
    const char *current;

    if (lbl == NULL || text == NULL) {
        return;
    }
    current = lv_label_get_text(lbl);
    if (current != NULL && strcmp(current, text) == 0) {
        return;
    }
    lv_label_set_text(lbl, text);
}

static void ui_set_color(lv_obj_t *obj, uint32_t color)
{
    if (obj != NULL) {
        lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
    }
}

static void ui_set_bg(lv_obj_t *obj, uint32_t color)
{
    if (obj != NULL) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    }
}

/* 单行裁剪标签：chinese_16 的 CJK 字模会画到 +20，必须显式给 20px 高 */
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

static lv_obj_t *ui_page_title(lv_obj_t *page, const char *text)
{
    lv_obj_t *title = ui_label(page, text, C_AMBER, &lv_font_chinese_16);

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 3);
    ui_box(page, 12, 28, 216, 1, 0, C_GRID_DIM);
    return title;
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

    s_lbl_net = ui_label(s_page_player, "离线", C_RED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_net, LV_ALIGN_TOP_LEFT, 12, 3);

    s_lbl_vol = ui_label(s_page_player, "60", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_vol, LV_ALIGN_TOP_LEFT, 58, 3);

    s_lbl_clock = ui_label(s_page_player, "--:--", C_TEXT, &lv_font_chinese_16);
    lv_obj_align(s_lbl_clock, LV_ALIGN_TOP_RIGHT, -12, 3);

    /* 封面：容器负责圆角裁剪，图片按 100x100 原尺寸贴上去
     * （RAW/JPEG 解码不支持 zoom，所以直接请求 100x100 的图） */
    s_cover_box = ui_box(s_page_player, COVER_X, COVER_Y, COVER_SZ, COVER_SZ, 10, C_PANEL);
    lv_obj_set_style_clip_corner(s_cover_box, true, LV_PART_MAIN);
    lv_obj_clear_flag(s_cover_box, LV_OBJ_FLAG_SCROLLABLE);

    s_cover_img = lv_img_create(s_cover_box);
    lv_obj_set_pos(s_cover_img, 0, 0);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);

    s_lbl_title = ui_label(s_page_player, "网易云音乐", C_TEXT, &lv_font_chinese_16);
    lv_obj_set_width(s_lbl_title, TEXT_W);
    lv_obj_set_height(s_lbl_title, 38);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_LEFT, TEXT_X, 28);

    s_lbl_artist = ui_clip_label(s_page_player, "", C_MUTED, &lv_font_chinese_16, TEXT_W);
    lv_obj_align(s_lbl_artist, LV_ALIGN_TOP_LEFT, TEXT_X, 70);

    s_lbl_album = ui_clip_label(s_page_player, "", C_MUTED_DIM, &lv_font_chinese_16, TEXT_W);
    lv_obj_align(s_lbl_album, LV_ALIGN_TOP_LEFT, TEXT_X, 90);

    s_bar_bg = ui_box(s_page_player, BAR_X, BAR_Y, BAR_W, BAR_H, 2, C_PANEL);
    s_bar_fill = ui_box(s_page_player, BAR_X, BAR_Y, 1, BAR_H, 2, C_AMBER);

    s_lbl_time = ui_label(s_page_player, "0:00 / 0:00", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_LEFT, BAR_X, 140);

    s_lbl_lyric = ui_clip_label(s_page_player, "", C_AMBER_SOFT, &lv_font_chinese_16, 220);
    lv_obj_align(s_lbl_lyric, LV_ALIGN_TOP_MID, 0, 164);

    s_lbl_detail = ui_clip_label(s_page_player, "", C_RED, &lv_font_chinese_16, 220);
    lv_obj_align(s_lbl_detail, LV_ALIGN_TOP_MID, 0, 184);

    s_lbl_hint = ui_label(s_page_player, "单击下一首 双击暂停 三击菜单", C_FOOT,
                          &lv_font_chinese_16);
    lv_obj_align(s_lbl_hint, LV_ALIGN_TOP_LEFT, 12, 206);
}

static void build_menu(lv_obj_t *root)
{
    s_page_menu = ui_page(root);
    s_lbl_menu_title = ui_page_title(s_page_menu, "网易云音乐");

    /* 底部提示兼「登录结果 / 错误」显示位：扫码成功后这里会短暂显示“登录成功” */
    s_lbl_menu_hint = ui_label(s_page_menu, "单击选择 双击确认 三击返回", C_MUTED_DIM,
                               &lv_font_chinese_16);
    lv_obj_align(s_lbl_menu_hint, LV_ALIGN_TOP_MID, 0, 206);

    /* 二级歌单超过五项时滚动，右侧标一下当前位置 */
    s_lbl_menu_pos = ui_label(s_page_menu, "", C_MUTED_DIM, &lv_font_chinese_16);
    lv_obj_align(s_lbl_menu_pos, LV_ALIGN_TOP_RIGHT, -12, 3);

    for (int i = 0; i < MUSIC_UI_ROWS; ++i) {
        const int y = ROW_Y0 + i * ROW_H;

        s_menu_row[i] = ui_box(s_page_menu, ROW_X, y, ROW_W, ROW_BOX_H, 8,
                               (i == 0) ? C_CARD_TOP : C_PANEL);
        s_menu_name[i] = ui_label(s_menu_row[i], "", C_TEXT, &lv_font_chinese_16);
        lv_obj_align(s_menu_name[i], LV_ALIGN_LEFT_MID, 12, 0);
        s_menu_value[i] = ui_label(s_menu_row[i], "", C_MUTED, &lv_font_chinese_16);
        lv_obj_align(s_menu_value[i], LV_ALIGN_RIGHT_MID, -12, 0);
    }
}

static void build_list(lv_obj_t *root)
{
    s_page_list = ui_page(root);
    ui_page_title(s_page_list, "歌曲");
    ui_page_hint(s_page_list, "单击选择 双击播放 三击返回");

    s_lbl_list_title = ui_label(s_page_list, "", C_MUTED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_list_title, LV_ALIGN_TOP_LEFT, 88, 3);

    s_lbl_list_pos = ui_label(s_page_list, "", C_MUTED_DIM, &lv_font_chinese_16);
    lv_obj_align(s_lbl_list_pos, LV_ALIGN_TOP_RIGHT, -12, 3);

    for (int i = 0; i < MUSIC_UI_ROWS; ++i) {
        const int y = ROW_Y0 + i * ROW_H;

        s_list_row[i] = ui_box(s_page_list, ROW_X, y, ROW_W, ROW_BOX_H, 8, C_PANEL);
        s_list_text[i] = ui_clip_label(s_list_row[i], "", C_TEXT, &lv_font_chinese_16, ROW_W - 16);
        lv_obj_align(s_list_text[i], LV_ALIGN_TOP_LEFT, 10, 0);
        s_list_meta[i] = ui_clip_label(s_list_row[i], "", C_MUTED_DIM, &lv_font_chinese_16,
                                       ROW_W - 16);
        lv_obj_align(s_list_meta[i], LV_ALIGN_TOP_LEFT, 10, 17);
    }
}

static void build_lyric(lv_obj_t *root)
{
    s_page_lyric = ui_page(root);
    ui_page_title(s_page_lyric, "歌词");
    ui_page_hint(s_page_lyric, "三击返回");

    for (int i = 0; i < MUSIC_UI_ROWS; ++i) {
        s_lyric_line[i] = ui_clip_label(s_page_lyric, "", C_MUTED_DIM, &lv_font_chinese_16, 224);
        lv_obj_set_style_text_align(s_lyric_line[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_lyric_line[i], LV_ALIGN_TOP_MID, 0, 40 + i * 30);
    }
}

static void build_login(lv_obj_t *root)
{
    s_page_login = ui_page(root);
    ui_page_title(s_page_login, "扫码登录");

    s_qr = lv_qrcode_create(s_page_login, 128, lv_color_hex(C_BG), lv_color_hex(C_TEXT));
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, 32);
    /* 还没申请到 unikey 时先空着，别让用户扫一张空白码 */
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);

    s_lbl_qr_hint = ui_label(s_page_login, "双击生成二维码", C_AMBER, &lv_font_chinese_16);
    lv_obj_align(s_lbl_qr_hint, LV_ALIGN_TOP_MID, 0, 166);

    s_lbl_logon = ui_label(s_page_login, "", C_MUTED_DIM, &lv_font_chinese_16);
    lv_obj_align(s_lbl_logon, LV_ALIGN_TOP_MID, 0, 186);

    ui_page_hint(s_page_login, "双击刷新 三击返回");
}

static void build_netcfg(lv_obj_t *root)
{
    s_page_netcfg = ui_page(root);
    ui_page_title(s_page_netcfg, "蓝牙配网");

    lv_obj_t *qr = lv_img_create(s_page_netcfg);
    lv_img_set_src(qr, &png_ble_netcfg_bind_qr_png);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 30);

    s_lbl_netcfg_state = ui_label(s_page_netcfg, "未连接", C_RED, &lv_font_chinese_16);
    lv_obj_align(s_lbl_netcfg_state, LV_ALIGN_TOP_MID, 0, 182);

    /* 底部提示：只有「已配过网、用户主动进来重配」时才显示；
     * 未配网时页面是锁死的，写「三击返回」会误导。 */
    s_lbl_netcfg_hint = ui_label(s_page_netcfg, "重新配网 · 三击返回", C_MUTED_DIM,
                                &lv_font_chinese_16);
    lv_obj_align(s_lbl_netcfg_hint, LV_ALIGN_TOP_MID, 0, 204);
    lv_obj_add_flag(s_lbl_netcfg_hint, LV_OBJ_FLAG_HIDDEN);
}

/* ==================== 刷新 ==================== */

static void apply_clock(void)
{
    struct tm calendar;
    char text[8];

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
    snprintf(text, sizeof(text), "%02d:%02d", calendar.tm_hour, calendar.tm_min);
    ui_set_text(s_lbl_clock, text);
}

static void format_mmss(uint32_t ms, char *out, size_t capacity)
{
    const uint32_t total = ms / 1000u;

    snprintf(out, capacity, "%u:%02u", (unsigned)(total / 60u), (unsigned)(total % 60u));
}

static void refresh_cover(const music_ui_model_t *m)
{
    size_t length = 0;

    if (m->art_gen == s_art_gen) {
        return;
    }
    s_art_gen = m->art_gen;

    if (m->art_len == 0) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_cover_buf == NULL) {
        s_cover_buf = lv_mem_alloc(MUSIC_COVER_BUF);
        if (s_cover_buf == NULL) {
            LOGW("no memory for cover buffer");
            lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }
    if (!music_app_cover_copy(s_cover_buf, MUSIC_COVER_BUF, &length)) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    s_cover_dsc.header.always_zero = 0;
    s_cover_dsc.header.cf = LV_IMG_CF_RAW;
    s_cover_dsc.header.w = (m->art_w > 0) ? (lv_coord_t)m->art_w : COVER_SZ;
    s_cover_dsc.header.h = (m->art_h > 0) ? (lv_coord_t)m->art_h : COVER_SZ;
    s_cover_dsc.data = s_cover_buf;
    s_cover_dsc.data_size = (uint32_t)length;

    /* 同一个 lv_img_dsc_t 地址换内容：先让 LVGL 丢掉上一张的解码缓存 */
    lv_img_cache_invalidate_src(&s_cover_dsc);
    lv_img_set_src(s_cover_img, &s_cover_dsc);
    /* 服务端一般按 ?param=100y100 返回，真不听话时也会被封面框裁掉 */
    lv_obj_set_size(s_cover_img, s_cover_dsc.header.w, s_cover_dsc.header.h);
    lv_obj_clear_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_player(const music_ui_model_t *m)
{
    char text[64];
    char pos[16];
    char dur[16];

    ui_set_text(s_lbl_net, m->net_ok ? "在线" : "离线");
    ui_set_color(s_lbl_net, m->net_ok ? C_GREEN : C_RED);

    snprintf(text, sizeof(text), "%u", (unsigned)m->volume);
    ui_set_text(s_lbl_vol, text);

    ui_set_text(s_lbl_title, m->title);
    ui_set_text(s_lbl_artist, m->artist);

    if (m->detail[0] != '\0') {
        ui_set_text(s_lbl_album, "");
        ui_set_text(s_lbl_detail, m->detail);
    } else {
        ui_set_text(s_lbl_album, m->album);
        ui_set_text(s_lbl_detail, m->busy ? "正在联网加载 …" : "");
    }
    ui_set_color(s_lbl_detail, m->busy ? C_AMBER : C_RED);

    /* 进度：暂停时整条变灰（和大数字同色系的约定） */
    const uint32_t dur_ms = (m->dur_ms > 0) ? m->dur_ms : 1u;
    uint32_t ratio = (m->pos_ms >= dur_ms) ? 1000u : (m->pos_ms * 1000u / dur_ms);
    int width = (int)((uint32_t)BAR_W * ratio / 1000u);

    if (width < 2) {
        width = 2;
    }
    lv_obj_set_size(s_bar_fill, width, BAR_H);
    ui_set_bg(s_bar_fill, m->playing ? C_AMBER : C_MUTED_DIM);

    format_mmss(m->pos_ms, pos, sizeof(pos));
    format_mmss(m->dur_ms, dur, sizeof(dur));
    snprintf(text, sizeof(text), "%s / %s", pos, dur);
    ui_set_text(s_lbl_time, text);

    if (m->lyric_valid && m->lyric_center >= 0 && m->lyric_center < MUSIC_UI_ROWS &&
        m->lyric[m->lyric_center][0] != '\0') {
        ui_set_text(s_lbl_lyric, m->lyric[m->lyric_center]);
        ui_set_color(s_lbl_lyric, m->playing ? C_AMBER : C_AMBER_SOFT);
    } else if (m->song_index == 0) {
        ui_set_text(s_lbl_lyric, m->net_ok ? "三击菜单选择歌单" : "未连接网络");
        ui_set_color(s_lbl_lyric, C_MUTED_DIM);
    } else {
        ui_set_text(s_lbl_lyric, "");
    }

    refresh_cover(m);
}

static void refresh_menu(const music_ui_model_t *m)
{
    char text[16];

    ui_set_text(s_lbl_menu_title, m->menu_title);

    if (m->menu_count > MUSIC_UI_ROWS) {
        snprintf(text, sizeof(text), "%u/%u", (unsigned)m->menu_index + 1u,
                 (unsigned)m->menu_count);
        ui_set_text(s_lbl_menu_pos, text);
    } else {
        ui_set_text(s_lbl_menu_pos, "");
    }

    for (int i = 0; i < MUSIC_UI_ROWS; ++i) {
        const uint8_t index = (uint8_t)(m->menu_window + i);

        if (index >= m->menu_count) {
            lv_obj_add_flag(s_menu_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_menu_row[i], LV_OBJ_FLAG_HIDDEN);

        const bool selected = (index == m->menu_index);

        ui_set_bg(s_menu_row[i], selected ? C_CARD_TOP : C_PANEL);
        ui_set_text(s_menu_name[i], m->menu_name[i]);
        ui_set_text(s_menu_value[i], m->menu_value[i]);
        ui_set_color(s_menu_name[i], selected ? C_AMBER : C_TEXT);
        ui_set_color(s_menu_value[i], selected ? C_AMBER : C_MUTED_DIM);
    }

    ui_set_text(s_lbl_menu_hint, (m->detail[0] != '\0') ? m->detail : m->menu_hint);
    ui_set_color(s_lbl_menu_hint, (m->detail[0] != '\0') ? C_RED : C_MUTED_DIM);
}

static void refresh_netcfg(const music_ui_model_t *m)
{
    ui_set_text(s_lbl_netcfg_state, m->net_ok ? "已连接" : "未连接");
    ui_set_color(s_lbl_netcfg_state, m->net_ok ? C_GREEN : C_RED);

    /* 已配过网（用户主动进来重配）才提示可以退出 */
    if (sys_wifi_has_ap()) {
        lv_obj_clear_flag(s_lbl_netcfg_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lbl_netcfg_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_list(const music_ui_model_t *m)
{
    char text[16];

    ui_set_text(s_lbl_list_title, m->list_title);
    snprintf(text, sizeof(text), "%u/%u", (unsigned)m->list_index + 1u,
             (unsigned)m->song_count);
    ui_set_text(s_lbl_list_pos, text);

    for (int i = 0; i < MUSIC_UI_ROWS; ++i) {
        const uint8_t index = (uint8_t)(m->list_window + i);

        if (i >= m->row_count) {
            lv_obj_add_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);
        ui_set_text(s_list_text[i], m->row_title[i]);
        ui_set_text(s_list_meta[i], m->row_sub[i]);

        const bool selected = (index == m->list_index);
        const bool current = (index + 1u == m->song_index);
        ui_set_bg(s_list_row[i], selected ? C_CARD_TOP : C_PANEL);
        ui_set_color(s_list_text[i], selected ? C_AMBER : (current ? C_GREEN : C_TEXT));
        ui_set_color(s_list_meta[i], selected ? C_AMBER_SOFT : C_MUTED_DIM);
    }
}

static void refresh_lyric(const music_ui_model_t *m)
{
    for (int i = 0; i < MUSIC_UI_ROWS; ++i) {
        const bool center = (m->lyric_valid && i == m->lyric_center);

        ui_set_text(s_lyric_line[i], m->lyric[i]);
        if (center) {
            ui_set_color(s_lyric_line[i], C_AMBER);
        } else if (m->lyric_valid && m->lyric_center >= 0 &&
                   (i == m->lyric_center - 1 || i == m->lyric_center + 1)) {
            ui_set_color(s_lyric_line[i], C_MUTED);
        } else {
            ui_set_color(s_lyric_line[i], C_MUTED_DIM);
        }
    }
}

static void refresh_login(const music_ui_model_t *m)
{
    if (m->qr_gen != s_qr_gen) {
        s_qr_gen = m->qr_gen;
        if (m->qr_url[0] != '\0') {
            lv_qrcode_update(s_qr, m->qr_url, (uint32_t)strlen(m->qr_url));
            lv_obj_clear_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        }
    }

    ui_set_text(s_lbl_qr_hint, m->detail[0] != '\0' ? m->detail : m->qr_hint);
    ui_set_color(s_lbl_qr_hint, (m->detail[0] != '\0') ? C_RED :
                                  ((m->qr_state == 803) ? C_GREEN : C_AMBER));

    ui_set_text(s_lbl_logon, m->logon);
    ui_set_color(s_lbl_logon, m->logged_in ? C_GREEN : C_MUTED_DIM);
}

static void refresh(void)
{
    if (s_scr == NULL) {
        return;
    }
    if (!music_app_get_model(&s_model)) {
        return;
    }
    const music_ui_model_t *m = &s_model;

    if (s_rendered_page != m->page) {
        s_rendered_page = m->page;
        lv_obj_add_flag(s_page_player, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_lyric, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_login, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_netcfg, LV_OBJ_FLAG_HIDDEN);
        switch ((music_page_t)m->page) {
        case MUSIC_PAGE_MENU:
            lv_obj_clear_flag(s_page_menu, LV_OBJ_FLAG_HIDDEN);
            break;
        case MUSIC_PAGE_SONGLIST:
            lv_obj_clear_flag(s_page_list, LV_OBJ_FLAG_HIDDEN);
            break;
        case MUSIC_PAGE_LYRIC:
            lv_obj_clear_flag(s_page_lyric, LV_OBJ_FLAG_HIDDEN);
            break;
        case MUSIC_PAGE_LOGIN:
            lv_obj_clear_flag(s_page_login, LV_OBJ_FLAG_HIDDEN);
            break;
        case MUSIC_PAGE_NETCFG:
            lv_obj_clear_flag(s_page_netcfg, LV_OBJ_FLAG_HIDDEN);
            break;
        case MUSIC_PAGE_PLAYER:
        default:
            lv_obj_clear_flag(s_page_player, LV_OBJ_FLAG_HIDDEN);
            break;
        }
    }

    apply_clock();

    switch ((music_page_t)m->page) {
    case MUSIC_PAGE_MENU:
        refresh_menu(m);
        break;
    case MUSIC_PAGE_SONGLIST:
        refresh_list(m);
        break;
    case MUSIC_PAGE_LYRIC:
        refresh_lyric(m);
        break;
    case MUSIC_PAGE_LOGIN:
        refresh_login(m);
        break;
    case MUSIC_PAGE_NETCFG:
        refresh_netcfg(m);
        break;
    case MUSIC_PAGE_PLAYER:
    default:
        refresh_player(m);
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
    /* 与内置页/电台页一致：做成 lv_scr_act() 的子对象，不新建 LVGL screen */
    s_scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_pos(s_scr, 0, 0);
    lv_obj_set_size(s_scr, 240, 240);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_scr, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);

    build_player(s_scr);
    build_menu(s_scr);
    build_list(s_scr);
    build_lyric(s_scr);
    build_login(s_scr);
    build_netcfg(s_scr);

    s_rendered_page = 0xFF;
    s_clock_sec = 0xFF;
    s_art_gen = 0;
    s_qr_gen = 0;

    s_timer = lv_timer_create(timer_cb, MUSIC_UI_TICK_MS, NULL);
    refresh();
    LOGI("music screen built");
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
    /* 先删控件再释放 JPEG 缓冲：图片对象只引用指针，不会自己 free */
    if (s_cover_buf != NULL) {
        lv_mem_free(s_cover_buf);
        s_cover_buf = NULL;
    }

    s_page_player = NULL;
    s_page_menu = NULL;
    s_lbl_menu_title = NULL;
    s_lbl_menu_pos = NULL;
    s_lbl_menu_hint = NULL;
    s_page_list = NULL;
    s_page_lyric = NULL;
    s_page_login = NULL;
    s_page_netcfg = NULL;
    s_lbl_netcfg_state = NULL;
    s_lbl_netcfg_hint = NULL;
    s_lbl_net = NULL;
    s_lbl_vol = NULL;
    s_lbl_clock = NULL;
    s_cover_box = NULL;
    s_cover_img = NULL;
    s_lbl_title = NULL;
    s_lbl_artist = NULL;
    s_lbl_album = NULL;
    s_bar_bg = NULL;
    s_bar_fill = NULL;
    s_lbl_time = NULL;
    s_lbl_lyric = NULL;
    s_lbl_detail = NULL;
    s_lbl_hint = NULL;
    s_lbl_list_title = NULL;
    s_lbl_list_pos = NULL;
    s_qr = NULL;
    s_lbl_qr_hint = NULL;
    s_lbl_logon = NULL;
    for (int i = 0; i < MUSIC_UI_ROWS; ++i) {
        s_menu_row[i] = NULL;
        s_menu_name[i] = NULL;
        s_menu_value[i] = NULL;
        s_list_row[i] = NULL;
        s_list_text[i] = NULL;
        s_list_meta[i] = NULL;
        s_lyric_line[i] = NULL;
    }
    s_rendered_page = 0xFF;
}

/* ==================== 导航 ==================== */

static int music_nav_open(const struct lisa_ui_nav_scr *scr, void **data)
{
    (void)scr;
    if (data != NULL) {
        *data = NULL;
    }
    build();
    return 0;
}

static int music_nav_show(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    if (s_scr != NULL) {
        lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_scr);
    }
    return 0;
}

static int music_nav_resume(const struct lisa_ui_nav_scr *scr, void *data)
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

static int music_nav_pause(const struct lisa_ui_nav_scr *scr, void *data)
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

static int music_nav_close(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    destroy();
    return 0;
}

static const struct lisa_ui_nav_scr s_music_nav_scr = {
    .unique_id = MUSIC_UI_SCR_ID,
    .open = music_nav_open,
    .show = music_nav_show,
    .pause = music_nav_pause,
    .resume = music_nav_resume,
    .close = music_nav_close,
};

int music_ui_init(void)
{
    const int ret = lisa_ui_nav_scr_add(&s_music_nav_scr);

    if (ret != 0) {
        LOGE("register nav screen failed: %d", ret);
        return ret;
    }
#ifdef CONFIG_NETEASE_MUSIC_ONLY
    /* 单应用固件：音乐页是整个 UI，注册成导航栈默认页（等价于 open + push + show）。
     * 全功能固件里默认页是 radio 页，音乐页靠 nav_to 抢前台。 */
    if (lisa_ui_nav_scr_default_set(&s_music_nav_scr) != 0) {
        LOGE("music_ui_set_default failed");
        return -1;
    }
#endif
    return 0;
}

int music_ui_screen_id(void)
{
    return MUSIC_UI_SCR_ID;
}

int music_ui_top_id(void)
{
    return lisa_ui_nav_scr_get_top_id();
}

void music_ui_show(void)
{
    if (music_ui_top_id() == MUSIC_UI_SCR_ID) {
        return;
    }
    if (lisa_ui_nav_scr_nav_to(MUSIC_UI_SCR_ID) != 0) {
        LOGE("nav to music screen failed");
    }
}

void music_ui_hide(void)
{
    (void)lisa_ui_nav_scr_nav_default();
}
