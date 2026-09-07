/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "icons/icons.h"
#include "animation.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

int zmk_widget_animation_init(struct zmk_widget_animation *widget, lv_obj_t *parent) {
  widget->obj = lv_animimg_create(parent);
#ifdef ZMK_WIDGET_ANIMATION_ICON
  lv_animimg_set_src(widget->obj, (const void **)ZMK_WIDGET_ANIMATION_ICON,
                     ARRAY_SIZE(ZMK_WIDGET_ANIMATION_ICON));
  lv_animimg_set_duration(widget->obj, CONFIG_ZMK_WIDGET_ANIMATION_DURATION);
#else
#error "Animation requires defining Kconfig CONFIG_ZMK_WIDGET_ANIMATION_SOURCE"
#endif
  lv_animimg_set_repeat_count(widget->obj, LV_ANIM_REPEAT_INFINITE);
  lv_animimg_start(widget->obj);

  sys_slist_append(&widgets, &widget->node);
  return 0;
}

lv_obj_t *zmk_widget_animation_obj(struct zmk_widget_animation *widget) { return widget->obj; }
