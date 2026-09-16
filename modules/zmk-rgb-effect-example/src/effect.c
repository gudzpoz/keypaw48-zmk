/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Example custom RGB matrix effect, provided by a SEPARATE module to demonstrate the
 * zmk-rgb-matrix extension point: a "comet" whose bright head sweeps along the
 * board's x axis and fades out behind it. Adding this effect requires NO edits to
 * zmk-rgb-matrix -- only:
 *
 *   1. this compatible's binding yaml (dts/bindings/keypaw,rgb-matrix-example.yaml),
 *   2. this render function + a KP_RGB_EFFECT_DEFINE call, and
 *   3. an fx_example child node under &kprgb in the shield DTSI.
 *
 * The effect is referenced from a keymap by name (&fx_example).
 *
 * Like the built-in effects, the device is instantiated from its devicetree
 * compatible rather than from a node label, so renaming or omitting the node
 * does not break the build. This file only uses the public API in
 * <zmk/rgb_matrix.h>; the module needs no access to the engine's internals.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_example

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_example_config {
  /* Must embed the common config as its first member so the shared command
   * handlers can reach index/colour/duration. */
  struct kp_rgb_effect_common_config common;
  uint16_t tail_length; /* custom field: layout units the tail fades over */
};

struct kp_eff_example_data {
  struct kp_rgb_effect_common_data common;
  uint32_t phase_ms;
};

/* Render the comet: bright at the head position, fading to black behind it.
 * The head travels the board's x span (f->board_length, measured by the engine
 * from the physical layout) once per animation period. */
static void kp_eff_example_render(const struct device *dev, struct kp_rgb_frame *f) {
  struct kp_eff_example_data *data = dev->data;
  const struct kp_eff_example_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t phase = data->phase_ms % period;
  uint8_t pct = kp_rgb_brightness_pct(f);
  uint32_t board = MAX(f->board_length, 1u);
  int32_t head = (int32_t)(phase * board / period);
  int32_t tail = MAX(cfg->tail_length, 1);
  struct kp_rgb_hsb base = data->common.color;

  for (size_t i = 0; i < f->count; i++) {
    int32_t d = (int32_t)f->coords[i].x - head;
    if (d < 0) {
      d = -d;
    }

    struct kp_rgb_hsb hsb = base;
    hsb.b = d >= tail ? 0 : (uint8_t)((tail - d) * (uint32_t)base.b / tail);
    f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
  }

  data->phase_ms = (phase + f->elapsed) % period;
}

#define KP_EFF_EXAMPLE_DEFINE(inst)                                               \
  static const struct kp_eff_example_config kp_eff_example_##inst##_cfg = {        \
      .common = {.index = DT_PROP(DT_DRV_INST(inst), index)},                      \
      .tail_length = DT_PROP_OR(DT_DRV_INST(inst), tail_length, 200),              \
  };                                                                               \
  static struct kp_eff_example_data kp_eff_example_##inst##_data = {               \
      .common =                                                                    \
          {                                                                        \
              .color = KP_RGB_HSB_FROM_HEX(                                        \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0x00FFAA)),                 \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),           \
          },                                                                       \
  };                                                                               \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_example_render, NULL,             \
                       kp_eff_example_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_EXAMPLE_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
