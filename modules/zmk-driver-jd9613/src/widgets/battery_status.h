/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

#include "icons/icons.h"
#include "animation_util.h"

DEFINE_ANIMATION(icon_battery_full)

struct zmk_widget_rgb_battery_status {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *label;
    lv_obj_t *icon;
    struct icon_battery_full_state anim;
};

int zmk_widget_rgb_battery_status_init(struct zmk_widget_rgb_battery_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_rgb_battery_status_obj(struct zmk_widget_rgb_battery_status *widget);
