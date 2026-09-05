/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * FT6236 touchpad input processor + gesture engine.
 *
 * Consumes the multitouch input events emitted by the ft6236 driver (which
 * arrive on the central half via the split link) and discriminates gestures
 * by contact count:
 *
 *  - One contact: single-finger scroll. Deltas of the active contact are
 *    converted into relative wheel events (REL_HWHEEL / REL_WHEEL) which the
 *    core input listener accumulates and sends as a mouse scroll report on
 *    the frame's sync event. When CONFIG_ZMK_POINTING_SMOOTH_SCROLLING is
 *    enabled the deltas are emitted in 1/16-notch subunits, matching the
 *    Resolution Multiplier (x16) declared on the mouse HID interface.
 *  - Two or more contacts: forwarded as Windows Precision Touchpad (PTP) HID
 *    reports so the host handles gestures such as pinch-zoom itself.
 *
 * Every event is swallowed (hollowed out) so the core ZMK input listener
 * never interprets INPUT_BTN_TOUCH as a mouse-button press; in scroll mode
 * the rewritten REL events are the only thing that reaches it. Hollowed
 * events keep the sync flag so the listener can flush accumulated wheel
 * deltas at end of frame — except in PTP mode, where sync is cleared because
 * the PTP report is sent from here.
 */

#define DT_DRV_COMPAT zmk_input_processor_ft6236_touchpad

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include <drivers/input_processor.h>
#include <zmk/ft6236_touchpad.h>

LOG_MODULE_REGISTER(ft6236_touchpad_proc, CONFIG_ZMK_LOG_LEVEL);

/* Dummy event type used to hollow out events we have fully consumed. */
#define ZMK_INPUT_EV_DUMMY 0xFF

/*
 * Wheel subunits emitted per notch when smooth scrolling is enabled. Must
 * match the Resolution Multiplier advertised on the mouse interface: ZMK
 * declares a maximum multiplier of 16 and its feature report defaults to
 * 15, i.e. x16, which makes the listener's own scaling a pass-through.
 */
#define FT6236_TP_SCROLL_SUBCOUNTS 16

#define FT6236_PROC_MAX_CONTACTS FT6236_TOUCHPAD_MAX_CONTACTS

enum ft6236_proc_mode {
  FT6236_PROC_MODE_IDLE,
  FT6236_PROC_MODE_SCROLL,
  FT6236_PROC_MODE_PTP,
};

struct ft6236_proc_contact {
  bool active;
  uint8_t id;
  uint16_t x;
  uint16_t y;
};

struct ft6236_touchpad_proc_config {
  /** Natural scrolling: content follows the finger. */
  bool natural;
};

struct ft6236_touchpad_proc_data {
  uint8_t slot;
  struct ft6236_proc_contact contacts[FT6236_PROC_MAX_CONTACTS];
  /* Gesture engine state. */
  enum ft6236_proc_mode mode;
  /** Slot of the contact driving single-finger scroll. */
  uint8_t scroll_slot;
  /** Scroll baselines (logical coordinates). */
  int32_t last_x;
  int32_t last_y;
  /** Unemitted wheel fraction, in wheel subunits. */
  int16_t rem_x;
  int16_t rem_y;
  bool have_last_x;
  bool have_last_y;
};

static uint8_t ft6236_proc_active_count(const struct ft6236_touchpad_proc_data *data) {
  uint8_t count = 0;

  for (uint8_t i = 0; i < FT6236_PROC_MAX_CONTACTS; i++) {
    if (data->contacts[i].active) {
      count++;
    }
  }
  return count;
}

static int8_t ft6236_proc_first_active_slot(const struct ft6236_touchpad_proc_data *data) {
  for (uint8_t i = 0; i < FT6236_PROC_MAX_CONTACTS; i++) {
    if (data->contacts[i].active) {
      return (int8_t)i;
    }
  }
  return -1;
}

static bool ft6236_proc_any_active(const struct ft6236_touchpad_proc_data *data) {
  return ft6236_proc_active_count(data) != 0;
}

/* Build and send a PTP report straight from the contact table: every slot is
 * reported (lifted slots keep their last known position and id, as PTP
 * expects for final frames), contact_count carries the active count. */
static void ft6236_proc_send_ptp_report(const struct ft6236_touchpad_proc_data *data) {
  struct ft6236_touchpad_report report;
  uint8_t count = 0;

  memset(&report, 0, sizeof(report));
  for (uint8_t i = 0; i < FT6236_PROC_MAX_CONTACTS; i++) {
    const struct ft6236_proc_contact *c = &data->contacts[i];

    report.contacts[i].active = c->active;
    report.contacts[i].id = c->id;
    report.contacts[i].x = c->x;
    report.contacts[i].y = c->y;
    if (c->active) {
      count++;
    }
  }
  report.contact_count = count;
  report.button = false;

  int ret = ft6236_touchpad_send_report(&report);
  if (ret < 0) {
    LOG_DBG("PTP report send failed: %d", ret);
  }
}

/* Force-lift every contact (surface switch / mode change / 2->1 finger
 * transition) so the host cleanly ends any in-flight gesture. */
static void ft6236_proc_send_ptp_lift_all(struct ft6236_touchpad_proc_data *data) {
  for (uint8_t i = 0; i < FT6236_PROC_MAX_CONTACTS; i++) {
    data->contacts[i].active = false;
  }
  ft6236_proc_send_ptp_report(data);
}

/* Recompute the gesture mode from the contact table, handling the
 * transitions between single-finger scroll and PTP forwarding. */
static void ft6236_proc_update_mode(struct ft6236_touchpad_proc_data *data) {
  uint8_t count = ft6236_proc_active_count(data);

  if (count == 0) {
    data->mode = FT6236_PROC_MODE_IDLE;
    data->have_last_x = false;
    data->have_last_y = false;
    return;
  }

  if (count == 1) {
    int8_t slot = ft6236_proc_first_active_slot(data);

    if (data->mode == FT6236_PROC_MODE_PTP) {
      /* 2 -> 1 fingers: end the two-finger gesture so the host
       * does not see a lingering contact, then scroll with the
       * survivor starting next frame. */
      ft6236_proc_send_ptp_lift_all(data);
      data->mode = FT6236_PROC_MODE_SCROLL;
      data->have_last_x = false;
      data->have_last_y = false;
      data->rem_x = 0;
      data->rem_y = 0;
    } else if (data->mode == FT6236_PROC_MODE_IDLE) {
      data->mode = FT6236_PROC_MODE_SCROLL;
      data->have_last_x = false;
      data->have_last_y = false;
      data->rem_x = 0;
      data->rem_y = 0;
    }

    if (slot >= 0 && (uint8_t)slot != data->scroll_slot) {
      /* Contact identity changed; scroll baselines must follow. */
      data->scroll_slot = (uint8_t)slot;
      data->have_last_x = false;
      data->have_last_y = false;
    }
    return;
  }

  /* count >= 2: hand everything to the host. Wheel deltas rewritten
   * earlier in this frame are not flushed now (PTP mode clears the
   * sync flag); they linger in the listener's accumulator and go out
   * with the next scroll-mode frame's sync. The PTP frame sent on
   * sync reports both contacts, giving the host a clean 2-finger
   * gesture (pinch / two-finger scroll). */
  data->mode = FT6236_PROC_MODE_PTP;
}

/* Convert one axis delta (logical units) into wheel counts: notches plain,
 * or 1/16-notch subunits when smooth scrolling is enabled. `remainder`
 * carries the fraction of a count not yet emitted. The divisor is in
 * logical units per notch regardless of subunit mode. */
static int32_t ft6236_proc_wheel_delta(int32_t delta, uint32_t div, int16_t *remainder) {
  int32_t acc = delta;
  int32_t divisor = (div != 0) ? (int32_t)div : 1;

  if (IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)) {
    acc *= FT6236_TP_SCROLL_SUBCOUNTS;
  }
  acc += *remainder;

  int32_t out = acc / divisor;
  *remainder = (int16_t)(acc - out * divisor);
  return out;
}

static int ft6236_touchpad_proc_handle_event(const struct device *dev,
                                             struct input_event *evt,
                                             uint32_t param1, uint32_t param2,
                                             struct zmk_input_processor_state *state) {
  struct ft6236_touchpad_proc_data *data = dev->data;
  const struct ft6236_touchpad_proc_config *config = dev->config;

  if (!ft6236_touchpad_surface_enabled()) {
    /* Host disabled the surface (or set mouse input mode): force
     * everything up once and stay silent. */
    if (ft6236_proc_any_active(data)) {
      ft6236_proc_send_ptp_lift_all(data);
    }
    data->mode = FT6236_PROC_MODE_IDLE;
    data->have_last_x = false;
    data->have_last_y = false;
    evt->type = ZMK_INPUT_EV_DUMMY;
    evt->code = 0;
    evt->value = 0;
    evt->sync = false;
    return ZMK_INPUT_PROC_CONTINUE;
  }

  switch (evt->type) {
  case INPUT_EV_ABS:
    switch (evt->code) {
    case INPUT_ABS_MT_SLOT:
      data->slot = (uint8_t)evt->value;
      if (data->slot >= FT6236_PROC_MAX_CONTACTS) {
        data->slot = FT6236_PROC_MAX_CONTACTS - 1;
      }
      break;
    case INPUT_ABS_X:
      data->contacts[data->slot].x = (uint16_t)evt->value;
      break;
    case INPUT_ABS_Y:
      data->contacts[data->slot].y = (uint16_t)evt->value;
      break;
    default:
      break;
    }
    break;
  case INPUT_EV_KEY:
    if (evt->code == INPUT_BTN_TOUCH) {
      data->contacts[data->slot].active = (evt->value != 0);
      data->contacts[data->slot].id = data->slot;
      ft6236_proc_update_mode(data);
    }
    break;
  default:
    break;
  }

  /* On the sync of a frame in PTP mode, build + send the PTP report. */
  if (evt->sync && data->mode == FT6236_PROC_MODE_PTP) {
    ft6236_proc_send_ptp_report(data);
  }

  /* In scroll mode, rewrite the scrolling contact's ABS events into
   * wheel deltas and let them through to the core listener; hollow out
   * everything else. */
  if (data->mode == FT6236_PROC_MODE_SCROLL && evt->type == INPUT_EV_ABS &&
      data->slot == data->scroll_slot) {
    bool natural = config->natural;

    if (evt->code == INPUT_ABS_X) {
      if (!data->have_last_x) {
        /* First reading of a new gesture: baseline only. */
        data->last_x = evt->value;
        data->have_last_x = true;
      } else {
        int32_t delta = (int32_t)evt->value - data->last_x;
        int32_t out = ft6236_proc_wheel_delta(delta, param1, &data->rem_x);

        data->last_x = evt->value;
        /* Natural horizontal: content follows the finger, so
         * panning inverts; traditional wheel-style does not. */
        if (natural) {
          out = -out;
        }
        if (out != 0) {
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
        int32_t out = ft6236_proc_wheel_delta(delta, param2, &data->rem_y);

        data->last_y = evt->value;
        /* Natural vertical: finger up scrolls down (content
         * follows the finger); traditional inverts. */
        if (!natural) {
          out = -out;
        }
        if (out != 0) {
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
   * final deltas of a lifting frame must still go out. Only in PTP mode
   * is sync cleared: the PTP report has already been sent from here,
   * and in a scroll->PTP transition frame the flush is deferred to the
   * next scroll-mode frame.
   */
  evt->type = ZMK_INPUT_EV_DUMMY;
  evt->code = 0;
  evt->value = 0;
  if (data->mode == FT6236_PROC_MODE_PTP) {
    evt->sync = false;
  }

  return ZMK_INPUT_PROC_CONTINUE;
}

static int ft6236_touchpad_proc_init(const struct device *dev) {
  struct ft6236_touchpad_proc_data *data = dev->data;

  data->slot = 0;
  data->mode = FT6236_PROC_MODE_IDLE;
  data->scroll_slot = 0;
  data->have_last_x = false;
  data->have_last_y = false;
  for (uint8_t i = 0; i < FT6236_PROC_MAX_CONTACTS; i++) {
    data->contacts[i].active = false;
    data->contacts[i].id = i;
    data->contacts[i].x = 0;
    data->contacts[i].y = 0;
  }

  if (!ft6236_touchpad_is_ready()) {
    LOG_WRN("FT6236 touchpad HID not ready at processor init");
  }

  return 0;
}

static const struct zmk_input_processor_driver_api ft6236_touchpad_proc_api = {
  .handle_event = ft6236_touchpad_proc_handle_event,
};

#define FT6236_TOUCHPAD_PROC_INIT(n)                                    \
  static struct ft6236_touchpad_proc_data ft6236_touchpad_proc_data_##n; \
  static const struct ft6236_touchpad_proc_config ft6236_touchpad_proc_config_##n = { \
    .natural = DT_INST_PROP(n, scroll_natural),                         \
  };                                                                    \
  DEVICE_DT_INST_DEFINE(n, ft6236_touchpad_proc_init, NULL,             \
                        &ft6236_touchpad_proc_data_##n,                 \
                        &ft6236_touchpad_proc_config_##n,               \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                        &ft6236_touchpad_proc_api);

DT_INST_FOREACH_STATUS_OKAY(FT6236_TOUCHPAD_PROC_INIT)
