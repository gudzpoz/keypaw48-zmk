/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_rel_throttle

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>

#include <drivers/input_processor.h>
#include <zmk/endpoints.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct rel_throttle_config {
    uint16_t ble_interval_ms;
    uint16_t usb_interval_ms;
};

struct rel_throttle_data {
    int64_t last_report_ms;
};

static inline uint16_t rel_throttle_interval(const struct rel_throttle_config *cfg) {
    return zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_BLE ? cfg->ble_interval_ms
                                                                     : cfg->usb_interval_ms;
}

static int rel_throttle_handle_event(const struct device *dev, struct input_event *event,
                                     uint32_t param1, uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    const struct rel_throttle_config *cfg = dev->config;
    struct rel_throttle_data *data = dev->data;

    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    /* Only relative motion and wheel are coalesced. Keys keep their own sync
     * so a button press is never held back behind the report interval. */
    if (event->type != INPUT_EV_REL || !event->sync) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint16_t interval_ms = rel_throttle_interval(cfg);
    if (interval_ms == 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int64_t now = k_uptime_get();
    if (now - data->last_report_ms < interval_ms) {
        /* Withhold the sync. input_listener.c keeps accumulating REL_X/REL_Y
         * and sends one report once we let a later event's sync through, so
         * the withheld delta is reported rather than lost. */
        event->sync = false;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    data->last_report_ms = now;
    return ZMK_INPUT_PROC_CONTINUE;
}

static int rel_throttle_init(const struct device *dev) { return 0; }

static struct zmk_input_processor_driver_api rel_throttle_driver_api = {
    .handle_event = rel_throttle_handle_event,
};

#define REL_THROTTLE_INST(n)                                                                       \
    BUILD_ASSERT(DT_INST_PROP(n, ble_interval_ms) >= 0 && DT_INST_PROP(n, usb_interval_ms) >= 0,   \
                 "rel-throttle intervals must not be negative");                                   \
    static const struct rel_throttle_config rel_throttle_config_##n = {                            \
        .ble_interval_ms = DT_INST_PROP(n, ble_interval_ms),                                       \
        .usb_interval_ms = DT_INST_PROP(n, usb_interval_ms),                                       \
    };                                                                                             \
    static struct rel_throttle_data rel_throttle_data_##n = {};                                     \
    DEVICE_DT_INST_DEFINE(n, rel_throttle_init, NULL, &rel_throttle_data_##n,                       \
                          &rel_throttle_config_##n, POST_KERNEL,                                    \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &rel_throttle_driver_api);

DT_INST_FOREACH_STATUS_OKAY(REL_THROTTLE_INST)
