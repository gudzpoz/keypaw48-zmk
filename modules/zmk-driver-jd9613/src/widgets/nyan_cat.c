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
  widget->obj = lv_gif_create(parent);
  lv_gif_set_src(widget->obj, &icon_nyan_cat);

  sys_slist_append(&widgets, &widget->node);
  return 0;
}

lv_obj_t *zmk_widget_nyan_cat_obj(struct zmk_widget_nyan_cat *widget) { return widget->obj; }
