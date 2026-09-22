/*
 * Copyright (c) 2020,2023 NXP
 * Copyright (c) 2020 Mark Olsson <mark@markolsson.se>
 * Copyright (c) 2020 Teslabs Engineering S.L.
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FT6236 / FT6x36 capacitive touch panel driver.
 *
 * Models the upstream input_ft5336.c for coordinates: only the first contact is
 * reported, as a plain single-touch frame (INPUT_ABS_X / INPUT_ABS_Y /
 * INPUT_BTN_TOUCH) in **raw panel pixels**. There is no logical coordinate space
 * to keep in sync, so the central half's scroll parameters are plain pixels per
 * wheel notch. A second finger cancels that frame rather than adding a contact,
 * so a pinch is never mistaken for a scroll drag by the central half.
 *
 * Gestures are classified here in firmware, from the contact geometry, and
 * reported as one INPUT_EV_KEY per gesture carrying an FT6x36 gesture id as the
 * event code. One finger yields a swipe; two fingers yield a pinch or a swipe,
 * told apart by whether the fingers moved relative to each other or together.
 * Those codes cross the split link as ordinary key events and are mapped to ZMK
 * behaviours on the central half, so gestures do not depend on multitouch
 * forwarding at all.
 *
 * The controller's own GEST_ID register is deliberately not read: this panel
 * (FT6x36U) reports 0x00 there for every touch, so its gesture engine is
 * unusable. Reading both contacts is all that is needed to classify a swipe or
 * a pinch, and unlike the register it is testable.
 */

#define DT_DRV_COMPAT focaltech_ft6236

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ft6236, CONFIG_FT6236_LOG_LEVEL);

#include <dt-bindings/zmk/ft6236.h>

#include "ft6236_regs.h"

/* Contacts the panel can report. Both are read (the second only to classify
 * two-finger gestures); only the first is ever forwarded as a coordinate frame. */
#define FT6236_MAX_POINTS 2

/** One decoded contact, in raw panel units. */
struct ft6236_contact {
  uint16_t x;
  uint16_t y;
  bool touch; /* tip switch / pressed */
};

/* Gesture classifier state. A session spans a single touch: it opens when the
 * panel first reports a contact and closes when every contact is lifted. Within
 * a session a one-finger swipe is reported once, and each time the second finger
 * goes down the pair gets one gesture of its own (a pinch or a two-finger swipe).
 * Reporting once is what makes a gesture behave like a tap rather than a held
 * key. */
struct ft6236_gesture_session {
  bool active;
  /** Contacts reported by the previous read. */
  uint8_t last_points;
  /** Reference time (ms uptime) for the swipe timeout. */
  uint32_t start_ms;
  /** First contact position when the one-finger swipe was last (re)baselined. */
  uint16_t start_x;
  uint16_t start_y;
  /** A one-finger swipe has already been reported for this session. */
  bool swipe_reported;
  /* Two-finger state: measured from where the pair was once both contacts were
   * cleanly down. */
  bool have_pair;
  int32_t start_separation2;
  int32_t start_mid_x;
  int32_t start_mid_y;
  /** A pinch or a two-finger swipe has already been reported for this pair. */
  bool two_finger_reported;
};

/** FT6236 configuration (DT). */
struct ft6236_config {
  struct input_touchscreen_common_config common;
  /** I2C bus. */
  struct i2c_dt_spec bus;
  struct gpio_dt_spec reset_gpio;
  /** Interrupt GPIO information. */
  struct gpio_dt_spec int_gpio;
  /** Gesture classifier tunables, all in raw panel units / ms. */
  uint16_t gesture_swipe_distance;
  uint32_t gesture_swipe_timeout_ms;
  uint16_t gesture_pinch_distance;
};

/** FT6236 data. */
struct ft6236_data {
  /** Device pointer. */
  const struct device *dev;
  /** Work queue (for deferred read). */
  struct k_work work;
  /** Interrupt GPIO callback. */
  struct gpio_callback int_gpio_cb;
  /** Last emitted contact, used to detect changes. */
  struct ft6236_contact last;
  /** Gesture classifier state. */
  struct ft6236_gesture_session gesture;
  /** Timestamp (ms) of the last emitted frame, for throttle. */
  uint32_t last_emit_ms;
  /** Set once the chip has been identified (see ft6236_identify()). */
  bool identified;
};

INPUT_TOUCH_STRUCT_CHECK(struct ft6236_config);

static void ft6236_identify(const struct device *dev) {
  const struct ft6236_config *config = dev->config;
  uint8_t vendor = 0;
  int r = i2c_reg_read_byte_dt(&config->bus, FT6236_REG_FOCALTECH_ID, &vendor);

  if (r < 0 || vendor != FT6236_FOCALTECH_ID) {
    LOG_WRN("FocalTech vendor id not found (0x%02x, r %d)", vendor, r);
    return;
  }

  uint8_t chip_id = 0;
  r = i2c_reg_read_byte_dt(&config->bus, FT6236_REG_CHIP_ID, &chip_id);
  if (r < 0) {
    LOG_WRN("Could not read chip id (%d)", r);
  } else {
    LOG_INF("FT6x36 chip id 0x%02x", chip_id);
  }
}

/* Read one point starting at byte register `xh_reg`. */
static int ft6236_read_contact(const struct i2c_dt_spec *bus, uint8_t xh_reg,
                               struct ft6236_contact *c) {
  uint8_t coords[4U];
  int r = i2c_burst_read_dt(bus, xh_reg, coords, sizeof(coords));
  if (r < 0) {
    return r;
  }

  uint8_t event = FIELD_GET(FT6236_EVENT_MSK, coords[0] >> FT6236_EVENT_POS);

  c->x = ((coords[0] & FT6236_POSITION_H_MSK) << 8U) | coords[1];
  c->y = ((coords[2] & FT6236_POSITION_H_MSK) << 8U) | coords[3];
  /* tip switch: pressed unless the event says lift-up/none */
  c->touch = (event != FT6236_EVENT_LIFT_UP) && (event != FT6236_EVENT_NONE);

  return 0;
}

/* Report a one-shot gesture: press immediately followed by release, so it reads
 * as a tap rather than a held key. sync is deliberately clear on both events —
 * an unmapped code must not make the central listener flush a mouse report. */
static void ft6236_report_gesture(const struct device *dev, uint8_t gesture) {
  LOG_DBG("gesture 0x%02x", gesture);
  input_report(dev, INPUT_EV_KEY, gesture, 1, false, K_FOREVER);
  input_report(dev, INPUT_EV_KEY, gesture, 0, false, K_FOREVER);
}

/* Fold the panel's orientation into a measured delta, so gesture directions are
 * reported in the keyboard's frame rather than the panel's. */
static void ft6236_orient_delta(const struct input_touchscreen_common_config *cfg,
                                int32_t *dx, int32_t *dy) {
  if (cfg->inverted_x) {
    *dx = -*dx;
  }
  if (cfg->inverted_y) {
    *dy = -*dy;
  }
  if (cfg->swapped_x_y) {
    int32_t tmp = *dx;
    *dx = *dy;
    *dy = tmp;
  }
}

/* Turn a measured travel into a direction gesture, or FT6236_GESTURE_NONE when
 * neither axis reached `threshold`. The caller passes the ids to use, so the
 * same logic serves one-finger and two-finger swipes. The dominant axis wins, so
 * a diagonal drag is not reported as two gestures. */
static uint8_t ft6236_swipe_direction(int32_t dx, int32_t dy, int32_t threshold, uint8_t up,
                                      uint8_t down, uint8_t left, uint8_t right) {
  int32_t adx = (dx < 0) ? -dx : dx;
  int32_t ady = (dy < 0) ? -dy : dy;

  if (adx >= threshold && adx >= ady) {
    return (dx > 0) ? right : left;
  }
  if (ady >= threshold) {
    return (dy > 0) ? down : up;
  }

  return FT6236_GESTURE_NONE;
}

/* Classify the current contact geometry into a gesture, or FT6236_GESTURE_NONE.
 * One contact is a swipe. Two contacts are a pinch or a swipe, decided below by
 * whichever moved more. */
static uint8_t ft6236_classify_gesture(const struct device *dev,
                                       const struct ft6236_contact *c0,
                                       const struct ft6236_contact *c1,
                                       uint8_t points) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *data = dev->data;
  struct ft6236_gesture_session *s = &data->gesture;

  if (points == 0) {
    s->active = false;
    s->last_points = 0;
    return FT6236_GESTURE_NONE;
  }

  if (!s->active) {
    s->active = true;
    s->last_points = points;
    s->start_ms = k_uptime_get_32();
    s->start_x = c0->x;
    s->start_y = c0->y;
    s->swipe_reported = false;
    s->have_pair = false;
    s->two_finger_reported = false;
  } else if (points >= 2 && s->last_points == 1) {
    /* The pair just formed, so it gets its own measurement reference and its own
     * gesture, mirroring the 2 -> 1 case below. */
    s->have_pair = false;
    s->two_finger_reported = false;
  } else if (points == 1 && s->last_points >= 2) {
    /* The pair ended: a one-finger swipe starts from wherever the surviving
     * finger is now, not from wherever the pair happened to begin. */
    s->start_ms = k_uptime_get_32();
    s->start_x = c0->x;
    s->start_y = c0->y;
  }

  s->last_points = points;

  if (points >= 2) {
    if (!c0->touch || !c1->touch) {
      /* One of the two is lifting, so its coordinates are not worth trusting;
       * wait for a clean two-finger frame before measuring anything. */
      return FT6236_GESTURE_NONE;
    }

    if (s->two_finger_reported) {
      return FT6236_GESTURE_NONE;
    }

    int32_t dx = (int32_t)c1->x - (int32_t)c0->x;
    int32_t dy = (int32_t)c1->y - (int32_t)c0->y;
    int32_t separation2 = dx * dx + dy * dy;
    int32_t mid_x = ((int32_t)c0->x + (int32_t)c1->x) / 2;
    int32_t mid_y = ((int32_t)c0->y + (int32_t)c1->y) / 2;

    if (!s->have_pair) {
      /* Reference the separation and the midpoint as they were once the pair was
       * cleanly down; the swipe timeout is measured from here too. */
      s->have_pair = true;
      s->start_ms = k_uptime_get_32();
      s->start_separation2 = separation2;
      s->start_mid_x = mid_x;
      s->start_mid_y = mid_y;
      return FT6236_GESTURE_NONE;
    }

    int32_t spread = separation2 - s->start_separation2;
    int32_t mid_dx = mid_x - s->start_mid_x;
    int32_t mid_dy = mid_y - s->start_mid_y;
    int32_t travel2 = mid_dx * mid_dx + mid_dy * mid_dy;
    int32_t spread_abs = (spread < 0) ? -spread : spread;

    int32_t pinch2 = (int32_t)config->gesture_pinch_distance * config->gesture_pinch_distance;
    int32_t swipe2 = (int32_t)config->gesture_swipe_distance * config->gesture_swipe_distance;

    /* `spread` (change in squared separation) and `travel2` (squared movement of
     * the pair's midpoint) are both squared distances in panel units, so they
     * compare directly. Judging by whichever grew more is what stops a two-finger
     * swipe -- during which the fingers always drift a little apart -- from being
     * read as a pinch, and vice versa. */
    if (spread_abs >= pinch2 && spread_abs >= travel2) {
      s->two_finger_reported = true;
      return (spread > 0) ? FT6236_GESTURE_ZOOM_IN : FT6236_GESTURE_ZOOM_OUT;
    }

    if (travel2 < swipe2 || travel2 <= spread_abs) {
      return FT6236_GESTURE_NONE;
    }

    if (config->gesture_swipe_timeout_ms != 0 &&
        (k_uptime_get_32() - s->start_ms) > config->gesture_swipe_timeout_ms) {
      s->two_finger_reported = true;
      return FT6236_GESTURE_NONE;
    }

    ft6236_orient_delta(&config->common, &mid_dx, &mid_dy);
    uint8_t gesture =
        ft6236_swipe_direction(mid_dx, mid_dy, config->gesture_swipe_distance,
                               FT6236_GESTURE_TWO_FINGER_UP, FT6236_GESTURE_TWO_FINGER_DOWN,
                               FT6236_GESTURE_TWO_FINGER_LEFT, FT6236_GESTURE_TWO_FINGER_RIGHT);
    if (gesture != FT6236_GESTURE_NONE) {
      s->two_finger_reported = true;
    }
    return gesture;
  }

  /* Single contact: a swipe, but only a quick one. Anything slower is a scroll
   * drag, which the coordinates already drive. */
  if (s->swipe_reported) {
    return FT6236_GESTURE_NONE;
  }

  if (config->gesture_swipe_timeout_ms != 0 &&
      (k_uptime_get_32() - s->start_ms) > config->gesture_swipe_timeout_ms) {
    s->swipe_reported = true;
    return FT6236_GESTURE_NONE;
  }

  int32_t dx = (int32_t)c0->x - (int32_t)s->start_x;
  int32_t dy = (int32_t)c0->y - (int32_t)s->start_y;

  ft6236_orient_delta(&config->common, &dx, &dy);
  uint8_t gesture =
      ft6236_swipe_direction(dx, dy, config->gesture_swipe_distance, FT6236_GESTURE_UP,
                             FT6236_GESTURE_DOWN, FT6236_GESTURE_LEFT, FT6236_GESTURE_RIGHT);
  if (gesture != FT6236_GESTURE_NONE) {
    s->swipe_reported = true;
  }

  return gesture;
}

/* Report the scroll coordinate frame: the contact's position while `touch`, a
 * release otherwise. */
static void ft6236_emit_frame(const struct device *dev, const struct ft6236_contact *c,
                              bool touch) {
  struct ft6236_data *data = dev->data;

  /* Nothing to say if neither the contact state nor the position moved. */
  bool edge = touch != data->last.touch;
  bool moved = touch && (c->x != data->last.x || c->y != data->last.y);

  if (!edge && !moved) {
    return;
  }

  /* Throttle movement frames, but never press/lift transitions: after a
   * lift the chip goes idle and stops interrupting, so a throttled
   * release frame would be lost forever, stranding the contact as
   * pressed on the receiving side. */
  uint32_t now = k_uptime_get_32();
  if (!edge && (now - data->last_emit_ms) < (uint32_t)CONFIG_FT6236_REPORT_PERIOD_MS) {
    return;
  }
  data->last_emit_ms = now;

  if (touch) {
    LOG_DBG("contact (%u, %u) px", c->x, c->y);
    /* Raw panel pixels; the helper applies inverted-x / inverted-y /
     * swapped-x-y from the common config. */
    input_touchscreen_report_pos(dev, c->x, c->y, K_FOREVER);
    /* Last event of the frame carries sync. */
    input_report(dev, INPUT_EV_KEY, INPUT_BTN_TOUCH, 1, true, K_FOREVER);
  } else {
    LOG_DBG("lift");
    input_report(dev, INPUT_EV_KEY, INPUT_BTN_TOUCH, 0, true, K_FOREVER);
  }

  data->last = *c;
  data->last.touch = touch;
}

static int ft6236_process(const struct device *dev) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *data = dev->data;

  int r;
  uint8_t points = 0;
  struct ft6236_contact c0 = {.x = 0, .y = 0, .touch = false};
  struct ft6236_contact c1 = {.x = 0, .y = 0, .touch = false};

  if (!data->identified) {
    data->identified = true;
    ft6236_identify(dev);
  }

  r = i2c_reg_read_byte_dt(&config->bus, FT6236_REG_TD_STATUS, &points);
  if (r < 0) {
    return r;
  }

  points = FIELD_GET(FT6236_TOUCH_POINTS_MSK, points);
  if (points > FT6236_MAX_POINTS) {
    points = FT6236_MAX_POINTS;
  }

  if (points >= 1) {
    r = ft6236_read_contact(&config->bus, FT6236_REG_P1_XH, &c0);
    if (r < 0) {
      return r;
    }
  }
  if (points >= 2) {
    r = ft6236_read_contact(&config->bus, FT6236_REG_P2_XH, &c1);
    if (r < 0) {
      return r;
    }
  }

  LOG_DBG("points %u, c0 (%u, %u) touch %d, c1 (%u, %u) touch %d", points, c0.x, c0.y,
          c0.touch, c1.x, c1.y, c1.touch);

  uint8_t gesture = ft6236_classify_gesture(dev, &c0, &c1, points);
  if (gesture != FT6236_GESTURE_NONE) {
    ft6236_report_gesture(dev, gesture);
  }

  /* Only a lone contact drives scrolling: a second finger is a pinch, and
   * reporting a lift keeps the pinch from being read as a scroll drag. */
  ft6236_emit_frame(dev, &c0, points == 1 && c0.touch);

  return 0;
}

static void ft6236_work_handler(struct k_work *work) {
  struct ft6236_data *data = CONTAINER_OF(work, struct ft6236_data, work);

  ft6236_process(data->dev);
}

static void ft6236_isr_handler(const struct device *dev,
                               struct gpio_callback *cb, uint32_t pins) {
  struct ft6236_data *data = CONTAINER_OF(cb, struct ft6236_data, int_gpio_cb);

  k_work_submit(&data->work);
}

static int ft6236_int_enable(const struct ft6236_config *config) {
  return gpio_pin_interrupt_configure_dt(&config->int_gpio,
                                         GPIO_INT_EDGE_TO_ACTIVE);
}

static int ft6236_int_disable(const struct ft6236_config *config) {
  return gpio_pin_interrupt_configure_dt(&config->int_gpio, GPIO_INT_DISABLE);
}

static int ft6236_reset(const struct ft6236_config *config) {
  int r = gpio_pin_set_dt(&config->reset_gpio, 1);
  if (r < 0) {
    return r;
  }

  k_sleep(K_MSEC(5));

  r = gpio_pin_set_dt(&config->reset_gpio, 0);
  if (r < 0) {
    return r;
  }

  k_sleep(K_MSEC(200));
  return 0;
}

static int ft6236_init(const struct device *dev) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *data = dev->data;
  int r;

  if (!device_is_ready(config->bus.bus)) {
    LOG_ERR("I2C controller device not ready");
    return -ENODEV;
  }

  data->dev = dev;
  data->last.touch = false;
  data->gesture.active = false;
  data->last_emit_ms = 0;

  k_work_init(&data->work, ft6236_work_handler);

  if (config->reset_gpio.port != NULL) {
    /* Enable reset GPIO (asserted) and run the reset sequence. */
    r = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
    if (r < 0) {
      LOG_ERR("Could not enable reset GPIO");
      return r;
    }
    r = ft6236_reset(config);
    if (r < 0) {
      return r;
    }
  }

  if (!gpio_is_ready_dt(&config->int_gpio)) {
    LOG_ERR("Interrupt GPIO controller device not ready");
    return -ENODEV;
  }

  r = gpio_pin_configure_dt(&config->int_gpio, GPIO_INPUT);
  if (r < 0) {
    LOG_ERR("Could not configure interrupt GPIO pin");
    return r;
  }

  r = ft6236_int_enable(config);
  if (r < 0) {
    LOG_ERR("Could not configure interrupt GPIO interrupt.");
    return r;
  }

  gpio_init_callback(&data->int_gpio_cb, ft6236_isr_handler,
                     BIT(config->int_gpio.pin));
  r = gpio_add_callback(config->int_gpio.port, &data->int_gpio_cb);
  if (r < 0) {
    LOG_ERR("Could not set gpio callback");
    return r;
  }

  r = pm_device_runtime_enable(dev);
  if (r < 0 && r != -ENOTSUP) {
    LOG_ERR("Failed to enable runtime power management");
    return r;
  }

  return 0;
}

#ifdef CONFIG_PM_DEVICE
static int ft6236_pm_action(const struct device *dev,
                            enum pm_device_action action) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *data = dev->data;
  int r;

  switch (action) {
  case PM_DEVICE_ACTION_SUSPEND: {
    r = ft6236_int_disable(config);
    if (r < 0) {
      return r;
    }
    k_work_cancel(&data->work);
    /* Hibernate can only be left through a reset line; without one, drop
     * to monitor mode instead, where the I2C interface stays alive so
     * ft6236_init() can bring the chip back to Active mode. */
    uint8_t pmod = (config->reset_gpio.port != NULL) ? FT6236_PMOD_HIBERNATE
                                                     : FT6236_PMOD_MONITOR;
    r = i2c_reg_write_byte_dt(&config->bus, FT6236_REG_G_PMODE, pmod);
    if (r < 0) {
      return r;
    }
    break;
  }
  case PM_DEVICE_ACTION_RESUME:
    if (config->reset_gpio.port != NULL) {
      /* Toggle reset: the documented way out of hibernation. */
      r = ft6236_reset(config);
      if (r < 0) {
        return r;
      }
    } else {
      r = i2c_reg_write_byte_dt(&config->bus, FT6236_REG_G_PMODE,
                                FT6236_PMOD_ACTIVE);
      if (r < 0) {
        return r;
      }
    }
    r = ft6236_int_enable(config);
    if (r < 0) {
      return r;
    }
    break;
  default:
    return -ENOTSUP;
  }

  return 0;
}
#endif

#define FT6236_INIT(index)                                                     \
  PM_DEVICE_DT_INST_DEFINE(index, ft6236_pm_action);                           \
  static const struct ft6236_config ft6236_config_##index = {                  \
      .common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(index),                 \
      .bus = I2C_DT_SPEC_INST_GET(index),                                      \
      .reset_gpio = GPIO_DT_SPEC_INST_GET_OR(index, reset_gpios, {0}),         \
      .int_gpio = GPIO_DT_SPEC_INST_GET(index, int_gpios),                     \
      .gesture_swipe_distance = DT_INST_PROP(index, gesture_swipe_distance),   \
      .gesture_swipe_timeout_ms =                                              \
          DT_INST_PROP(index, gesture_swipe_timeout_ms),                       \
      .gesture_pinch_distance = DT_INST_PROP(index, gesture_pinch_distance),   \
  };                                                                           \
  static struct ft6236_data ft6236_data_##index;                               \
  DEVICE_DT_INST_DEFINE(index, ft6236_init, PM_DEVICE_DT_INST_GET(index),      \
                        &ft6236_data_##index, &ft6236_config_##index,          \
                        POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(FT6236_INIT)
