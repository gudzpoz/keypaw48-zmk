/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Shared hardware reset line sequencer.
 *
 * Several chips share one reset net (touch panel + display on the left,
 * motion sensor + display on the right), so no single driver may own it.
 * This drives the line once at boot, before any of those drivers
 * initialize: they all come up after the release by construction.
 *
 * See the Kconfig help for the init priority constraints.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(keypaw_shared_reset, CONFIG_GPIO_LOG_LEVEL);

#define SHARED_RESET_NODE DT_INST(0, keypaw_shared_reset)

static int shared_reset_init(void) {
  static const struct gpio_dt_spec reset =
      GPIO_DT_SPEC_GET(SHARED_RESET_NODE, reset_gpios);

  if (!gpio_is_ready_dt(&reset)) {
    LOG_ERR("Reset GPIO controller device not ready");
    return -ENODEV;
  }

  int hold_ms = DT_PROP(SHARED_RESET_NODE, reset_hold_ms);
  int wait_ms = DT_PROP(SHARED_RESET_NODE, release_wait_ms);

  /* GPIO_OUTPUT_ACTIVE drives the active level, so the polarity comes
   * from the devicetree flags and this stays level-agnostic. */
  int r = gpio_pin_configure_dt(&reset, GPIO_OUTPUT_ACTIVE);
  if (r < 0) {
    LOG_ERR("Could not configure reset GPIO (%d)", r);
    return r;
  }
  LOG_DBG("Shared reset asserted");

  k_msleep(hold_ms);

  r = gpio_pin_set_dt(&reset, 0);
  if (r < 0) {
    LOG_ERR("Could not release reset GPIO (%d)", r);
    return r;
  }

  if (wait_ms > 0) {
    k_msleep(wait_ms);
  }

  LOG_DBG("Shared reset released");
  return 0;
}

SYS_INIT(shared_reset_init, POST_KERNEL,
         CONFIG_KEYPAW_SHARED_RESET_INIT_PRIORITY);
