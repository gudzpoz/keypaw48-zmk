/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "icons/icons.h"
#include "output_status.h"
#include "styling.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
  struct zmk_endpoint_instance selected_endpoint;
  enum zmk_transport preferred_transport;
  enum zmk_usb_conn_state usb_state;
  bool active_profile_connected;
  bool active_profile_bonded;
};

static struct output_status_state get_state(const zmk_event_t *_eh) {
  return (struct output_status_state){
    .selected_endpoint = zmk_endpoint_get_selected(),
    .preferred_transport = zmk_endpoint_get_preferred_transport(),
    .usb_state = zmk_usb_get_conn_state(),
    .active_profile_connected = zmk_ble_active_profile_is_connected(),
    .active_profile_bonded = !zmk_ble_active_profile_is_open(),
  };
}

static void set_status_symbol(struct zmk_widget_rgb_output_status *widget,
                              struct output_status_state state) {
  enum zmk_transport transport = state.selected_endpoint.transport;

  // If we aren't connected, show what we're *trying* to connect to.
  if (transport == ZMK_TRANSPORT_NONE) {
    transport = state.preferred_transport;
  }

  lv_image_set_src(widget->usb, state.usb_state == ZMK_USB_CONN_HID
                                    ? &icon_usb
                                    : &icon_usb_off);
  lv_image_set_src(widget->bt, state.active_profile_bonded
                   ? (state.active_profile_connected
                      ? &icon_bluetooth_connected
                      : &icon_bluetooth_disabled)
                   : &widget->anim.dsc);

  if (!state.active_profile_bonded && transport == ZMK_TRANSPORT_BLE) {
    lv_timer_resume(widget->timer);
  } else {
    lv_timer_pause(widget->timer);
  }

  lv_color_t usb_color = COLOR_INACTIVE;
  lv_color_t bt_color = COLOR_INACTIVE;

  char text[8] = {};
  if (state.active_profile_connected) {
    snprintf(text, sizeof(text), "%i",
             (int8_t)state.selected_endpoint.ble.profile_index + 1);
    lv_label_set_text(widget->label, text);
    lv_obj_clear_flag(widget->label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(widget->label, "");
    lv_obj_add_flag(widget->label, LV_OBJ_FLAG_HIDDEN);
  }

  if (transport == ZMK_TRANSPORT_USB && state.usb_state == ZMK_USB_CONN_HID) {
    usb_color = COLOR_ACTIVE;
  }
  if (transport == ZMK_TRANSPORT_BLE) {
    bt_color = COLOR_ACTIVE;
  }

  lv_obj_set_style_image_recolor(widget->usb, usb_color, LV_PART_MAIN);
  lv_obj_set_style_image_recolor(widget->bt, bt_color, LV_PART_MAIN);
  lv_obj_set_style_text_color(widget->label, bt_color, LV_PART_MAIN);
}

static void output_status_update_cb(struct output_status_state state) {
  struct zmk_widget_rgb_output_status *widget;
  SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_status_symbol(widget, state); }
}

static void output_status_anim_cb(lv_timer_t *timer) {
  struct zmk_widget_rgb_output_status *widget = lv_timer_get_user_data(timer);
  icon_bluetooth_searching_update(&widget->anim, widget->anim_step, 32);
  lv_obj_invalidate(widget->bt);
  widget->anim_step = (widget->anim_step + 50) % 150;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
// We don't get an endpoint changed event when the active profile connects/disconnects
// but there wasn't another endpoint to switch from/to, so update on BLE events too.
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

int zmk_widget_rgb_output_status_init(struct zmk_widget_rgb_output_status *widget, lv_obj_t *parent) {
  widget->obj = lv_obj_create(parent);
  lv_aux_flex_right(widget->obj);

  widget->timer = lv_timer_create(output_status_anim_cb, 500, widget);
  lv_timer_pause(widget->timer);

  widget->usb = lv_image_create(widget->obj);
  widget->bt = lv_image_create(widget->obj);
  widget->label = lv_label_create(widget->obj);

  lv_image_set_src(widget->usb, &icon_usb_off);
  lv_obj_set_style_image_recolor(widget->usb, COLOR_INACTIVE, LV_PART_MAIN);
  lv_obj_set_style_image_recolor_opa(widget->usb, 255, LV_PART_MAIN);

  lv_image_set_src(widget->bt, &icon_bluetooth_disabled);
  lv_obj_set_style_image_recolor(widget->bt, COLOR_INACTIVE, LV_PART_MAIN);
  lv_obj_set_style_image_recolor_opa(widget->bt, 255, LV_PART_MAIN);

  lv_label_set_text(widget->label, "");
  lv_obj_add_flag(widget->label, LV_OBJ_FLAG_HIDDEN);

  icon_bluetooth_searching_state_init(&widget->anim);
  widget->anim_step = 0;

  sys_slist_append(&widgets, &widget->node);

  widget_output_status_init();
  return 0;
}

lv_obj_t *zmk_widget_rgb_output_status_obj(struct zmk_widget_rgb_output_status *widget) {
  return widget->obj;
}
