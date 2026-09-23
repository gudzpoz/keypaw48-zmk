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
 * This driver is a gesture source, not a coordinate source: it does not report
 * touch coordinates at all (scrolling was removed -- the PAW3395 trackball
 * scrolls more smoothly). Instead it reads the contacts, classifies a gesture
 * from their geometry, and emits that gesture as an INPUT_EV_KEY whose code is
 * an FT6x36 gesture id. Those codes cross the split link as ordinary key events
 * and are mapped to ZMK behaviours on the central half, so gestures do not
 * depend on multitouch forwarding.
 *
 * A gesture is HELD: value 1 is reported when it is recognized and value 0 when
 * the contact that produced it lifts. A momentary behaviour such as &mkp LCLK
 * therefore behaves like a real button -- a one-finger press-and-drag is a
 * click-drag -- while a quick tap still reads as a click.
 *
 *   - one finger  -> FT6236_GESTURE_PRESS (a plain press), or a flick. The press
 *     is reported only after gesture-press-settle-ms, so a second finger landing
 *     first cancels it instead of emitting a stray click at the start of a
 *     two-finger gesture. The settle window doubles as the flick window: if the
 *     finger crosses gesture-swipe-distance inside it (and within the swipe
 *     timeout) the contact is a swipe instead, so a flick never clicks. A finger
 *     that dwells first and then moves is a press/drag, and a finger that lifts
 *     inside the window is a one-shot tap.
 *   - two fingers -> a pinch, a swipe, or (if they lift without either) a tap,
 *     told apart by whether the fingers moved relative to each other or
 *     together, or not at all.
 *
 * The controller's own GEST_ID register is deliberately not read: this panel
 * (FT6x36U) reports 0x00 there for every touch, so its gesture engine is
 * unusable. Reading both contacts is all that is needed to classify a gesture,
 * and unlike the register it is testable.
 */

#define DT_DRV_COMPAT focaltech_ft6236

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ft6236, CONFIG_FT6236_LOG_LEVEL);

#include <dt-bindings/zmk/ft6236.h>

#include "ft6236_regs.h"

/* Contacts the panel can report. Both are read (the second only to classify
 * two-finger gestures). */
#define FT6236_MAX_POINTS 2

/** One decoded contact, in raw panel units. */
struct ft6236_contact {
  uint16_t x;
  uint16_t y;
  bool touch; /* tip switch / pressed */
};

/* Gesture-session states. A session spans a single touch: it opens when the
 * panel first reports a contact and closes when every contact is lifted. At most
 * one gesture code is held at a time. */
enum ft6236_gesture_state {
  /** No contact down. */
  FT6236_IDLE,
  /** One finger down, the settle window is running, PRESS is not held yet. */
  FT6236_ONE_PENDING,
  /** One finger down, a flick was recognized; the swipe code is held. */
  FT6236_ONE_SWIPE_HELD,
  /** One finger down, PRESS is held. */
  FT6236_ONE_HELD,
  /** Two fingers down, classifying or holding a two-finger gesture. */
  FT6236_TWO,
  /** A two-finger pair ended but one finger is still down. */
  FT6236_POST_PAIR,
};

/** Gesture classifier state. */
struct ft6236_gesture_session {
  enum ft6236_gesture_state state;
  /** Contacts currently down (0..2), counted from the tip switches. */
  uint8_t down;
  /** Gesture code currently held, or FT6236_GESTURE_NONE. */
  uint8_t held;
  /** A pinch or two-finger swipe has already been emitted for this pair. */
  bool two_finger_reported;
  /** The pair has been cleanly measured at least once. */
  bool have_pair;
  /** Reference time (ms uptime) for the pair's swipe/tap windows. */
  uint32_t pair_start_ms;
  /* Single-finger reference: where and when the contact went down, so a flick
   * can be told from a press before the press is committed. */
  uint32_t press_start_ms;
  uint16_t press_start_x;
  uint16_t press_start_y;
  /* Two-finger reference, measured once both contacts are cleanly down. */
  int32_t start_separation2;
  int32_t start_mid_x;
  int32_t start_mid_y;
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
  uint32_t gesture_press_settle_ms;
  uint32_t gesture_tap_timeout_ms;
  uint32_t gesture_poll_ms;
};

/** FT6236 data. */
struct ft6236_data {
  /** Device pointer. */
  const struct device *dev;
  /** Work queue entry for a deferred read after an interrupt. */
  struct k_work work;
  /** Commits the single-finger press once the settle window elapses. */
  struct k_work_delayable settle_work;
  /** Re-reads the panel while a session is open, to unstick a held gesture. */
  struct k_work_delayable watchdog;
  /** Interrupt GPIO callback. */
  struct gpio_callback int_gpio_cb;
  /** Gesture classifier state. */
  struct ft6236_gesture_session gesture;
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

/* Read both contacts and the contact count. */
static int ft6236_read_contacts(const struct device *dev, struct ft6236_contact *c0,
                                struct ft6236_contact *c1, uint8_t *points_out) {
  const struct ft6236_config *config = dev->config;
  uint8_t points = 0;
  int r = i2c_reg_read_byte_dt(&config->bus, FT6236_REG_TD_STATUS, &points);

  if (r < 0) {
    return r;
  }

  points = FIELD_GET(FT6236_TOUCH_POINTS_MSK, points);
  if (points > FT6236_MAX_POINTS) {
    points = FT6236_MAX_POINTS;
  }

  if (points >= 1) {
    r = ft6236_read_contact(&config->bus, FT6236_REG_P1_XH, c0);
    if (r < 0) {
      return r;
    }
  }
  if (points >= 2) {
    r = ft6236_read_contact(&config->bus, FT6236_REG_P2_XH, c1);
    if (r < 0) {
      return r;
    }
  }

  *points_out = points;
  return 0;
}

/* Report one gesture edge. sync is deliberately clear: an unmapped code must
 * not make the central listener flush a mouse report. The mapped behaviour
 * (e.g. &mkp) sends its own report. */
static void ft6236_emit(const struct device *dev, uint8_t code, bool pressed) {
  LOG_DBG("gesture 0x%02x %s", code, pressed ? "down" : "up");
  input_report(dev, INPUT_EV_KEY, code, pressed ? 1 : 0, false, K_FOREVER);
}

/* Press `code`, releasing whatever was held first, so at most one gesture code
 * is ever held at a time. */
static void ft6236_hold(const struct device *dev, uint8_t code) {
  struct ft6236_gesture_session *s = &((struct ft6236_data *)dev->data)->gesture;

  if (s->held == code) {
    return;
  }
  if (s->held != FT6236_GESTURE_NONE) {
    ft6236_emit(dev, s->held, false);
  }
  s->held = code;
  ft6236_emit(dev, code, true);
}

/* Release the held gesture, if any. */
static void ft6236_release_held(const struct device *dev) {
  struct ft6236_gesture_session *s = &((struct ft6236_data *)dev->data)->gesture;

  if (s->held == FT6236_GESTURE_NONE) {
    return;
  }
  ft6236_emit(dev, s->held, false);
  s->held = FT6236_GESTURE_NONE;
}

/* A one-shot gesture: press immediately followed by release. */
static void ft6236_tap(const struct device *dev, uint8_t code) {
  ft6236_emit(dev, code, true);
  ft6236_emit(dev, code, false);
}

static void ft6236_arm_settle(const struct device *dev) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *data = dev->data;

  if (config->gesture_press_settle_ms == 0) {
    return;
  }
  k_work_reschedule(&data->settle_work, K_MSEC(config->gesture_press_settle_ms));
}

static void ft6236_cancel_settle(const struct device *dev) {
  struct ft6236_data *data = dev->data;

  k_work_cancel_delayable(&data->settle_work);
}

static void ft6236_arm_watchdog(const struct device *dev) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *data = dev->data;

  if (config->gesture_poll_ms == 0) {
    return;
  }
  k_work_reschedule(&data->watchdog, K_MSEC(config->gesture_poll_ms));
}

static void ft6236_cancel_watchdog(const struct device *dev) {
  struct ft6236_data *data = dev->data;

  k_work_cancel_delayable(&data->watchdog);
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
 * neither axis reached `threshold`. The caller passes the ids to use. The
 * dominant axis wins, so a diagonal drag is not reported as two gestures. */
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

/* A single-finger flick: the contact has moved past gesture-swipe-distance from
 * where it went down, within the swipe timeout. Called only while the press is
 * still pending, so a slow drag (which would have committed PRESS first) is
 * never read as a swipe. Returns the direction code or FT6236_GESTURE_NONE. */
static uint8_t ft6236_single_flick(const struct device *dev, const struct ft6236_contact *c0) {
  const struct ft6236_config *config = dev->config;
  const struct ft6236_gesture_session *s = &((struct ft6236_data *)dev->data)->gesture;

  if (config->gesture_swipe_timeout_ms != 0 &&
      (k_uptime_get_32() - s->press_start_ms) > config->gesture_swipe_timeout_ms) {
    return FT6236_GESTURE_NONE;
  }

  int32_t dx = (int32_t)c0->x - (int32_t)s->press_start_x;
  int32_t dy = (int32_t)c0->y - (int32_t)s->press_start_y;
  ft6236_orient_delta(&config->common, &dx, &dy);
  return ft6236_swipe_direction(dx, dy, config->gesture_swipe_distance, FT6236_GESTURE_UP,
                                FT6236_GESTURE_DOWN, FT6236_GESTURE_LEFT,
                                FT6236_GESTURE_RIGHT);
}

/* End a two-finger pair: release a held pinch/swipe, or, if the pair never
 * produced one and lifted soon enough, report a two-finger tap. */
static void ft6236_finish_pair(const struct device *dev) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *s_data = dev->data;
  struct ft6236_gesture_session *s = &s_data->gesture;

  if (s->held != FT6236_GESTURE_NONE) {
    /* A pinch or two-finger swipe was held; the lift is its release. */
    ft6236_release_held(dev);
    return;
  }

  if (s->two_finger_reported || !s->have_pair) {
    /* A gesture was already reported, or the pair never got a clean frame. */
    return;
  }

  if (config->gesture_tap_timeout_ms != 0 &&
      (k_uptime_get_32() - s->pair_start_ms) > config->gesture_tap_timeout_ms) {
    /* Held too long to be a tap. */
    return;
  }

  ft6236_tap(dev, FT6236_GESTURE_TWO_FINGER_TAP);
}

/* Classify a two-finger frame and hold the gesture it yields, if any. */
static void ft6236_classify_pair(const struct device *dev, const struct ft6236_contact *c0,
                                 const struct ft6236_contact *c1) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *data = dev->data;
  struct ft6236_gesture_session *s = &data->gesture;

  if (!c0->touch || !c1->touch) {
    /* One of the two is lifting, so its coordinates are not worth trusting. */
    return;
  }

  if (s->two_finger_reported) {
    /* A gesture is held (or was already reported) for this pair. */
    return;
  }

  int32_t dx = (int32_t)c1->x - (int32_t)c0->x;
  int32_t dy = (int32_t)c1->y - (int32_t)c0->y;
  int32_t separation2 = dx * dx + dy * dy;
  int32_t mid_x = ((int32_t)c0->x + (int32_t)c1->x) / 2;
  int32_t mid_y = ((int32_t)c0->y + (int32_t)c1->y) / 2;

  if (!s->have_pair) {
    /* Reference the separation and the midpoint as they were once the pair was
     * cleanly down; the swipe/tap windows are measured from pair_start_ms. */
    s->have_pair = true;
    s->start_separation2 = separation2;
    s->start_mid_x = mid_x;
    s->start_mid_y = mid_y;
    return;
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
    ft6236_hold(dev, (spread > 0) ? FT6236_GESTURE_ZOOM_IN : FT6236_GESTURE_ZOOM_OUT);
    return;
  }

  if (travel2 < swipe2 || travel2 <= spread_abs) {
    return;
  }

  if (config->gesture_swipe_timeout_ms != 0 &&
      (k_uptime_get_32() - s->pair_start_ms) > config->gesture_swipe_timeout_ms) {
    /* Too slow to be a swipe; mark the pair so it cannot become one later (and
     * stops being eligible for the two-finger tap). */
    s->two_finger_reported = true;
    return;
  }

  ft6236_orient_delta(&config->common, &mid_dx, &mid_dy);
  uint8_t gesture =
      ft6236_swipe_direction(mid_dx, mid_dy, config->gesture_swipe_distance,
                             FT6236_GESTURE_TWO_FINGER_UP, FT6236_GESTURE_TWO_FINGER_DOWN,
                             FT6236_GESTURE_TWO_FINGER_LEFT, FT6236_GESTURE_TWO_FINGER_RIGHT);
  if (gesture != FT6236_GESTURE_NONE) {
    s->two_finger_reported = true;
    ft6236_hold(dev, gesture);
  }
}

/* Advance the gesture state machine by one read of the panel. `settle_elapsed`
 * is true only when the caller is the settle timer or the poll -- a regular
 * interrupt frame must not commit the single-finger press early. */
static void ft6236_gesture_update(const struct device *dev, const struct ft6236_contact *c0,
                                  const struct ft6236_contact *c1, uint8_t points,
                                  bool settle_elapsed) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *data = dev->data;
  struct ft6236_gesture_session *s = &data->gesture;

  /* Count contacts that are actually down, so the panel's lift-up frame -- where
   * TD_STATUS can still report the point but EVENT says lift -- transitions the
   * state machine as a release. */
  uint8_t down = 0;
  if (points >= 1 && c0->touch) {
    down++;
  }
  if (points >= 2 && c1->touch) {
    down++;
  }

  uint8_t prev = s->down;

  LOG_DBG("points %u down %u, c0 (%u, %u) touch %d, c1 (%u, %u) touch %d", points, down, c0->x,
          c0->y, c0->touch, c1->x, c1->y, c1->touch);

  if (down == 0) {
    /* The session closes. */
    ft6236_cancel_settle(dev);
    if (s->state == FT6236_ONE_PENDING) {
      /* Lifted inside the settle window: honour the tap. */
      ft6236_tap(dev, FT6236_GESTURE_PRESS);
    } else if (s->state == FT6236_TWO) {
      ft6236_finish_pair(dev);
    }
    ft6236_release_held(dev);
    ft6236_cancel_watchdog(dev);
    s->state = FT6236_IDLE;
    s->down = 0;
    s->have_pair = false;
    s->two_finger_reported = false;
    return;
  }

  bool classify = false;

  if (prev == 0) {
    /* A session opens. */
    s->have_pair = false;
    s->two_finger_reported = false;
    s->pair_start_ms = k_uptime_get_32();
    if (down == 1) {
      s->state = FT6236_ONE_PENDING;
      s->press_start_ms = k_uptime_get_32();
      s->press_start_x = c0->x;
      s->press_start_y = c0->y;
      if (config->gesture_press_settle_ms == 0) {
        ft6236_hold(dev, FT6236_GESTURE_PRESS);
        s->state = FT6236_ONE_HELD;
      } else {
        ft6236_arm_settle(dev);
      }
    } else {
      s->state = FT6236_TWO;
      classify = true;
    }
  } else if (prev == 1 && down == 1) {
    if (s->state == FT6236_ONE_PENDING) {
      /* A flick wins over the press, but only until the press is committed:
       * the settle window is also the flick window. */
      uint8_t flick = ft6236_single_flick(dev, c0);
      if (flick != FT6236_GESTURE_NONE) {
        ft6236_cancel_settle(dev);
        ft6236_hold(dev, flick);
        s->state = FT6236_ONE_SWIPE_HELD;
      } else if (settle_elapsed) {
        ft6236_cancel_settle(dev);
        ft6236_hold(dev, FT6236_GESTURE_PRESS);
        s->state = FT6236_ONE_HELD;
      }
    }
  } else if (prev == 1 && down >= 2) {
    /* The second finger cancels any pending or held single-finger press. */
    ft6236_cancel_settle(dev);
    ft6236_release_held(dev);
    s->state = FT6236_TWO;
    s->have_pair = false;
    s->two_finger_reported = false;
    s->pair_start_ms = k_uptime_get_32();
    classify = true;
  } else if (prev >= 2 && down == 1) {
    /* The pair ended with one finger still down: never re-press. */
    ft6236_finish_pair(dev);
    s->state = FT6236_POST_PAIR;
    s->have_pair = false;
    s->two_finger_reported = false;
  } else if (prev >= 2 && down >= 2) {
    classify = (s->state == FT6236_TWO);
  }

  s->down = down;

  if (classify) {
    ft6236_classify_pair(dev, c0, c1);
  }

  /* Poll while a session is open, so a lift interrupt that was missed still
   * releases a held gesture instead of stranding a mouse button down. */
  ft6236_arm_watchdog(dev);
}

/* Read the panel and advance the state machine. `settle_elapsed` is forwarded
 * to ft6236_gesture_update() and is true only for the settle timer and poll. */
static void ft6236_process(const struct device *dev, bool settle_elapsed) {
  struct ft6236_data *data = dev->data;
  struct ft6236_contact c0 = {.x = 0, .y = 0, .touch = false};
  struct ft6236_contact c1 = {.x = 0, .y = 0, .touch = false};
  uint8_t points = 0;

  if (!data->identified) {
    data->identified = true;
    ft6236_identify(dev);
  }

  int r = ft6236_read_contacts(dev, &c0, &c1, &points);
  if (r < 0) {
    /* Leave the session as-is; a held gesture is released as soon as a later
     * read reports no contacts. Re-arm the poll even here, or a run of failures
     * would stop the retries and strand anything held. */
    LOG_DBG("panel read failed (%d)", r);
    if (data->gesture.state != FT6236_IDLE) {
      ft6236_arm_watchdog(dev);
    }
    return;
  }

  ft6236_gesture_update(dev, &c0, &c1, points, settle_elapsed);
}

static void ft6236_work_handler(struct k_work *work) {
  struct ft6236_data *data = CONTAINER_OF(work, struct ft6236_data, work);

  ft6236_process(data->dev, false);
}

static void ft6236_settle_handler(struct k_work *work) {
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  struct ft6236_data *data = CONTAINER_OF(dwork, struct ft6236_data, settle_work);

  ft6236_process(data->dev, true);
}

static void ft6236_watchdog_handler(struct k_work *work) {
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  struct ft6236_data *data = CONTAINER_OF(dwork, struct ft6236_data, watchdog);

  /* By the time the poll fires the settle window is long past, so any pending
   * press is a real one. */
  ft6236_process(data->dev, true);
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
  struct ft6236_gesture_session *s = &data->gesture;
  int r;

  if (!device_is_ready(config->bus.bus)) {
    LOG_ERR("I2C controller device not ready");
    return -ENODEV;
  }

  data->dev = dev;
  s->state = FT6236_IDLE;
  s->down = 0;
  s->held = FT6236_GESTURE_NONE;
  s->have_pair = false;
  s->two_finger_reported = false;
  s->pair_start_ms = 0;
  s->press_start_ms = 0;
  s->press_start_x = 0;
  s->press_start_y = 0;

  k_work_init(&data->work, ft6236_work_handler);
  k_work_init_delayable(&data->settle_work, ft6236_settle_handler);
  k_work_init_delayable(&data->watchdog, ft6236_watchdog_handler);

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
  struct ft6236_gesture_session *s = &data->gesture;
  int r;

  switch (action) {
  case PM_DEVICE_ACTION_SUSPEND: {
    r = ft6236_int_disable(config);
    if (r < 0) {
      return r;
    }
    k_work_cancel(&data->work);
    /* Release anything held before the chip loses its interrupt, so a gesture
     * cannot be stranded down across a sleep. */
    ft6236_cancel_settle(dev);
    ft6236_cancel_watchdog(dev);
    ft6236_release_held(dev);
    s->state = FT6236_IDLE;
    s->down = 0;
    s->have_pair = false;
    s->two_finger_reported = false;
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
      .gesture_press_settle_ms = DT_INST_PROP(index, gesture_press_settle_ms), \
      .gesture_tap_timeout_ms = DT_INST_PROP(index, gesture_tap_timeout_ms),   \
      .gesture_poll_ms = DT_INST_PROP(index, gesture_poll_ms),                 \
  };                                                                           \
  static struct ft6236_data ft6236_data_##index;                               \
  DEVICE_DT_INST_DEFINE(index, ft6236_init, PM_DEVICE_DT_INST_GET(index),      \
                        &ft6236_data_##index, &ft6236_config_##index,          \
                        POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(FT6236_INIT)
