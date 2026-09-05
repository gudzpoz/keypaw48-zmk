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
 * Models the upstream input_ft5336.c but reports BOTH touch points as
 * Zephyr multitouch slots (INPUT_ABS_MT_SLOT). The driver scales the
 * raw panel coordinates into the PTP logical coordinate space and emits
 * rate-throttled frames; the central half turns these events into
 * Windows Precision Touchpad HID reports.
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
LOG_MODULE_REGISTER(ft6236, CONFIG_INPUT_LOG_LEVEL);

#include "ft6236_regs.h"

/* Throttled frame: cap contacts at the configured maximum. */
#define FT6236_MAX_POINTS CONFIG_FT6236_MAX_TOUCH_POINTS

/* Frame events: (slot, X, Y, tip switch) per current contact, plus a
 * synthesized (slot, tip switch = 0) release pair per contact that
 * disappeared since the last emitted frame. */
#define FT6236_MAX_FRAME_EVTS (FT6236_MAX_POINTS * 4 + FT6236_MAX_POINTS * 2)

/* A single (type, code, value) triple in a multitouch frame. */
struct ft6236_frame_evt {
  uint8_t type;
  uint16_t code;
  int32_t value;
};

/* One decoded contact. */
struct ft6236_point {
  uint8_t id; /* hardware touch id (or point index if invalid) */
  uint16_t x; /* scaled to 0..FT6236_TOUCHPAD logical max */
  uint16_t y;
  bool touch; /* tip switch / pressed */
};

/** FT6236 configuration (DT). */
struct ft6236_config {
  struct input_touchscreen_common_config common;
  /** I2C bus. */
  struct i2c_dt_spec bus;
  struct gpio_dt_spec reset_gpio;
  /** Interrupt GPIO information. */
  struct gpio_dt_spec int_gpio;
};

/** FT6236 data. */
struct ft6236_data {
  /** Device pointer. */
  const struct device *dev;
  /** Work queue (for deferred read). */
  struct k_work work;
  /** Interrupt GPIO callback. */
  struct gpio_callback int_gpio_cb;
  /** Last emitted contact state, used to detect changes. */
  struct ft6236_point last[FT6236_MAX_POINTS];
  uint8_t last_count;
  /** Timestamp (ms) of the last emitted frame, for throttle. */
  uint32_t last_emit_ms;
};

INPUT_TOUCH_STRUCT_CHECK(struct ft6236_config);

/* Scale a raw panel coordinate to the PTP logical space, applying
 * inverted / swapped orientation from the common config. */
static void ft6236_orient_and_scale(const struct input_touchscreen_common_config *cfg,
                                    uint16_t raw_x, uint16_t raw_y,
                                    uint16_t *out_x, uint16_t *out_y) {
  uint32_t lw = cfg->screen_width ? cfg->screen_width : 1;
  uint32_t lh = cfg->screen_height ? cfg->screen_height : 1;
  uint32_t logical = CONFIG_FT6236_TP_LOGICAL_MAX;

  uint32_t sx = (uint32_t)raw_x * logical / lw;
  uint32_t sy = (uint32_t)raw_y * logical / lh;

  if (cfg->inverted_x) {
    sx = logical - sx;
  }
  if (cfg->inverted_y) {
    sy = logical - sy;
  }

  if (cfg->swapped_x_y) {
    *out_x = (uint16_t)sy;
    *out_y = (uint16_t)sx;
  } else {
    *out_x = (uint16_t)sx;
    *out_y = (uint16_t)sy;
  }
}

/* Read and decode one point starting at byte register `xh_reg`. */
static int ft6236_read_point(const struct i2c_dt_spec *bus, uint8_t xh_reg,
                             uint8_t point_index, struct ft6236_point *pt) {
  uint8_t coords[4U];
  int r = i2c_burst_read_dt(bus, xh_reg, coords, sizeof(coords));
  if (r < 0) {
    return r;
  }

  uint8_t event = FIELD_GET(FT6236_EVENT_MSK, coords[0] >> FT6236_EVENT_POS);
  uint8_t touch_id = FIELD_GET(FT6236_TOUCH_ID_MSK, coords[2] >> FT6236_TOUCH_ID_POS);
  uint16_t raw_x = ((coords[0] & FT6236_POSITION_H_MSK) << 8U) | coords[1];
  uint16_t raw_y = ((coords[2] & FT6236_POSITION_H_MSK) << 8U) | coords[3];

  pt->id = (touch_id != FT6236_TOUCH_ID_INVALID) ? touch_id : point_index;
  /* tip switch: pressed unless the event says lift-up/none */
  pt->touch = (event != FT6236_EVENT_LIFT_UP) && (event != FT6236_EVENT_NONE);
  /* x/y scaled later in the caller once we know the config */
  pt->x = raw_x;
  pt->y = raw_y;
  return 0;
}

static bool ft6236_state_changed(const struct ft6236_data *data,
                                 const struct ft6236_point *cur, uint8_t count) {
  if (count != data->last_count) {
    return true;
  }
  for (uint8_t i = 0; i < count; i++) {
    const struct ft6236_point *l = &data->last[i];
    if (cur[i].id != l->id || cur[i].touch != l->touch ||
        cur[i].x != l->x || cur[i].y != l->y) {
      return true;
    }
  }
  return false;
}

static int ft6236_process(const struct device *dev) {
  const struct ft6236_config *config = dev->config;
  struct ft6236_data *data = dev->data;

  int r;
  uint8_t points;
  struct ft6236_point cur[FT6236_MAX_POINTS] = {0};

  r = i2c_reg_read_byte_dt(&config->bus, FT6236_REG_TD_STATUS, &points);
  if (r < 0) {
    return r;
  }

  points = FIELD_GET(FT6236_TOUCH_POINTS_MSK, points);
  if (points > FT6236_MAX_POINTS) {
    points = FT6236_MAX_POINTS;
  }

  for (uint8_t i = 0; i < points; i++) {
    uint8_t xh_reg = (i == 0) ? FT6236_REG_P1_XH : FT6236_REG_P2_XH;
    r = ft6236_read_point(&config->bus, xh_reg, i, &cur[i]);
    if (r < 0) {
      return r;
    }
    /* Orient + scale raw into logical space now that we have the config. */
    uint16_t ox, oy;
    ft6236_orient_and_scale(&config->common, cur[i].x, cur[i].y, &ox, &oy);
    cur[i].x = ox;
    cur[i].y = oy;
  }

  /* Only emit when something changed since the last read. */
  if (!ft6236_state_changed(data, cur, points)) {
    return 0;
  }

  /* Throttle movement frames, but never press/lift transitions: after a
   * lift the chip goes idle and stops interrupting, so a throttled
   * release frame would be lost forever, stranding the contact as
   * pressed on the receiving side. */
  bool edge = points != data->last_count;
  for (uint8_t i = 0; i < points && i < data->last_count; i++) {
    edge = edge || cur[i].touch != data->last[i].touch;
  }

  uint32_t now = k_uptime_get_32();
  if (!edge && (now - data->last_emit_ms) < (uint32_t)CONFIG_FT6236_REPORT_PERIOD_MS) {
    return 0;
  }
  data->last_emit_ms = now;

  for (uint8_t i = 0; i < points; i++) {
    LOG_DBG("point %u: id %u (%u, %u) touch %d", i, cur[i].id, cur[i].x, cur[i].y,
            cur[i].touch);
  }

  /* Build the frame as (type, code, value) triples; last carries sync. */
  struct ft6236_frame_evt evt[FT6236_MAX_FRAME_EVTS];
  uint8_t n = 0;

  for (uint8_t i = 0; i < points; i++) {
    evt[n++] = (struct ft6236_frame_evt){INPUT_EV_ABS, INPUT_ABS_MT_SLOT, cur[i].id};
    evt[n++] = (struct ft6236_frame_evt){INPUT_EV_ABS, INPUT_ABS_X, cur[i].x};
    evt[n++] = (struct ft6236_frame_evt){INPUT_EV_ABS, INPUT_ABS_Y, cur[i].y};
    evt[n++] = (struct ft6236_frame_evt){INPUT_EV_KEY, INPUT_BTN_TOUCH, cur[i].touch ? 1 : 0};
  }
  /* Lifted contacts: synthesize releases (tip switch 0) for slots that
   * disappeared since the last frame. */
  for (uint8_t i = 0; i < data->last_count; i++) {
    bool still_present = false;
    for (uint8_t j = 0; j < points; j++) {
      if (data->last[i].id == cur[j].id) {
        still_present = true;
        break;
      }
    }
    if (!still_present) {
      evt[n++] = (struct ft6236_frame_evt){INPUT_EV_ABS, INPUT_ABS_MT_SLOT, data->last[i].id};
      evt[n++] = (struct ft6236_frame_evt){INPUT_EV_KEY, INPUT_BTN_TOUCH, 0};
    }
  }

  for (uint8_t i = 0; i < n; i++) {
    bool sync = (i == n - 1);
    input_report(dev, evt[i].type, evt[i].code, evt[i].value, sync, K_FOREVER);
  }

  memcpy(data->last, cur, sizeof(cur));
  data->last_count = points;

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
  data->last_count = 0;
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

  /* Log the chip identity for sanity checking the wiring/register map. */
  uint8_t chip_id = 0;
  r = i2c_reg_read_byte_dt(&config->bus, FT6236_REG_CHIP_ID, &chip_id);
  if (r == 0) {
    LOG_INF("FT6x36 chip id 0x%02x (expected 0x%02x)", chip_id,
            FT6236_CHIP_ID_FAMILY);
  } else {
    LOG_WRN("Could not read chip id (0x%02x)", r);
  }

  /* Force Active mode: a previous suspend may have left the chip in
   * monitor/hibernate mode (e.g. after a wake from system deep sleep). */
  r = i2c_reg_write_byte_dt(&config->bus, FT6236_REG_G_PMODE,
                            FT6236_PMOD_ACTIVE);
  if (r < 0) {
    LOG_WRN("Could not set Active power mode (0x%02x)", r);
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

#define FT6236_INIT(index)                                              \
  PM_DEVICE_DT_INST_DEFINE(index, ft6236_pm_action);                    \
  static const struct ft6236_config ft6236_config_##index = {           \
    .common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(index),            \
    .bus = I2C_DT_SPEC_INST_GET(index),                                 \
    .reset_gpio = GPIO_DT_SPEC_INST_GET_OR(index, reset_gpios, {0}),    \
    .int_gpio = GPIO_DT_SPEC_INST_GET(index, int_gpios),                \
  };                                                                    \
  static struct ft6236_data ft6236_data_##index;                        \
  DEVICE_DT_INST_DEFINE(index, ft6236_init, PM_DEVICE_DT_INST_GET(index), \
                        &ft6236_data_##index, &ft6236_config_##index,   \
                        POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(FT6236_INIT)
