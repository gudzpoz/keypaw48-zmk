/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Behavior to adjust the JD9613 screen brightness (relative inc/dec).
 *
 * The JD9613 controller exposes brightness through the standard Zephyr
 * display API (set_brightness -> WRDISBV register, 0-255). We track the
 * level in software as a percent (0-100) and scale it to the register so
 * that inc/dec steps are relative and intuitive.
 */

#define DT_DRV_COMPAT zmk_behavior_jd9613_brightness

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <drivers/behavior.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include <dt-bindings/zmk/display_brightness.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define DISPLAY_DEV DEVICE_DT_GET(DT_CHOSEN(zephyr_display))

/* Current brightness, tracked in percent (0-100). The driver initializes the
 * panel to WRDISBV = 0xff (100%), so we start there to stay in sync. */
static uint8_t brightness_pct = 100;

/* Whether the display is actually reachable on this half. The display lives on
 * the peripheral half; on the other half the chosen device exists in the
 * devicetree but has no hardware, so writes fail. We use this to avoid
 * spamming errors from the activity listener on the half without a panel. */
static bool display_controllable;

static int jd9613_brightness_apply(void);

#if IS_ENABLED(CONFIG_SETTINGS)

static void jd9613_brightness_save_work_handler(struct k_work *work) {
  settings_save_one("jd9613_brightness/state", &brightness_pct,
                    sizeof(brightness_pct));
}

static struct k_work_delayable jd9613_brightness_save_work;

static int jd9613_brightness_settings_load_cb(const char *name, size_t len,
                                              settings_read_cb read_cb,
                                              void *cb_arg) {
  const char *next;

  if (settings_name_steq(name, "state", &next) && !next) {
    if (len != sizeof(brightness_pct)) {
      return -EINVAL;
    }
    int rc = read_cb(cb_arg, &brightness_pct, sizeof(brightness_pct));
    if (rc >= 0) {
      /* A stale or corrupt setting could hold an out-of-range byte; clamp so
       * the percent -> register scaling below can never produce garbage. */
      brightness_pct = MIN(brightness_pct, 100U);
      rc = jd9613_brightness_apply();
    }
    return MIN(rc, 0);
  }
  return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(jd9613_brightness, "jd9613_brightness", NULL,
                               jd9613_brightness_settings_load_cb, NULL, NULL);

static int jd9613_brightness_settings_init(void) {
  k_work_init_delayable(&jd9613_brightness_save_work, jd9613_brightness_save_work_handler);
  return 0;
}

SYS_INIT(jd9613_brightness_settings_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

static int jd9613_brightness_apply(void) {
  if (!device_is_ready(DISPLAY_DEV)) {
    LOG_DBG("Display device not ready; brightness change deferred");
    return -ENODEV;
  }

  uint8_t reg = (uint8_t)(((uint32_t)brightness_pct * 255U) / 100U);
  int rc = display_set_brightness(DISPLAY_DEV, reg);
  if (rc < 0) {
    LOG_ERR("Failed to set display brightness: %d", rc);
    display_controllable = false;
    return rc;
  }

  display_controllable = true;
  LOG_DBG("Display brightness now %u%% (reg 0x%02x)", brightness_pct, reg);
  return 0;
}

static int behavior_jd9613_brightness_pressed(struct zmk_behavior_binding *binding,
                                              struct zmk_behavior_binding_event event) {
  switch (binding->param1) {
  case JD_BRT_INC_CMD:
    brightness_pct = MIN(brightness_pct + CONFIG_ZMK_JD9613_BRIGHTNESS_STEP, 100U);
    break;
  case JD_BRT_DEC_CMD:
    brightness_pct = (brightness_pct > CONFIG_ZMK_JD9613_BRIGHTNESS_STEP)
      ? brightness_pct - CONFIG_ZMK_JD9613_BRIGHTNESS_STEP
      : 0U;
    break;
  default:
    LOG_ERR("Unknown JD9613 brightness command: %d", binding->param1);
    return -ENOTSUP;
  }

  int rc = jd9613_brightness_apply();

#if IS_ENABLED(CONFIG_SETTINGS)
  k_work_reschedule(&jd9613_brightness_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#endif

  return rc;
}

static int behavior_jd9613_brightness_released(struct zmk_behavior_binding *binding,
                                               struct zmk_behavior_binding_event event) {
  return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata inc_dec_values[] = {
  {
    .display_name = "Increase Brightness",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = JD_BRT_INC_CMD,
  },
  {
    .display_name = "Decrease Brightness",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = JD_BRT_DEC_CMD,
  },
};

static const struct behavior_parameter_metadata_set inc_dec_set = {
  .param1_values = inc_dec_values,
  .param1_values_len = ARRAY_SIZE(inc_dec_values),
};

static const struct behavior_parameter_metadata_set sets[] = {inc_dec_set};

static const struct behavior_parameter_metadata metadata = {
  .sets_len = ARRAY_SIZE(sets),
  .sets = sets,
};

#endif /* IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) */

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE)

/* The driver defers real panel init (which sets WRDISBV = 0xff) to the first
 * unblank, i.e. the first time the display goes ACTIVE. Re-apply our tracked
 * level then (and on later wakes) so a persisted level actually sticks. */
static int jd9613_brightness_activity_listener(const zmk_event_t *eh) {
  struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

  if (ev == NULL) {
    return -ENOTSUP;
  }
  if (ev->state == ZMK_ACTIVITY_ACTIVE && display_controllable) {
    jd9613_brightness_apply();
  }
  return 0;
}

ZMK_LISTENER(jd9613_brightness, jd9613_brightness_activity_listener);
ZMK_SUBSCRIPTION(jd9613_brightness, zmk_activity_state_changed);

#endif /* IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE) */

static const struct behavior_driver_api behavior_jd9613_brightness_driver_api = {
  .binding_pressed = behavior_jd9613_brightness_pressed,
  .binding_released = behavior_jd9613_brightness_released,
  /* GLOBAL: when a key is pressed, ZMK runs the behavior on the central and
   * automatically forwards it to every peripheral (see behavior.c). Since
   * both halves have an OLED, this keeps the screen brightness in sync
   * across the split, exactly like the RGB matrix brightness. */
  .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
  .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_jd9613_brightness_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
