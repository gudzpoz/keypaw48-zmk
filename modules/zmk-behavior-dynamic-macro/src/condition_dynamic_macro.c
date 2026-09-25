/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Dynamic macro recording condition: active while &dm is recording a take.
 * Living in the dynamic-macro module proves the public condition API is enough
 * for a third-party predicate, with no edit to zmk-rgb-matrix.
 *
 * The recorder only exists on the split central. The device still has to exist
 * and link on the peripheral, because the shield's shared kp-rgb.dtsi expands
 * the node on both halves; the read is gated instead of the device. The consuming
 * overlay uses the default (central evaluation), so this is never called on a
 * peripheral -- but a `local` overlay would call it, so the unit must link.
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_dynamic_macro

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#include "dynamic_macro_state.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool kp_cond_dm_active(const struct device *dev) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO)
  ARG_UNUSED(dev);
  return zmk_dynamic_macro_is_recording();
#else
  /* Peripheral: no recorder here. The default overlay never calls this there
   * (the central evaluates and pushes the state), but a `local` overlay would,
   * and the unit still has to compile and link. */
  ARG_UNUSED(dev);
  return false;
#endif
}

#define KP_COND_DM_DEFINE(inst)                                                \
  KP_RGB_CONDITION_DEFINE(inst, kp_cond_dm_active, NULL)

DT_INST_FOREACH_STATUS_OKAY(KP_COND_DM_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
