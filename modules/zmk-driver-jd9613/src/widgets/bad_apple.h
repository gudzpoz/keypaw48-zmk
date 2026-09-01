/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

#define BAD_APPLE_WIDTH  128
#define BAD_APPLE_HEIGHT 128

struct zmk_widget_bad_apple {
    lv_obj_t *obj;
    lv_obj_t *canvas;
    lv_timer_t *timer;
    uint32_t frame;
    uint8_t cbuf[LV_CANVAS_BUF_SIZE(BAD_APPLE_WIDTH, BAD_APPLE_HEIGHT,
                                    LV_COLOR_FORMAT_GET_BPP(LV_COLOR_FORMAT_L8),
                                    LV_DRAW_BUF_STRIDE_ALIGN)];
};

int zmk_widget_bad_apple_init(struct zmk_widget_bad_apple *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_bad_apple_obj(struct zmk_widget_bad_apple *widget);

static inline void lv_aux_flat_container(lv_obj_t *obj) {
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}
