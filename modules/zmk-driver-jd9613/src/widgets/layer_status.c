/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include <zmk/display.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>

#include <dt-bindings/zmk/hid_indicators.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "icons/icons.h"
#include "layer_status.h"
#include "styling.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct layer_status_state {
  bool caps_lock;
  const char *labels[ZMK_WIDGET_RGB_LAYER_STATUS_MAX_N];
};

static void set_layer_symbols(struct zmk_widget_rgb_layer_status *widget, struct layer_status_state state) {
  for (int i = 0; i < ARRAY_SIZE(state.labels); i++) {
    const char *name = state.labels[i];
    lv_obj_t *label = widget->layers[i];
    if (name == NULL || strlen(name) == 0) {
      lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    } else {
      if (strcmp(lv_label_get_text(label), name) != 0) {
        lv_label_set_text(label, name);
      }
      lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
  }

  lv_image_set_src(widget->icon, state.caps_lock ? &icon_keyboard_capslock : &icon_keyboard);
  lv_obj_set_style_border_color(widget->icon, state.caps_lock ? COLOR_ACTIVE : COLOR_INACTIVE, LV_PART_MAIN);
  lv_obj_set_style_border_width(widget->icon, state.caps_lock ? 1 : 0, LV_PART_MAIN);
}

static void layer_status_update_cb(struct layer_status_state state) {
  struct zmk_widget_rgb_layer_status *widget;
  SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_symbols(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
  struct layer_status_state state = {
    .caps_lock = (zmk_hid_indicators_get_current_profile() &
                  HID_INDICATOR_CAPS_LOCK) != 0,
    .labels = {NULL},
  };

  int n = 0;
  for (zmk_keymap_layer_index_t l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(l);

    if (layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
      continue;
    }
    if (zmk_keymap_layer_active(layer_id)) {
      const char *name = zmk_keymap_layer_name(layer_id);
      if (name != NULL) {
        state.labels[n++] = name;
        if (n >= ZMK_WIDGET_RGB_LAYER_STATUS_MAX_N) {
          break;
        }
      }
    }
  }

  if (n == 0) {
    state.labels[0] = zmk_keymap_layer_name(zmk_keymap_layer_default());
  }

  return state;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(widget_layer_status, zmk_hid_indicators_changed);

static lv_style_t style_framed;
static bool style_inited = false;

int zmk_widget_rgb_layer_status_init(struct zmk_widget_rgb_layer_status *widget, lv_obj_t *parent) {
  widget->obj = lv_label_create(parent);
  lv_aux_flex_left(widget->obj);
  lv_obj_set_flex_flow(widget->obj, LV_FLEX_FLOW_ROW_WRAP);

  widget->icon = lv_image_create(widget->obj);
  lv_image_set_src(widget->icon, &icon_keyboard);
  lv_obj_set_style_image_recolor(widget->icon, COLOR_INACTIVE, LV_PART_MAIN);
  lv_obj_set_style_image_recolor_opa(widget->icon, 255, LV_PART_MAIN);

  if (!style_inited) {
    style_inited = true;
    lv_style_set_radius(&style_framed, 4);
    lv_style_set_border_color(&style_framed, COLOR_ACTIVE);
    lv_style_set_border_width(&style_framed, 1);
    lv_style_set_margin_left(&style_framed, 8);
  }

  for (int i = 0; i < ARRAY_SIZE(widget->layers); i++) {
    lv_obj_t *label = lv_label_create(widget->obj);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_style(label, &style_framed, 0);
    lv_obj_set_style_text_color(
        label, TABLEAU_COLORS[i % ARRAY_SIZE(TABLEAU_COLORS)], LV_PART_MAIN);
    widget->layers[i] = label;
  }

  sys_slist_append(&widgets, &widget->node);

  widget_layer_status_init();
  return 0;
}

lv_obj_t *zmk_widget_rgb_layer_status_obj(struct zmk_widget_rgb_layer_status *widget) {
  return widget->obj;
}
