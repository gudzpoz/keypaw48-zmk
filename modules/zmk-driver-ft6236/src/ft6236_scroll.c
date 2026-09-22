/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * FT6236 single-finger scroll input processor.
 *
 * Consumes the single-touch frames the ft6236 driver emits — raw panel pixels,
 * arriving on the central half via the split link — and turns the coordinates of
 * the pressed contact into relative wheel events (REL_HWHEEL / REL_WHEEL), which
 * the core input listener accumulates and sends as a mouse scroll report on the
 * frame's sync event.
 *
 * The two per-listener params are the only sensitivity knobs and they are in the
 * same units as the incoming coordinates: param1 is X pixels per wheel notch,
 * param2 is Y pixels per notch. Lower is faster. With
 * CONFIG_ZMK_POINTING_SMOOTH_SCROLLING the deltas are emitted in
 * FT6236_SCROLL_SUBCOUNTS (1/16) notch subunits, matching the Resolution
 * Multiplier (x16) declared on the mouse HID interface, so a notch still takes a
 * whole param-worth of pixels; the remainder carries the fraction not yet
 * emitted, which is what keeps the motion smooth instead of stepping a whole
 * notch at a time.
 *
 * Every event is swallowed (hollowed out) so the core ZMK input listener never
 * interprets INPUT_BTN_TOUCH as a mouse-button press; the rewritten REL events
 * are the only thing that reaches it. Hollowed events keep the sync flag so the
 * listener flushes accumulated wheel deltas at end of frame.
 *
 * Multi-finger gestures are not handled here: the driver classifies the contact
 * geometry and reports gestures as key events, which an earlier processor in the
 * listener's chain matches against behaviours.
 */

#define DT_DRV_COMPAT zmk_input_processor_ft6236_scroll

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include <drivers/input_processor.h>

LOG_MODULE_REGISTER(ft6236_scroll, CONFIG_FT6236_LOG_LEVEL);

/* Dummy event type used to hollow out events we have fully consumed. */
#define ZMK_INPUT_EV_DUMMY 0xFF

/*
 * Wheel subunits emitted per notch when smooth scrolling is enabled. Must
 * match the Resolution Multiplier advertised on the mouse interface: ZMK
 * declares a maximum multiplier of 16 and its feature report defaults to
 * 15, i.e. x16, which makes the listener's own scaling a pass-through.
 */
#define FT6236_SCROLL_SUBCOUNTS 16

struct ft6236_scroll_config {
  /** Natural scrolling: content follows the finger. */
  bool natural;
};

struct ft6236_scroll_data {
  /** Slot is currently pressed. */
  bool touching;
  /** Scroll baselines (panel pixels). */
  int32_t last_x;
  int32_t last_y;
  /** Whether a baseline has been captured for this touch. */
  bool have_last_x;
  bool have_last_y;
  /** Unemitted wheel fraction, in wheel subunits. */
  int16_t rem_x;
  int16_t rem_y;
  /* Per-touch totals. Logged when the contact lifts, so the px-per-notch
   * params can be checked against real travel: a drag of N pixels should
   * emit about N * FT6236_SCROLL_SUBCOUNTS / param subunits. */
  int32_t travelled_x;
  int32_t travelled_y;
  int32_t emitted_x;
  int32_t emitted_y;
};

/* Convert one axis delta (panel pixels) into wheel counts: notches plain, or
 * 1/16-notch subunits when smooth scrolling is enabled. `remainder` carries the
 * fraction of a count not yet emitted, so slow movement still eventually
 * produces output rather than being rounded away. */
static int32_t ft6236_scroll_wheel_delta(int32_t delta, uint32_t px_per_notch,
                                         int16_t *remainder) {
  int32_t acc = delta;
  int32_t divisor = (px_per_notch != 0) ? (int32_t)px_per_notch : 1;

  if (IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)) {
    acc *= FT6236_SCROLL_SUBCOUNTS;
  }
  acc += *remainder;

  int32_t out = acc / divisor;
  *remainder = (int16_t)(acc - out * divisor);
  return out;
}

static int ft6236_scroll_handle_event(const struct device *dev,
                                      struct input_event *evt,
                                      uint32_t param1, uint32_t param2,
                                      struct zmk_input_processor_state *state) {
  struct ft6236_scroll_data *data = dev->data;
  const struct ft6236_scroll_config *config = dev->config;

  if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH) {
    if (evt->value != 0 && !data->touching) {
      /* A new touch starts here: drop the baselines so the next coordinate
       * only establishes a reference, and start a fresh travel tally. The
       * lift branch already reset these, but a touch can also begin without
       * the driver having reported a lift first (e.g. after a reconnect). */
      data->have_last_x = false;
      data->have_last_y = false;
      data->rem_x = 0;
      data->rem_y = 0;
      data->travelled_x = 0;
      data->travelled_y = 0;
      data->emitted_x = 0;
      data->emitted_y = 0;
    } else if (evt->value == 0 && data->touching) {
      LOG_DBG("touch scroll: X %d px -> %d subunits, Y %d px -> %d subunits"
              " (%u/%u px per notch)",
              data->travelled_x, data->emitted_x, data->travelled_y, data->emitted_y, param1,
              param2);
    }

    data->touching = evt->value != 0;
  }

  if (data->touching && evt->type == INPUT_EV_ABS) {
    if (evt->code == INPUT_ABS_X) {
      if (!data->have_last_x) {
        /* First reading of a new touch: baseline only. */
        data->last_x = evt->value;
        data->have_last_x = true;
      } else {
        int32_t delta = (int32_t)evt->value - data->last_x;
        int32_t out = ft6236_scroll_wheel_delta(delta, param1, &data->rem_x);

        data->last_x = evt->value;
        data->travelled_x += delta;
        /* Natural horizontal: content follows the finger, so
         * panning inverts; traditional wheel-style does not. */
        if (config->natural) {
          out = -out;
        }
        if (out != 0) {
          data->emitted_x += out;
          evt->type = INPUT_EV_REL;
          evt->code = INPUT_REL_HWHEEL;
          evt->value = out;
          return ZMK_INPUT_PROC_CONTINUE;
        }
      }
    } else if (evt->code == INPUT_ABS_Y) {
      if (!data->have_last_y) {
        data->last_y = evt->value;
        data->have_last_y = true;
      } else {
        int32_t delta = (int32_t)evt->value - data->last_y;
        int32_t out = ft6236_scroll_wheel_delta(delta, param2, &data->rem_y);

        data->last_y = evt->value;
        data->travelled_y += delta;
        /* Natural vertical: finger up scrolls down (content
         * follows the finger); traditional inverts. */
        if (!config->natural) {
          out = -out;
        }
        if (out != 0) {
          data->emitted_y += out;
          evt->type = INPUT_EV_REL;
          evt->code = INPUT_REL_WHEEL;
          evt->value = out;
          return ZMK_INPUT_PROC_CONTINUE;
        }
      }
    }
  }

  /*
   * Swallow the event so the core listener ignores it: INPUT_BTN_TOUCH
   * must NOT become a left mouse click and non-REL events must not
   * trigger stray mouse reports. The sync flag is preserved so the
   * listener flushes accumulated wheel deltas at end of frame — the
   * final deltas of a lifting frame must still go out.
   */
  evt->type = ZMK_INPUT_EV_DUMMY;
  evt->code = 0;
  evt->value = 0;

  return ZMK_INPUT_PROC_CONTINUE;
}

static int ft6236_scroll_init(const struct device *dev) {
  struct ft6236_scroll_data *data = dev->data;

  data->touching = false;
  data->have_last_x = false;
  data->have_last_y = false;
  data->rem_x = 0;
  data->rem_y = 0;
  data->travelled_x = 0;
  data->travelled_y = 0;
  data->emitted_x = 0;
  data->emitted_y = 0;

  return 0;
}

static const struct zmk_input_processor_driver_api ft6236_scroll_api = {
  .handle_event = ft6236_scroll_handle_event,
};

#define FT6236_SCROLL_INIT(n)                                           \
  static struct ft6236_scroll_data ft6236_scroll_data_##n;              \
  static const struct ft6236_scroll_config ft6236_scroll_config_##n = { \
    .natural = DT_INST_PROP(n, scroll_natural),                         \
  };                                                                    \
  DEVICE_DT_INST_DEFINE(n, ft6236_scroll_init, NULL,                    \
                        &ft6236_scroll_data_##n,                        \
                        &ft6236_scroll_config_##n,                      \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                        &ft6236_scroll_api);

DT_INST_FOREACH_STATUS_OKAY(FT6236_SCROLL_INIT)
