#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "lv_port_disp.h"
#include "lisa_display.h"
#include "lisa_device.h"
#include "lisa_log.h"

#include "arcade_core.h"
#include "arcade_rom.h"
#include "arcade_ui.h"
#include "arcade_video.h"

#define TAG "arcade-ui"

#define SCREEN_W 240
#define SCREEN_H 240

#define LIST_ROWS        5
#define LIST_ROW_H       36
#define LIST_TOP         34
#define LIST_LEFT        8
#define LIST_TEXT_W      (SCREEN_W - 2 * LIST_LEFT - 14)

#define C_BG      lv_color_hex(0x000000)
#define C_TEXT    lv_color_hex(0xE8E8E8)
#define C_DIM     lv_color_hex(0x707070)
#define C_ACCENT  lv_color_hex(0xFFB020)

#define CJK_FONT (&lv_font_simsun_16_cjk)

static lv_obj_t *s_list_box;
static lv_obj_t *s_player_box;
static lv_obj_t *s_list_rows[LIST_ROWS];
static lv_obj_t *s_list_hint;
static lv_obj_t *s_list_title;
static lv_obj_t *s_canvas;
static lv_obj_t *s_center_label;

static int s_scroll;      /* 列表可见窗口的第一个元素下标 */
static bool s_player_mode;

static void screen_style(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, lv_color_t color, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_obj_set_style_text_font(label, CJK_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, "");

    return label;
}

static void build_list_page(lv_obj_t *root)
{
    s_list_box = lv_obj_create(root);
    lv_obj_set_pos(s_list_box, 0, 0);
    lv_obj_set_size(s_list_box, SCREEN_W, SCREEN_H);
    screen_style(s_list_box);

    s_list_title = make_label(s_list_box, C_DIM, LIST_LEFT, 6, SCREEN_W - 2 * LIST_LEFT, 22);
    lv_label_set_text(s_list_title, "模拟器");

    for (int i = 0; i < LIST_ROWS; i++) {
        s_list_rows[i] = make_label(s_list_box, C_TEXT, LIST_LEFT, LIST_TOP + i * LIST_ROW_H,
                                    LIST_TEXT_W, 22);
    }

    s_list_hint = make_label(s_list_box, C_DIM, LIST_LEFT, SCREEN_H - 24, SCREEN_W - 2 * LIST_LEFT, 20);
    lv_label_set_text(s_list_hint, "单击下移 双击进入");
}

static void build_player_page(lv_obj_t *root)
{
    lv_color_t *fb = arcade_video_get_fb();

    s_player_box = lv_obj_create(root);
    lv_obj_set_pos(s_player_box, 0, 0);
    lv_obj_set_size(s_player_box, SCREEN_W, SCREEN_H);
    screen_style(s_player_box);
    lv_obj_add_flag(s_player_box, LV_OBJ_FLAG_HIDDEN);

    s_canvas = lv_canvas_create(s_player_box);
    lv_canvas_set_buffer(s_canvas, fb, ARCADE_FB_WIDTH, ARCADE_FB_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_canvas, 0, (SCREEN_H - ARCADE_FB_HEIGHT) / 2);

    /* 覆盖在画面之上的提示（加载失败等），平时隐藏 */
    s_center_label = make_label(s_player_box, C_ACCENT, 16, SCREEN_H / 2 - 12,
                                SCREEN_W - 32, 24);
    lv_obj_set_style_text_align(s_center_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
}

int arcade_ui_init(void)
{
    lisa_device_t *display;

    LISA_LOGI(TAG, "lv_init");
    lv_init();

    display = lisa_device_get("display");
    if (display == NULL) {
        LISA_LOGE(TAG, "display device not found");
        return -1;
    }

    /* 必须先 lv_port_disp_init() 再 blanking_on()：面板是在 port init 里才
     * attach 上的，提前调用 blanking_on() 会解引用空的 priv->panel 直接 Load
     * access fault（与 apps-ui/portable/arcs/lisa_ui_lvgl.c 里顺序一致）。 */
    LISA_LOGI(TAG, "lv_port_disp_init");
    lv_port_disp_init(display);
    LISA_LOGI(TAG, "lv_port_disp_init done");

    lisa_display_blanking_on(display);

    lv_obj_t *root = lv_obj_create(NULL);
    screen_style(root);
    lv_scr_load(root);

    build_list_page(root);
    build_player_page(root);

    lv_task_handler();
    lisa_display_blanking_off(display);

    LISA_LOGI(TAG, "LVGL ready");

    return 0;
}

void arcade_ui_show_list(void)
{
    s_player_mode = false;
    lv_obj_clear_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_player_box, LV_OBJ_FLAG_HIDDEN);
}

void arcade_ui_show_player(void)
{
    s_player_mode = true;
    lv_obj_add_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_player_box, LV_OBJ_FLAG_HIDDEN);
}

void arcade_ui_list_set_hint(const char *text)
{
    if (s_list_hint == NULL || text == NULL) {
        return;
    }

    lv_label_set_text(s_list_hint, text);
}

void arcade_ui_list_refresh(int selected)
{
    int count = arcade_rom_count();
    char buf[80];

    if (s_list_box == NULL) {
        return;
    }

    if (count == 0) {
        if (!arcade_rom_scanned()) {
            lv_label_set_text(s_list_title, "模拟器");
            lv_label_set_text(s_list_hint, "正在读取 TF 卡…");
        } else {
            lv_label_set_text(s_list_title, "模拟器");
            lv_label_set_text(s_list_hint, "TF 卡里没有 GB/GBC ROM");
        }
        for (int i = 0; i < LIST_ROWS; i++) {
            lv_label_set_text(s_list_rows[i], "");
        }
        return;
    }

    if (selected < 0) {
        selected = 0;
    }
    if (selected >= count) {
        selected = count - 1;
    }

    if (selected < s_scroll) {
        s_scroll = selected;
    } else if (selected >= s_scroll + LIST_ROWS) {
        s_scroll = selected - LIST_ROWS + 1;
    }

    snprintf(buf, sizeof(buf), "模拟器  %d/%d", selected + 1, count);
    lv_label_set_text(s_list_title, buf);
    lv_label_set_text(s_list_hint, "单击下移 双击进入 三击上移");

    for (int i = 0; i < LIST_ROWS; i++) {
        int index = s_scroll + i;

        if (index >= count) {
            lv_label_set_text(s_list_rows[i], "");
            continue;
        }

        const char *name = arcade_rom_name(index);
        bool picked = (index == selected);

        snprintf(buf, sizeof(buf), "%s%s", picked ? "> " : "  ", name ? name : "?");
        lv_label_set_text(s_list_rows[i], buf);
        lv_obj_set_style_text_color(s_list_rows[i], picked ? C_ACCENT : C_TEXT, LV_PART_MAIN);
    }
}

void arcade_ui_player_invalidate(void)
{
    if (s_canvas == NULL) {
        return;
    }

    lv_obj_invalidate(s_canvas);
}

uint32_t arcade_ui_handler(void)
{
    uint32_t idle = lv_timer_handler();

    (void)s_player_mode;

    if (idle == LV_NO_TIMER_READY || idle > 20) {
        idle = 20;
    }

    return idle;
}

void arcade_ui_player_debug(char *buf, uint32_t len)
{
    int n;

    if (buf == NULL || len == 0) {
        return;
    }

    if (s_canvas == NULL || s_player_box == NULL) {
        snprintf(buf, len, "canvas=(null)");
        return;
    }

    n = snprintf(buf, len,
                 "scr=%p box=%p box_hidden=%d canvas=%p pos=%d,%d size=%dx%d hidden=%d",
                 (void *)lv_scr_act(), (void *)s_player_box,
                 lv_obj_has_flag(s_player_box, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
                 (void *)s_canvas,
                 (int)lv_obj_get_x(s_canvas), (int)lv_obj_get_y(s_canvas),
                 (int)lv_obj_get_width(s_canvas), (int)lv_obj_get_height(s_canvas),
                 lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN) ? 1 : 0);

    if (n < 0 || (uint32_t)n >= len) {
        return;
    }

    snprintf(buf + n, len - (uint32_t)n, " fb=%p", (void *)arcade_video_get_fb());
}

void arcade_ui_player_message(const char *text)
{
    if (s_center_label == NULL) {
        return;
    }

    if (text == NULL || text[0] == '\0') {
        lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s_center_label, text);
    lv_obj_clear_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
}
