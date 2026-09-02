/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "icons/icons.h"
#include "animation_util.h"

DEFINE_ANIMATION(icon_bluetooth_searching)

struct zmk_widget_rgb_output_status {
  sys_snode_t node;
  lv_obj_t *obj;
  lv_obj_t *usb;
  lv_obj_t *bt;
  lv_obj_t *label;
  lv_timer_t *timer;
  uint8_t anim_step;
  struct icon_bluetooth_searching_state anim;
};

int zmk_widget_rgb_output_status_init(struct zmk_widget_rgb_output_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_rgb_output_status_obj(struct zmk_widget_rgb_output_status *widget);
