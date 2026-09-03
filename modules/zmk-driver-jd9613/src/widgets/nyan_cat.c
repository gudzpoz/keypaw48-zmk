/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "icons/icons.h"
#include "nyan_cat.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

int zmk_widget_nyan_cat_init(struct zmk_widget_nyan_cat *widget, lv_obj_t *parent) {
  widget->obj = lv_animimg_create(parent);
  lv_animimg_set_src(widget->obj, (const void **) icon_anim_nyan_cat, ARRAY_SIZE(icon_anim_nyan_cat));
  lv_animimg_set_duration(widget->obj, 1500);
  lv_animimg_set_repeat_count(widget->obj, LV_ANIM_REPEAT_INFINITE);
  lv_animimg_start(widget->obj);

  sys_slist_append(&widgets, &widget->node);
  return 0;
}

lv_obj_t *zmk_widget_nyan_cat_obj(struct zmk_widget_nyan_cat *widget) { return widget->obj; }
