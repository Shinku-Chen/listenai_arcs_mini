#ifndef MINIAPP_DRAW_H
#define MINIAPP_DRAW_H

#include "miniapp.h"
#if MINIAPP_HAS_SCREEN
#include "lvgl.h"

/* UI-task only: font and LVGL access must remain on the display thread. */
void miniapp_draw_scene(lv_draw_ctx_t *ctx, const miniapp_scene_t *scene, int x, int y);
bool miniapp_scene_damage(const miniapp_scene_t *previous, const miniapp_scene_t *next, lv_area_t *area);
#endif
#endif
