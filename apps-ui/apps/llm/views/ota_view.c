/**
 * @file ota_view.c
 * @brief OTA status view implementation
 */

#define TAG "ota_view"

#include "ota_view.h"
#include "lisa_ui.h"
#include "lisa_ui_assets.h"
#include "lisa_ui_llm_base.h"
#include "lisa_ui_fonts.h"
#ifdef CONFIG_BOARD_ARCS_MINI3
#include "lisa_ui_llm_primary.h"
#endif
#include <stdio.h>

LV_IMG_DECLARE(icons_icon_finger_png);

typedef struct {
    lisa_ui_llm_base_t base;
    lv_obj_t *status_label;
    lv_obj_t *progress_label;
    lv_obj_t *step_label;
    lv_obj_t *progress_bar;
    lv_obj_t *package_info_container;
    lv_obj_t *package_info_label;
    lv_obj_t *version_label;
    lv_obj_t *eta_label;
    lv_obj_t *finger_icon;
    lv_obj_t *finger_hint_label;
#ifdef CONFIG_BOARD_ARCS_MINI3
    lv_obj_t *power_key_hint;
#endif
    char status_text[64];
    char progress_text[224];
    char step_text[48];
    char package_info_text[512];
    char version_text[80];
    char eta_text[96];
} lisa_ui_ota_view_t;

static void ota_view_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj);
static void ota_view_destructor(const lv_obj_class_t *class_p, lv_obj_t *obj);
static void ota_view_show_checking(lisa_ui_ota_view_t *view, const ota_state_t *state);
static void ota_view_show_package_info(lisa_ui_ota_view_t *view, const ota_state_t *state);
static void ota_view_show_action_hint(lisa_ui_ota_view_t *view, const char *text);
static void ota_view_hide_action_hint(lisa_ui_ota_view_t *view);

const lv_obj_class_t lisa_ui_ota_view_class = {
    .base_class = &lisa_ui_llm_base_class,
    .instance_size = sizeof(lisa_ui_ota_view_t),
    .constructor_cb = ota_view_constructor,
    .destructor_cb = ota_view_destructor,
};

static void ota_view_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    LV_UNUSED(class_p);

    lisa_ui_ota_view_t *view = (lisa_ui_ota_view_t *)obj;

    lv_obj_t *bar = lisa_ui_llm_base_bar_get(obj);
    if (bar) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *container = lisa_ui_llm_base_container_get(obj);
    if (!container) {
        return;
    }

    view->status_label = lv_label_create(container);
    lv_label_set_text(view->status_label, "正在获取更新信息…");
    lv_label_set_long_mode(view->status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(view->status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(view->status_label, &lv_font_chinese_16, 0);
    lv_obj_set_width(view->status_label, lv_pct(100));
    lv_obj_set_style_text_align(view->status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(view->status_label, LV_ALIGN_CENTER, 0, -16);

    view->progress_label = lv_label_create(container);
    lv_label_set_text(view->progress_label, "");
    lv_label_set_long_mode(view->progress_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(view->progress_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(view->progress_label, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_opa(view->progress_label, LV_OPA_70, 0);
    lv_obj_set_width(view->progress_label, lv_pct(100));
    lv_obj_set_style_text_align(view->progress_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(view->progress_label, LV_ALIGN_CENTER, 0, 8);

    view->step_label = lv_label_create(container);
    lv_label_set_text(view->step_label, "");
    lv_obj_set_style_text_font(view->step_label, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(view->step_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(view->step_label, LV_OPA_70, 0);
    lv_obj_align(view->step_label, LV_ALIGN_CENTER, 0, -70);
    lv_obj_add_flag(view->step_label, LV_OBJ_FLAG_HIDDEN);

    view->progress_bar = lv_bar_create(container);
    lv_obj_set_size(view->progress_bar, lv_pct(80), 6);
    lv_obj_align(view->progress_bar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(view->progress_bar, 0, 100);
    lv_obj_set_style_bg_color(view->progress_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view->progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(view->progress_bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(view->progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(view->progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(view->progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_add_flag(view->progress_bar, LV_OBJ_FLAG_HIDDEN);

    view->version_label = lv_label_create(container);
    lv_obj_set_style_text_font(view->version_label, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(view->version_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(view->version_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(view->version_label, lv_pct(90));
    lv_obj_align(view->version_label, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(view->version_label, LV_OBJ_FLAG_HIDDEN);

    /* 说明单独裁剪，滚动时不覆盖版本和底部操作提示。 */
    view->package_info_container = lv_obj_create(container);
    lv_obj_set_width(view->package_info_container, lv_pct(90));
    lv_obj_set_style_pad_all(view->package_info_container, 0, 0);
    lv_obj_set_style_border_width(view->package_info_container, 0, 0);
    lv_obj_set_style_radius(view->package_info_container, 0, 0);
    lv_obj_set_style_bg_opa(view->package_info_container, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(view->package_info_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(view->package_info_container, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_add_flag(view->package_info_container, LV_OBJ_FLAG_HIDDEN);

    view->package_info_label = lv_label_create(view->package_info_container);
    lv_label_set_text(view->package_info_label, "");
    lv_label_set_long_mode(view->package_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(view->package_info_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(view->package_info_label, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_opa(view->package_info_label, LV_OPA_70, 0);
    lv_obj_set_width(view->package_info_label, lv_pct(100));
    lv_obj_set_style_text_align(view->package_info_label, LV_TEXT_ALIGN_LEFT, 0);

    view->eta_label = lv_label_create(container);
    lv_label_set_text(view->eta_label, "");
    lv_label_set_long_mode(view->eta_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(view->eta_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(view->eta_label, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_opa(view->eta_label, LV_OPA_50, 0);
    lv_obj_set_width(view->eta_label, lv_pct(100));
    lv_obj_set_style_text_align(view->eta_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(view->eta_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    view->finger_icon = lv_img_create(obj);
    lv_img_set_src(view->finger_icon, &icons_icon_finger_png);
    lv_obj_add_flag(view->finger_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->finger_icon, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_align(view->finger_icon, LV_ALIGN_BOTTOM_LEFT, 12, -16);
    lv_obj_move_foreground(view->finger_icon);

    view->finger_hint_label = lv_label_create(obj);
    lv_label_set_text(view->finger_hint_label, "");
    lv_obj_set_style_text_color(view->finger_hint_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(view->finger_hint_label, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_align(view->finger_hint_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(view->finger_hint_label, LV_PCT(70));
    lv_label_set_long_mode(view->finger_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(view->finger_hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->finger_hint_label, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_align_to(view->finger_hint_label, view->finger_icon, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_move_foreground(view->finger_hint_label);

#ifdef CONFIG_BOARD_ARCS_MINI3
    view->power_key_hint = lisa_ui_llm_primary_power_key_hint_create(obj);
    if (view->power_key_hint) {
        lv_obj_add_flag(view->power_key_hint, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

static void ota_view_show_checking(lisa_ui_ota_view_t *view, const ota_state_t *state)
{
    const char *status_text = "正在检查更新…";

    if (view == NULL) {
        return;
    }

    if (state != NULL) {
        status_text = (state->target == OTA_TARGET_APP) ? "正在检查系统更新…" : "正在检查资源更新…";
    }

    lv_label_set_text_static(view->status_label, status_text);
    lv_label_set_text_static(view->progress_label, "");
    lv_label_set_text_static(view->eta_label, "");
}

static void ota_view_show_action_hint(lisa_ui_ota_view_t *view, const char *text)
{
    if (view == NULL || view->finger_icon == NULL || view->finger_hint_label == NULL) {
        return;
    }

    lv_label_set_text_static(view->finger_hint_label, text ? text : "");
    lv_obj_align(view->finger_icon, LV_ALIGN_BOTTOM_LEFT, 12, -16);
    lv_obj_align_to(view->finger_hint_label, view->finger_icon, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_move_foreground(view->finger_icon);
    lv_obj_move_foreground(view->finger_hint_label);
    lv_obj_clear_flag(view->finger_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(view->finger_hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->eta_label, LV_OBJ_FLAG_HIDDEN);
}

static void ota_view_hide_action_hint(lisa_ui_ota_view_t *view)
{
    if (view == NULL) {
        return;
    }

    if (view->finger_icon != NULL) {
        lv_obj_add_flag(view->finger_icon, LV_OBJ_FLAG_HIDDEN);
    }

    if (view->finger_hint_label != NULL) {
        lv_obj_add_flag(view->finger_hint_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (view->eta_label != NULL) {
        lv_obj_clear_flag(view->eta_label, LV_OBJ_FLAG_HIDDEN);
    }

#ifdef CONFIG_BOARD_ARCS_MINI3
    if (view->power_key_hint != NULL) {
        lv_obj_add_flag(view->power_key_hint, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

static void ota_view_scroll_notes(void *obj, int32_t y)
{
    lv_obj_set_y(obj, y);
}

static void ota_view_show_package_info(lisa_ui_ota_view_t *view, const ota_state_t *state)
{
    if (!view || !state) {
        return;
    }

    lv_label_set_text_static(view->status_label, "发现系统更新");
    lv_obj_align(view->status_label, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_clear_flag(view->status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->progress_label, LV_OBJ_FLAG_HIDDEN);
    snprintf(view->version_text, sizeof(view->version_text), "%s → %s",
             state->current_version[0] ? state->current_version : "当前版本",
             state->target_version[0] ? state->target_version : "新版本");
    lv_label_set_text_static(view->version_label, view->version_text);
    lv_obj_clear_flag(view->version_label, LV_OBJ_FLAG_HIDDEN);

    snprintf(view->package_info_text, sizeof(view->package_info_text), "%s",
             state->update_notes[0] ? state->update_notes : "检测到新的系统版本，建议完成更新。");
    lv_label_set_text_static(view->package_info_label, view->package_info_text);
    lv_obj_t *container = lisa_ui_llm_base_container_get((lv_obj_t *)view);
    lv_obj_update_layout(container);
    lv_coord_t notes_height = lv_obj_get_content_height(container) - 76 - 60;
    lv_obj_set_height(view->package_info_container, LV_MAX(notes_height, 32));
    lv_obj_clear_flag(view->package_info_container, LV_OBJ_FLAG_HIDDEN);
    lv_anim_del(view->package_info_label, ota_view_scroll_notes);
    lv_obj_set_y(view->package_info_label, 0);
    /* 重新按文本测量，包含末行字模的下沿，避免滚到底仍缺笔画。 */
    lv_obj_set_height(view->package_info_label, LV_SIZE_CONTENT);
    lv_obj_update_layout(view->package_info_container);
    lv_obj_set_height(view->package_info_label, lv_obj_get_height(view->package_info_label) + 4);
    lv_obj_update_layout(view->package_info_container);
    lv_coord_t overflow = lv_obj_get_height(view->package_info_label) -
                          lv_obj_get_content_height(view->package_info_container);
    if (overflow > 0) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, view->package_info_label);
        lv_anim_set_exec_cb(&anim, ota_view_scroll_notes);
        lv_anim_set_values(&anim, 0, -overflow);
        lv_anim_set_time(&anim, lv_anim_speed_to_time(16, 0, overflow));
        lv_anim_set_delay(&anim, 3000);
        lv_anim_set_playback_time(&anim, lv_anim_speed_to_time(32, 0, overflow));
        lv_anim_set_playback_delay(&anim, 3000);
        lv_anim_set_repeat_delay(&anim, 3000);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&anim);
    }
    ota_view_show_action_hint(view, "单击：更新\n长按：下次再说");
#ifdef CONFIG_BOARD_ARCS_MINI3
    lv_obj_add_flag(view->finger_icon, LV_OBJ_FLAG_HIDDEN);
    if (view->power_key_hint) {
        lv_obj_clear_flag(view->power_key_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(view->power_key_hint);
    }
#endif
}

static void ota_view_destructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    (void)class_p;
    (void)obj;
}

lv_obj_t *lisa_ui_ota_view_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&lisa_ui_ota_view_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

static const char *ota_target_name(ota_target_e target)
{
    switch (target) {
    case OTA_TARGET_APP:
        return "固件";
    case OTA_TARGET_WAKE_WORD:
        return "唤醒词";
    case OTA_TARGET_PROMPT_TONE:
        return "提示音";
    case OTA_TARGET_EMOJI:
        return "表情";
    default:
        return "资源";
    }
}

void lisa_ui_ota_view_update(lv_obj_t *obj, const ota_state_t *state)
{
    if (!obj || !state || !lv_obj_has_class(obj, &lisa_ui_ota_view_class)) {
        return;
    }

    lisa_ui_ota_view_t *view = (lisa_ui_ota_view_t *)obj;
    lv_obj_add_flag(view->progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->step_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(view->status_label, LV_ALIGN_CENTER, 0, -16);
    lv_obj_align(view->progress_label, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_text_font(view->progress_label, &lv_font_chinese_16, 0);
    if (state->state != OTA_STATE_PACKAGE_INFO) {
        ota_view_hide_action_hint(view);
        lv_obj_add_flag(view->package_info_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->version_label, LV_OBJ_FLAG_HIDDEN);
        lv_anim_del(view->package_info_label, ota_view_scroll_notes);
        lv_obj_clear_flag(view->status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(view->progress_label, LV_OBJ_FLAG_HIDDEN);
    }

    switch (state->state) {
    case OTA_STATE_CHECKING:
        ota_view_show_checking(view, state);
        break;
    case OTA_STATE_PACKAGE_INFO:
        ota_view_show_package_info(view, state);
        break;
    case OTA_STATE_UPDATING: {
        lv_obj_align(view->status_label, LV_ALIGN_CENTER, 0, -42);
        lv_obj_align(view->progress_label, LV_ALIGN_CENTER, 0, 28);
        if (state->target == OTA_TARGET_APP) {
            snprintf(view->status_text, sizeof(view->status_text), "正在下载系统更新…");
        } else {
            snprintf(view->status_text, sizeof(view->status_text), "正在更新%s",
                     ota_target_name(state->target));
        }
        lv_label_set_text_static(view->status_label, view->status_text);
        if (state->update_total > 1) {
            snprintf(view->step_text, sizeof(view->step_text), "第 %u 项，共 %u 项",
                     state->update_index, state->update_total);
            lv_label_set_text_static(view->step_label, view->step_text);
            lv_obj_clear_flag(view->step_label, LV_OBJ_FLAG_HIDDEN);
        }

        /* 百分比仅表示当前资源已处理的字节，100% 后仍要等待更新结果。 */
        if (state->bytes_total > 0) {
            uint32_t percent = state->bytes_processed >= state->bytes_total ? 100 :
                (uint32_t)((uint64_t)state->bytes_processed * 100 / state->bytes_total);
            lv_bar_set_value(view->progress_bar, percent, LV_ANIM_OFF);
            lv_obj_clear_flag(view->progress_bar, LV_OBJ_FLAG_HIDDEN);
            if (percent == 100) {
                lv_obj_set_style_text_font(view->progress_label, &lv_font_chinese_16, 0);
                lv_label_set_text_static(view->progress_label, "正在完成更新…");
            } else {
                snprintf(view->progress_text, sizeof(view->progress_text), "%u%%", percent);
                lv_label_set_text_static(view->progress_label, view->progress_text);
            }
        } else {
            if (state->bytes_processed >= 1024U * 1024U) {
                uint32_t whole = state->bytes_processed / (1024U * 1024U);
                uint32_t fraction = (state->bytes_processed % (1024U * 1024U)) * 10U / (1024U * 1024U);
                snprintf(view->progress_text, sizeof(view->progress_text), "已下载 %u.%u MB", whole, fraction);
            } else if (state->bytes_processed > 0) {
                snprintf(view->progress_text, sizeof(view->progress_text), "已下载 %u KB",
                         state->bytes_processed / 1024U);
            } else {
                snprintf(view->progress_text, sizeof(view->progress_text), "正在下载…");
            }
            lv_label_set_text_static(view->progress_label, view->progress_text);
        }

        /* 起步阶段不猜剩余时间；资源切换时 bytes_processed=0 会清除旧 ETA。 */
        if (state->bytes_total > state->bytes_processed &&
            state->bytes_processed >= 16384 && state->elapsed_ms >= 3000) {
            uint64_t remaining_ms = (uint64_t)(state->bytes_total - state->bytes_processed) *
                                    state->elapsed_ms / state->bytes_processed;
            uint32_t eta_min = (uint32_t)((remaining_ms + 59999) / 60000);
            if (eta_min <= 1) {
                snprintf(view->eta_text, sizeof(view->eta_text), "本项预计不到 1 分钟\n请保持设备开机");
            } else {
                snprintf(view->eta_text, sizeof(view->eta_text), "本项预计约 %u 分钟\n请保持设备开机", eta_min);
            }
            lv_label_set_text_static(view->eta_label, view->eta_text);
        } else {
            lv_label_set_text_static(view->eta_label, "请保持设备开机");
        }
        break;
    }
    case OTA_STATE_SUCCESSED:
        if (state->target == OTA_TARGET_APP) {
            lv_label_set_text_static(view->status_label, "下载完毕");
        } else {
            lv_label_set_text_static(view->status_label, "更新完毕");
        }
        lv_label_set_text_static(view->progress_label, "");
        if (state->reboot == OTA_REBOOT_STRATEGY_AUTO) {
            lv_label_set_text_static(view->eta_label, "正在重启…");
        } else {
            lv_label_set_text_static(view->eta_label, "请手动重启设备");
        }
        break;
    case OTA_STATE_APP_FAILED:
        lv_obj_align(view->status_label, LV_ALIGN_CENTER, 0, -24);
        lv_obj_align(view->progress_label, LV_ALIGN_CENTER, 0, 16);
        lv_label_set_text_static(view->status_label, "系统更新失败");
        lv_label_set_text_static(view->progress_label, "稍后重启设备可重新检查");
        lv_label_set_text_static(view->eta_label, "即将返回主页");
        break;
    case OTA_STATE_RESOURCE_FAILED:
        lv_label_set_text_static(view->status_label, "资源更新失败");
        lv_label_set_text_static(view->progress_label, "");
        if (state->reboot == OTA_REBOOT_STRATEGY_AUTO) {
            lv_label_set_text_static(view->eta_label, "正在重启…");
        } else {
            lv_label_set_text_static(view->eta_label, "请手动重启设备");
        }
        break;
    case OTA_STATE_UP_TO_DATE:
        // lv_label_set_text_static(view->status_label, "已完成更新检查");
        // lv_label_set_text_static(view->progress_label, "");
        // lv_label_set_text_static(view->eta_label, "");
        break;
    default:
        break;
    }
}
