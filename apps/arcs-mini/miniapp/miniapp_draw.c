#include "miniapp_draw.h"

#if MINIAPP_HAS_SCREEN
#include <string.h>
#include "lisa_ui_fonts.h"

static lv_area_t text_area(const miniapp_text_t *text)
{
    lv_point_t size;
    /* Legacy labels use LV_SIZE_CONTENT: long lines clip at the canvas edge,
     * rather than wrapping. Only an explicit width enables wrapping. */
    lv_coord_t width = text->width ? text->width : LV_COORD_MAX;
    lv_txt_get_size(&size, text->text, &lv_font_chinese_16, 0, 0, width, LV_TEXT_FLAG_NONE);
    return (lv_area_t){text->x, text->y, text->x + (text->width ? width : size.x) - 1, text->y + size.y - 1};
}

static lv_area_t shape_area(const miniapp_shape_t *shape)
{
    if (shape->kind == MINIAPP_SHAPE_RECT) {
        return (lv_area_t){shape->x, shape->y, shape->x + shape->w - 1, shape->y + shape->h - 1};
    }
    if (shape->kind == MINIAPP_SHAPE_LINE) {
        /* Conservative bound includes line caps and antialiasing. */
        int margin = shape->width + 1;
        return (lv_area_t){LV_MIN(shape->x, shape->w) - margin, LV_MIN(shape->y, shape->h) - margin,
                           LV_MAX(shape->x, shape->w) + margin, LV_MAX(shape->y, shape->h) + margin};
    }
    return (lv_area_t){shape->x - shape->w - 1, shape->y - shape->w - 1, shape->x + shape->w + 1,
                       shape->y + shape->w + 1};
}

static void add_damage(lv_area_t *area, bool *changed, lv_area_t item)
{
    const lv_area_t canvas = {0, 0, MINIAPP_SCREEN_WIDTH - 1, MINIAPP_SCREEN_HEIGHT - 1};
    if (item.x2 < item.x1 || item.y2 < item.y1 || !_lv_area_intersect(&item, &item, &canvas)) {
        return;
    }
    if (*changed) {
        _lv_area_join(area, area, &item);
    } else {
        *area = item;
    }
    *changed = true;
}

static void include_intersecting(lv_area_t *area, const miniapp_scene_t *scene)
{
    bool changed = true;
    lv_area_t overlap;
    for (unsigned i = 0; i < scene->shape_count; ++i) {
        lv_area_t bounds = shape_area(&scene->shapes[i]);
        if (_lv_area_intersect(&overlap, area, &bounds)) {
            add_damage(area, &changed, bounds);
        }
    }
    for (unsigned i = 0; i < scene->text_count; ++i) {
        lv_area_t bounds = text_area(&scene->texts[i]);
        if (_lv_area_intersect(&overlap, area, &bounds)) {
            add_damage(area, &changed, bounds);
        }
    }
}

bool miniapp_scene_damage(const miniapp_scene_t *previous, const miniapp_scene_t *next, lv_area_t *area)
{
    if (previous->background != next->background || previous->ordered != next->ordered) {
        *area = (lv_area_t){0, 0, MINIAPP_SCREEN_WIDTH - 1, MINIAPP_SCREEN_HEIGHT - 1};
        return true;
    }
    bool changed = false;
    for (unsigned i = 0; i < LV_MAX(previous->shape_count, next->shape_count); ++i) {
        if (i < previous->shape_count && i < next->shape_count &&
            memcmp(&previous->shapes[i], &next->shapes[i], sizeof(next->shapes[i])) == 0) {
            continue;
        }
        if (i < previous->shape_count) {
            add_damage(area, &changed, shape_area(&previous->shapes[i]));
        }
        if (i < next->shape_count) {
            add_damage(area, &changed, shape_area(&next->shapes[i]));
        }
    }
    for (unsigned i = 0; i < LV_MAX(previous->text_count, next->text_count); ++i) {
        if (i < previous->text_count && i < next->text_count &&
            memcmp(&previous->texts[i], &next->texts[i], sizeof(next->texts[i])) == 0) {
            continue;
        }
        if (i < previous->text_count) {
            add_damage(area, &changed, text_area(&previous->texts[i]));
        }
        if (i < next->text_count) {
            add_damage(area, &changed, text_area(&next->texts[i]));
        }
    }
    if (changed) {
        /* LVGL's antialias masks can round edge pixels differently when a
         * clip cuts through an unchanged primitive. Redraw whole intersecting
         * primitives, including text. Bound overlap-chain work per frame. */
        for (unsigned pass = 0; pass < 4; ++pass) {
            lv_area_t before = *area;
            include_intersecting(area, previous);
            include_intersecting(area, next);
            if (before.x1 == area->x1 && before.y1 == area->y1 && before.x2 == area->x2 && before.y2 == area->y2) {
                return true;
            }
        }
        *area = (lv_area_t){0, 0, MINIAPP_SCREEN_WIDTH - 1, MINIAPP_SCREEN_HEIGHT - 1};
    }
    return changed;
}

static void draw_shape(lv_draw_ctx_t *ctx, const miniapp_shape_t *shape, int x, int y)
{
    if (!shape->opacity) {
        return;
    }
    lv_area_t bounds = shape_area(shape);
    bounds.x1 += x;
    bounds.x2 += x;
    bounds.y1 += y;
    bounds.y2 += y;
    lv_area_t visible;
    if (!_lv_area_intersect(&visible, &bounds, ctx->clip_area)) {
        return;
    }
    if (shape->kind == MINIAPP_SHAPE_LINE) {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(shape->color);
        dsc.opa = shape->opacity;
        dsc.width = shape->width;
        dsc.round_start = dsc.round_end = shape->rounded;
        lv_point_t start = {x + shape->x, y + shape->y};
        lv_point_t end = {x + shape->w, y + shape->h};
        lv_draw_line(ctx, &dsc, &start, &end);
    } else if (shape->kind == MINIAPP_SHAPE_ARC) {
        lv_draw_arc_dsc_t dsc;
        lv_draw_arc_dsc_init(&dsc);
        dsc.color = lv_color_hex(shape->color);
        dsc.opa = shape->opacity;
        dsc.width = shape->width;
        dsc.rounded = shape->rounded;
        lv_point_t center = {x + shape->x, y + shape->y};
        lv_draw_arc(ctx, &dsc, &center, shape->w, shape->start_angle, shape->end_angle);
    } else {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(shape->color);
        dsc.bg_opa = shape->fill ? shape->opacity : LV_OPA_TRANSP;
        dsc.border_color = lv_color_hex(shape->border_color);
        dsc.border_width = shape->width;
        dsc.border_opa = shape->opacity;
        dsc.radius = shape->kind == MINIAPP_SHAPE_CIRCLE ? LV_RADIUS_CIRCLE : shape->radius;
        lv_area_t area;
        if (shape->kind == MINIAPP_SHAPE_CIRCLE) {
            area = (lv_area_t){x + shape->x - shape->w, y + shape->y - shape->w, x + shape->x + shape->w - 1,
                               y + shape->y + shape->w - 1};
        } else {
            area = (lv_area_t){x + shape->x, y + shape->y, x + shape->x + shape->w - 1, y + shape->y + shape->h - 1};
        }
        lv_draw_rect(ctx, &dsc, &area);
    }
}

static void draw_text(lv_draw_ctx_t *ctx, const miniapp_text_t *text, int x, int y)
{
    if (!text->opacity || !text->text[0]) {
        return;
    }
    lv_area_t area = text_area(text);
    area.x1 += x;
    area.x2 += x;
    area.y1 += y;
    area.y2 += y;
    lv_area_t visible;
    if (!_lv_area_intersect(&visible, &area, ctx->clip_area)) {
        return;
    }
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = &lv_font_chinese_16;
    dsc.flag = text->width ? LV_TEXT_FLAG_NONE : LV_TEXT_FLAG_FIT;
    dsc.color = lv_color_hex(text->color);
    dsc.opa = text->opacity;
    static const lv_text_align_t aligns[] = {LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT};
    dsc.align = aligns[text->align];
    lv_draw_label(ctx, &dsc, &area, text->text, NULL);
}

void miniapp_draw_scene(lv_draw_ctx_t *ctx, const miniapp_scene_t *scene, int x, int y)
{
    lv_draw_rect_dsc_t background;
    lv_draw_rect_dsc_init(&background);
    background.bg_color = lv_color_hex(scene->background);
    lv_area_t canvas = {x, y, x + MINIAPP_SCREEN_WIDTH - 1, y + MINIAPP_SCREEN_HEIGHT - 1};
    lv_draw_rect(ctx, &background, &canvas);
    unsigned shape = 0, text = 0;
    while (shape < scene->shape_count || text < scene->text_count) {
        bool next_shape = shape < scene->shape_count && (text == scene->text_count || !scene->ordered ||
                                                         scene->shapes[shape].order < scene->texts[text].order);
        if (next_shape) {
            draw_shape(ctx, &scene->shapes[shape++], x, y);
        } else {
            draw_text(ctx, &scene->texts[text++], x, y);
        }
    }
}
#endif
