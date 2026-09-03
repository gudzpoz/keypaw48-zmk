/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "battery_status.h"
#include "styling.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct battery_status_state {
  uint8_t level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
  bool usb_present;
#endif
};

static void set_battery_symbol(struct zmk_widget_rgb_battery_status *widget,
                               struct battery_status_state state) {
  uint8_t level = state.level;
  lv_color_t color;

  char text[12] = {0};
  int chars = snprintf(text, sizeof(text), "%3u%%", level);

  if (level > 30) {
    color = lv_color_hex(0xEEEEEE);
  } else if (level >= 10) {
    color = lv_color_hex(0xFFCC22);
  } else {
    color = lv_color_hex(0xFF6644);
  }

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
  if (state.usb_present) {
    strncpy(text + chars, " " LV_SYMBOL_CHARGE, sizeof(text) - chars - 1);
    color = lv_color_hex(0x77DDCC);
  }
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

  lv_label_set_text(widget->label, text);
  lv_obj_set_style_text_color(widget->label, color, LV_PART_MAIN);

  icon_battery_full_update(&widget->anim, level, 128);
  lv_obj_set_style_image_recolor(widget->icon, color, LV_PART_MAIN);
  lv_obj_invalidate(widget->icon);
}

void battery_status_update_cb(struct battery_status_state state) {
  struct zmk_widget_rgb_battery_status *widget;
  SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_symbol(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
  const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

  return (struct battery_status_state){
    .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
  };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

int zmk_widget_rgb_battery_status_init(struct zmk_widget_rgb_battery_status *widget, lv_obj_t *parent) {
  widget->obj = lv_obj_create(parent);
  lv_aux_flex_right(widget->obj);

  widget->label = lv_label_create(widget->obj);
  lv_label_set_text(widget->label, "...%");

  widget->icon = lv_image_create(widget->obj);
  icon_battery_full_state_init(&widget->anim);
  lv_image_set_src(widget->icon, &widget->anim.dsc);
  lv_obj_set_style_image_recolor(widget->icon, lv_color_hex(0x884444), LV_PART_MAIN);
  lv_obj_set_style_image_recolor_opa(widget->icon, 255, LV_PART_MAIN);

  sys_slist_append(&widgets, &widget->node);

  widget_battery_status_init();
  return 0;
}

lv_obj_t *zmk_widget_rgb_battery_status_obj(struct zmk_widget_rgb_battery_status *widget) {
  return widget->obj;
}
