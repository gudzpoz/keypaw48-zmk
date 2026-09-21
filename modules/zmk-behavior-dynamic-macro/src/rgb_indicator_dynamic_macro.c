/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * RGB matrix indicator kind for the dynamic macro recorder: paints its targeted
 * LEDs while &dm is recording a take. Living in the dynamic-macro module proves
 * the public indicator API (KP_RGB_INDICATOR_DEFINE + kp_rgb_indicator_paint) is
 * enough for a third-party kind, with no edit to zmk-rgb-matrix.
 *
 * The recorder only exists on the split central (CONFIG_ZMK_BEHAVIOR_DYNAMIC_
 * MACRO is gated that way). The device still has to exist and link on the
 * peripheral, because the shield's shared kp-rgb.dtsi lists it on &kprgb for
 * both halves, so the renderer compiles everywhere and simply stays dark where
 * the getter does not exist.
 */

#define DT_DRV_COMPAT keypaw_rgb_indicator_dynamic_macro

#include <zephyr/device.h>

#include <zmk/rgb_matrix.h>

#include "dynamic_macro_state.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_ind_dm_config {
  struct kp_rgb_indicator_common_config common;
};
struct kp_ind_dm_data {
  struct kp_rgb_indicator_common_data common;
};

static void kp_ind_dm_render(const struct device *dev, struct kp_rgb_frame *frame) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO)
  const struct kp_ind_dm_config *cfg = dev->config;
  const struct kp_ind_dm_data *data = dev->data;

  if (!zmk_dynamic_macro_is_recording()) {
    return;
  }

  kp_rgb_indicator_paint(frame, data->common.leds, data->common.led_count,
                         kp_hex_to_rgb(cfg->common.color), cfg->common.brightness);
#else
  /* Peripheral: no recorder here, so nothing to show. */
  ARG_UNUSED(dev);
  ARG_UNUSED(frame);
#endif
}

#define KP_IND_DM_DEFINE(inst)                                                 \
  KP_RGB_INDICATOR_TARGET_ARRAYS(inst, kp_ind_dm_##inst);                      \
  static const struct kp_ind_dm_config kp_ind_dm_##inst##_cfg = {              \
      .common = KP_RGB_INDICATOR_COMMON(DT_DRV_INST(inst), kp_ind_dm_##inst),  \
  };                                                                           \
  static struct kp_ind_dm_data kp_ind_dm_##inst##_data;                        \
  KP_RGB_INDICATOR_DEFINE(inst, kp_ind_dm_render, kp_ind_dm_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_IND_DM_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
