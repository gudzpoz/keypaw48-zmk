/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "widgets/styling.h"

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_BATTERY_STATUS)
#include "widgets/battery_status.h"
static struct zmk_widget_rgb_battery_status battery_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_OUTPUT_STATUS)
#include "widgets/output_status.h"
static struct zmk_widget_rgb_output_status output_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_WPM_STATUS)
#include "widgets/wpm_status.h"
static struct zmk_widget_rgb_wpm_status wpm_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_PERIPHERAL_STATUS)
#include "widgets/peripheral_status.h"
static struct zmk_widget_rgb_peripheral_status peripheral_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_LAYER_STATUS)
#include "widgets/layer_status.h"
static struct zmk_widget_rgb_layer_status layer_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BAD_APPLE)
#include "widgets/bad_apple.h"
static struct zmk_widget_bad_apple bad_apple_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_NYAN_CAT)
#include "widgets/nyan_cat.h"
static struct zmk_widget_nyan_cat nyan_cat_widget;
#endif

lv_obj_t *zmk_display_status_screen() {
  lv_obj_t *screen = lv_obj_create(NULL);
  lv_aux_flat_container(screen);

  lv_obj_t *top = screen, *bottom = screen;

#define ALIGN_SOME(obj, prev, screen_align, align, rel) \
  {                                                     \
    lv_obj_t *v = (obj);                                \
    if (prev == screen)                                 \
      lv_obj_align(v, screen_align, 0, rel);            \
    else                                                \
      lv_obj_align_to(v, prev, align, 0, rel);          \
    prev = v;                                           \
  }

#define ALIGN_TOP(obj, rel)                                             \
  ALIGN_SOME(obj, top, LV_ALIGN_TOP_MID, LV_ALIGN_OUT_BOTTOM_MID, rel)

#define ALIGN_BOTTOM(obj, rel)                                          \
  ALIGN_SOME(obj, bottom, LV_ALIGN_BOTTOM_MID, LV_ALIGN_OUT_TOP_MID, -(rel))

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_BATTERY_STATUS)
  zmk_widget_rgb_battery_status_init(&battery_status_widget, screen);
  ALIGN_TOP(zmk_widget_rgb_battery_status_obj(&battery_status_widget), 0)
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_OUTPUT_STATUS)
  zmk_widget_rgb_output_status_init(&output_status_widget, screen);
  ALIGN_TOP(zmk_widget_rgb_output_status_obj(&output_status_widget), 0)
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_WPM_STATUS)
  zmk_widget_rgb_wpm_status_init(&wpm_status_widget, screen);
  ALIGN_BOTTOM(zmk_widget_rgb_wpm_status_obj(&wpm_status_widget), 0)
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_PERIPHERAL_STATUS)
  zmk_widget_rgb_peripheral_status_init(&peripheral_status_widget, screen);
  ALIGN_BOTTOM(zmk_widget_rgb_peripheral_status_obj(&peripheral_status_widget), 0)
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_RGB_LAYER_STATUS)
  zmk_widget_rgb_layer_status_init(&layer_status_widget, screen);
  ALIGN_BOTTOM(zmk_widget_rgb_layer_status_obj(&layer_status_widget), 0)
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BAD_APPLE)
  zmk_widget_bad_apple_init(&bad_apple_widget, screen);
  ALIGN_TOP(zmk_widget_bad_apple_obj(&bad_apple_widget), 2)
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_NYAN_CAT)
  zmk_widget_nyan_cat_init(&nyan_cat_widget, screen);
  ALIGN_BOTTOM(zmk_widget_nyan_cat_obj(&nyan_cat_widget), 2)
#endif

  return screen;
}
