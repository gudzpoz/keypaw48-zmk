/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <zmk/display.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/endpoints.h>
#include <zmk/wpm.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "icons/icons.h"
#include "wpm_status.h"
#include "styling.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct wpm_status_state {
  uint8_t wpm;
};

struct wpm_status_state wpm_status_get_state(const zmk_event_t *eh) {
  return (struct wpm_status_state){.wpm = zmk_wpm_get_state()};
};

void set_wpm_symbol(struct zmk_widget_rgb_wpm_status *widget, struct wpm_status_state state) {
  char text[4] = {};
  lv_color_t color;

  LOG_DBG("WPM changed to %i", state.wpm);
  snprintf(text, sizeof(text), "%i", state.wpm);

  if (state.wpm < 30) {
    color = lv_color_hex(0xFFFFFF);
  } else if (state.wpm < 60) {
    color = lv_color_hex(0x00AA00);
  } else if (state.wpm < 80) {
    color = lv_color_hex(0xFFAA00);
  } else {
    color = lv_color_hex(0xCC0000);
  }

  lv_label_set_text(widget->label, text);
  lv_obj_set_style_text_color(widget->label, color, LV_PART_MAIN);
  lv_obj_set_style_image_recolor(widget->icon, color, LV_PART_MAIN);
}

void wpm_status_update_cb(struct wpm_status_state state) {
  struct zmk_widget_rgb_wpm_status *widget;
  SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_wpm_symbol(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_wpm_status, struct wpm_status_state, wpm_status_update_cb,
                            wpm_status_get_state)
ZMK_SUBSCRIPTION(widget_wpm_status, zmk_wpm_state_changed);

int zmk_widget_rgb_wpm_status_init(struct zmk_widget_rgb_wpm_status *widget, lv_obj_t *parent) {
  widget->obj = lv_obj_create(parent);
  lv_aux_flex_right(widget->obj);

  widget->label = lv_label_create(widget->obj);
  widget->icon = lv_image_create(widget->obj);
  lv_image_set_src(widget->icon, &icon_speed);
  lv_obj_set_style_image_recolor_opa(widget->icon, 255, LV_PART_MAIN);

  sys_slist_append(&widgets, &widget->node);

  widget_wpm_status_init();
  return 0;
}

lv_obj_t *zmk_widget_rgb_wpm_status_obj(struct zmk_widget_rgb_wpm_status *widget) { return widget->obj; }
