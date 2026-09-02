/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

struct zmk_widget_nyan_cat {
  sys_snode_t node;
  lv_obj_t *obj;
};

int zmk_widget_nyan_cat_init(struct zmk_widget_nyan_cat *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_nyan_cat_obj(struct zmk_widget_nyan_cat *widget);
