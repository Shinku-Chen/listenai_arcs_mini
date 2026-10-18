/*
 * 对讲机界面实现（LVGL，240x240）
 *
 * 布局（240x240）：
 *   y=0..22    顶栏：左侧「对讲」，中间链路/网关状态，右侧电量
 *   y=24..176  对话区：可滚动，气泡最多保留 IC_UI_MAX_BUBBLES 条
 *   y=178..202 轮次指示：空闲「按住说话」/ 红色「准备中…」/ 绿色「可以说话了」
 *   y=204..236 提示行 + 网关原因
 *
 * 参考 ai-passport oc_ui.c 的视觉语义（用户右、助手左、轮次红绿），几何按 240x240 重排。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_log.h"
#include "lisa_ui.h"
#include "lisa_ui_nav_scr.h"
#include "shell.h"

#include <lvgl.h>
#include "lisa_ui_fonts.h" /* lv_font_chinese_16 */

#include "ic_proto.h"
#include "ic_ui.h"

#define TAG "ic.ui"

#define IC_UI_SCR_ID 91

/* 配色 */
#define C_BG 0x071017
#define C_PANEL 0x0D1B23
#define C_TEXT 0xE6F0F2
#define C_MUTED 0x849BA0
#define C_AMBER 0xFFB74D
#define C_GREEN 0x4ED39A
#define C_RED 0xFF5D62
#define C_USER_BG 0x2A3A22
#define C_AI_BG 0x14262F

/* 几何 */
#define TB_H 22
#define CONV_X 6
#define CONV_Y 24
#define CONV_W 228
#define CONV_H 152

#define IC_UI_MAX_BUBBLES 8
/* 单条气泡的字符上限：屏幕上放不下的部分截断（参考实现也做截断，只是它按 KB 算） */
#define IC_UI_BUBBLE_MAX_BYTES 640

static lv_obj_t *s_scr;
static lv_obj_t *s_conv;
static lv_obj_t *s_lbl_link;
static lv_obj_t *s_lbl_batt;
static lv_obj_t *s_lbl_turn;
static lv_obj_t *s_dot_turn;
static lv_obj_t *s_lbl_hint;

static lv_obj_t *s_bubbles[IC_UI_MAX_BUBBLES];
static uint8_t s_bubble_count;

static bool s_connected;
static bool s_ready;
static char s_gw_state[16] = "unknown";
static char s_gw_detail[64];
static uint8_t s_turn;
static uint8_t s_battery;

/* ---------------- 异步任务：从任意任务投递到 LVGL 线程 ---------------- */

enum { JOB_TEXT = 1, JOB_CLEAR, JOB_LINK, JOB_GATEWAY, JOB_TURN, JOB_BATT };

typedef struct {
    uint8_t kind;
    uint8_t role;
    bool flag;
    uint8_t value;
    char text[IC_UI_BUBBLE_MAX_BYTES + 4];
    char aux[64];
} ui_job_t;
static void ui_apply_text(uint8_t role, const char *text);
static void ui_apply_status(void);

static ui_job_t *job_new(uint8_t kind)
{
    ui_job_t *job = (ui_job_t *)pvPortMalloc(sizeof(ui_job_t));
    if (job != NULL) {
        memset(job, 0, sizeof(*job));
        job->kind = kind;
    }
    return job;
}

static void ui_job_run(void *arg)
{
    ui_job_t *job = (ui_job_t *)arg;
    if (job == NULL) {
        return;
    }

    switch (job->kind) {
    case JOB_TEXT:
        if (s_scr != NULL) {
            ui_apply_text(job->role, job->text);
        }
        break;
    case JOB_CLEAR:
        if (s_scr != NULL) {
            for (uint8_t i = 0; i < s_bubble_count; i++) {
                if (s_bubbles[i] != NULL) {
                    lv_obj_del(s_bubbles[i]);
                    s_bubbles[i] = NULL;
                }
            }
            s_bubble_count = 0;
        }
        break;
    case JOB_LINK:
        s_connected = job->flag;
        s_ready = (job->value != 0);
        if (s_scr != NULL) {
            ui_apply_status();
        }
        break;
    case JOB_GATEWAY:
        snprintf(s_gw_state, sizeof(s_gw_state), "%s", job->text);
        snprintf(s_gw_detail, sizeof(s_gw_detail), "%s", job->aux);
        if (s_scr != NULL) {
            ui_apply_status();
        }
        break;
    case JOB_TURN:
        s_turn = job->value;
        if (s_scr != NULL) {
            ui_apply_status();
        }
        break;
    case JOB_BATT:
        s_battery = job->value;
        if (s_scr != NULL) {
            ui_apply_status();
        }
        break;
    default:
        break;
    }

    vPortFree(job);
}

static void ui_post(ui_job_t *job)
{
    if (job == NULL) {
        return;
    }
    lv_async_call(ui_job_run, job);
}

/* ---------------- 界面构建 ---------------- */

static lv_obj_t *make_label(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    return lbl;
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void ui_apply_status(void)
{
    if (s_lbl_link == NULL) {
        return;
    }

    if (!s_connected) {
        lv_label_set_text(s_lbl_link, "未连接");
        lv_obj_set_style_text_color(s_lbl_link, lv_color_hex(C_MUTED), LV_PART_MAIN);
    } else if (!s_ready) {
        char buf[40];
        snprintf(buf, sizeof(buf), "已连接·网关%s", s_gw_state);
        lv_label_set_text(s_lbl_link, buf);
        lv_obj_set_style_text_color(s_lbl_link, lv_color_hex(C_AMBER), LV_PART_MAIN);
    } else {
        lv_label_set_text(s_lbl_link, "就绪");
        lv_obj_set_style_text_color(s_lbl_link, lv_color_hex(C_GREEN), LV_PART_MAIN);
    }

    if (s_lbl_batt != NULL) {
        char buf[12];
        if (s_battery > 0) {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)s_battery);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        lv_label_set_text(s_lbl_batt, buf);
    }

    if (s_lbl_turn != NULL && s_dot_turn != NULL) {
        switch (s_turn) {
        case IC_TURN_PREPARE:
            lv_label_set_text(s_lbl_turn, "已按下，准备中…");
            lv_obj_set_style_text_color(s_lbl_turn, lv_color_hex(C_RED), LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_dot_turn, lv_color_hex(C_RED), LV_PART_MAIN);
            break;
        case IC_TURN_READY:
            lv_label_set_text(s_lbl_turn, "可以说话了");
            lv_obj_set_style_text_color(s_lbl_turn, lv_color_hex(C_GREEN), LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_dot_turn, lv_color_hex(C_GREEN), LV_PART_MAIN);
            break;
        default:
            lv_label_set_text(s_lbl_turn, "按住说话");
            lv_obj_set_style_text_color(s_lbl_turn, lv_color_hex(C_MUTED), LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_dot_turn, lv_color_hex(C_MUTED), LV_PART_MAIN);
            break;
        }
    }

    if (s_lbl_hint != NULL) {
        if (s_connected && !s_ready && s_gw_detail[0] != '\0') {
            lv_label_set_text(s_lbl_hint, s_gw_detail);
        } else {
            lv_label_set_text(s_lbl_hint, "长按说话  双击退出对讲");
        }
    }
}

static void ui_apply_text(uint8_t role, const char *text)
{
    if (s_conv == NULL || text == NULL) {
        return;
    }

    /* 超出上限的旧气泡先删（滚动容器里保留最近 N 条） */
    if (s_bubble_count >= IC_UI_MAX_BUBBLES) {
        if (s_bubbles[0] != NULL) {
            lv_obj_del(s_bubbles[0]);
        }
        for (uint8_t i = 1; i < IC_UI_MAX_BUBBLES; i++) {
            s_bubbles[i - 1] = s_bubbles[i];
        }
        s_bubbles[IC_UI_MAX_BUBBLES - 1] = NULL;
        s_bubble_count = IC_UI_MAX_BUBBLES - 1;
    }

    /* 一行 = 满宽 flex row，气泡在其中靠左或靠右 */
    lv_obj_t *row = lv_obj_create(s_conv);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, CONV_W - 12);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          (role == IC_ROLE_USER) ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, CONV_W - 36, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bubble, 6, LV_PART_MAIN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t bg = C_AI_BG;
    uint32_t fg = C_TEXT;
    if (role == IC_ROLE_USER) {
        bg = C_USER_BG;
        fg = C_AMBER;
    } else if (role == IC_ROLE_SYSTEM) {
        bg = C_PANEL;
        fg = C_MUTED;
    }
    lv_obj_set_style_bg_color(bubble, lv_color_hex(bg), LV_PART_MAIN);

    lv_obj_t *lbl = make_label(bubble, fg);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lbl, CONV_W - 48, LV_PART_MAIN);
    lv_label_set_text(lbl, text);

    s_bubbles[s_bubble_count++] = row;
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    lv_obj_scroll_to_view(bubble, LV_ANIM_OFF);
}

static void ic_build(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶栏 */
    lv_obj_t *title = make_label(s_scr, C_AMBER);
    lv_label_set_text(title, "对讲");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 2);

    s_lbl_link = make_label(s_scr, C_MUTED);
    lv_label_set_text(s_lbl_link, "未连接");
    lv_obj_align(s_lbl_link, LV_ALIGN_TOP_MID, 6, 2);

    s_lbl_batt = make_label(s_scr, C_TEXT);
    lv_label_set_text(s_lbl_batt, "--");
    lv_obj_align(s_lbl_batt, LV_ALIGN_TOP_RIGHT, -10, 2);

    /* 对话区 */
    s_conv = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_conv);
    lv_obj_set_pos(s_conv, CONV_X, CONV_Y);
    lv_obj_set_size(s_conv, CONV_W, CONV_H);
    lv_obj_set_style_bg_color(s_conv, lv_color_hex(C_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_conv, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(s_conv, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_conv, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_conv, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_conv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_conv, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_conv, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_conv, LV_SCROLLBAR_MODE_AUTO);

    /* 轮次指示 */
    s_dot_turn = make_box(s_scr, 12, 188, 8, 8, 4, C_MUTED);
    s_lbl_turn = make_label(s_scr, C_MUTED);
    lv_label_set_text(s_lbl_turn, "按住说话");
    lv_obj_align(s_lbl_turn, LV_ALIGN_TOP_LEFT, 26, 183);

    s_lbl_hint = make_label(s_scr, C_MUTED);
    lv_label_set_text(s_lbl_hint, "长按说话  双击退出对讲");
    lv_obj_set_style_text_font(s_lbl_hint, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_align(s_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    s_bubble_count = 0;
    for (uint8_t i = 0; i < IC_UI_MAX_BUBBLES; i++) {
        s_bubbles[i] = NULL;
    }

    ui_apply_status();
    LISA_LOGI(TAG, "intercom screen built");
}

static int ic_open(const struct lisa_ui_nav_scr *scr, void **data)
{
    (void)scr;
    (void)data;
    ic_build();
    return 0;
}

static int ic_show_cb(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    if (s_scr != NULL) {
        lv_scr_load(s_scr);
    }
    return 0;
}

static int ic_close(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    if (s_scr != NULL) {
        lv_obj_del(s_scr);
        s_scr = NULL;
        s_conv = NULL;
        s_lbl_link = NULL;
        s_lbl_batt = NULL;
        s_lbl_turn = NULL;
        s_dot_turn = NULL;
        s_lbl_hint = NULL;
        s_bubble_count = 0;
        for (uint8_t i = 0; i < IC_UI_MAX_BUBBLES; i++) {
            s_bubbles[i] = NULL;
        }
    }
    return 0;
}

static const struct lisa_ui_nav_scr s_ic_scr = {
    .unique_id = IC_UI_SCR_ID,
    .open = ic_open,
    .show = ic_show_cb,
    .close = ic_close,
};

int ic_ui_init(void)
{
    const int ret = lisa_ui_nav_scr_add(&s_ic_scr);
    LISA_LOGI(TAG, "ic_ui_init ret=%d", ret);
    return ret;
}

/* ---------------- 对外接口 ---------------- */

void ic_ui_show(void)
{
    lisa_ui_nav_scr_nav_to(IC_UI_SCR_ID);
}

void ic_ui_hide(void)
{
    lisa_ui_nav_scr_nav_default();
}

void ic_ui_add_text(uint8_t role, const char *utf8, uint16_t len)
{
    if (utf8 == NULL || len == 0) {
        return;
    }

    ui_job_t *job = job_new(JOB_TEXT);
    if (job == NULL) {
        LISA_LOGW(TAG, "no memory for text job");
        return;
    }
    job->role = role;
    uint16_t n = len;
    if (n > IC_UI_BUBBLE_MAX_BYTES) {
        n = IC_UI_BUBBLE_MAX_BYTES;
    }
    memcpy(job->text, utf8, n);
    /* 截断时补省略号（用中文六点省略号，和参考实现一致） */
    if (n < len) {
        static const char k_ellipsis[] = "……";
        if (n + sizeof(k_ellipsis) - 1 < sizeof(job->text)) {
            memcpy(job->text + n, k_ellipsis, sizeof(k_ellipsis) - 1);
            n = (uint16_t)(n + sizeof(k_ellipsis) - 1);
        }
    }
    job->text[n] = '\0';
    ui_post(job);
}

void ic_ui_clear(void)
{
    ui_post(job_new(JOB_CLEAR));
}

void ic_ui_set_link(bool connected, bool ready)
{
    ui_job_t *job = job_new(JOB_LINK);
    if (job == NULL) {
        return;
    }
    job->flag = connected;
    job->value = ready ? 1 : 0;
    ui_post(job);
}

void ic_ui_set_gateway(const char *state, const char *detail)
{
    ui_job_t *job = job_new(JOB_GATEWAY);
    if (job == NULL) {
        return;
    }
    snprintf(job->text, sizeof(job->text), "%s", (state != NULL) ? state : "unknown");
    snprintf(job->aux, sizeof(job->aux), "%s", (detail != NULL) ? detail : "");
    ui_post(job);
}

void ic_ui_set_turn(uint8_t state)
{
    ui_job_t *job = job_new(JOB_TURN);
    if (job == NULL) {
        return;
    }
    job->value = state;
    ui_post(job);
}

void ic_ui_set_battery(uint8_t pct)
{
    ui_job_t *job = job_new(JOB_BATT);
    if (job == NULL) {
        return;
    }
    job->value = pct;
    ui_post(job);
}

/* ---------------- 调试命令 ---------------- */

static void show_async(void *arg)
{
    (void)arg;
    ic_ui_show();
}

static void hide_async(void *arg)
{
    (void)arg;
    ic_ui_hide();
}

static int ic_cmd(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "show";

    if (strcmp(sub, "hide") == 0) {
        lv_async_call(hide_async, NULL);
        return 0;
    }

    int ret = 0;
    if (strcmp(sub, "stat") == 0) {
        const ic_proto_stat_t *ps = ic_proto_stat();
        LISA_LOGI(TAG, "proto frames=%u texts=%u ctl=%u resync=%u bad=%u trunc=%u",
                  (unsigned)ps->frames, (unsigned)ps->texts, (unsigned)ps->controls,
                  (unsigned)ps->resyncs, (unsigned)ps->bad_frames, (unsigned)ps->text_truncated);
        return 0;
    }
    if (strcmp(sub, "text") == 0 && argc >= 3) {
        ic_ui_add_text(IC_ROLE_ASSIST, argv[2], (uint16_t)strlen(argv[2]));
        return 0;
    }

    lv_async_call(show_async, NULL);
    return ret;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 ic, ic_cmd, ic show|hide|stat|text <s>);
