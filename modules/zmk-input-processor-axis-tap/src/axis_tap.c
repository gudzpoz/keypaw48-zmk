/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_axis_tap

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <drivers/input_processor.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>
#include <zmk/events/position_state_changed.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Cap the taps one event may emit; leftover travel stays in the accumulator
 * and is emitted by later events. Keeps a huge delta from blocking the input
 * thread on synchronous HID sends. */
#define AXIS_TAP_MAX_STEPS_PER_EVENT 4

/* Drop a half-completed step after this much inactivity, so re-holding the
 * layer much later does not fire an arrow from stale accumulation. */
#define AXIS_TAP_IDLE_RESET_MS 150

enum axis_tap_binding {
    AXIS_TAP_LEFT = 0,
    AXIS_TAP_RIGHT,
    AXIS_TAP_UP,
    AXIS_TAP_DOWN,
    AXIS_TAP_BINDINGS,
};

struct axis_tap_config {
    uint8_t index;
    int16_t x_threshold;
    int16_t y_threshold;
    const struct zmk_behavior_binding *bindings;
};

struct axis_tap_data {
    int32_t x;
    int32_t y;
    int64_t last_ms;
};

static inline int32_t axis_tap_abs(int32_t v) { return v < 0 ? -v : v; }

static void axis_tap_tap(const struct axis_tap_config *cfg,
                         const struct zmk_input_processor_state *state, uint8_t binding) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(
            state->input_device_index, cfg->index),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    zmk_behavior_invoke_binding(&cfg->bindings[binding], event, true);
    zmk_behavior_invoke_binding(&cfg->bindings[binding], event, false);
}

static int axis_tap_handle_event(const struct device *dev, struct input_event *event,
                                 uint32_t param1, uint32_t param2,
                                 struct zmk_input_processor_state *state) {
    const struct axis_tap_config *cfg = dev->config;
    struct axis_tap_data *data = dev->data;

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int64_t now = k_uptime_get();
    if (now - data->last_ms > AXIS_TAP_IDLE_RESET_MS) {
        data->x = 0;
        data->y = 0;
    }
    data->last_ms = now;

    if (event->code == INPUT_REL_X) {
        data->x += event->value;
    } else {
        data->y += event->value;
    }

    /* Consume the motion unconditionally: the listener swallows
     * ZMK_INPUT_PROC_STOP from a layer-override processor, so zeroing the
     * value is what keeps the mouse cursor still. */
    event->value = 0;

    const int32_t x_th = cfg->x_threshold;
    const int32_t y_th = cfg->y_threshold;
    bool tapped = false;

    for (int step = 0; step < AXIS_TAP_MAX_STEPS_PER_EVENT; step++) {
        const bool x_ready = axis_tap_abs(data->x) >= x_th;
        const bool y_ready = axis_tap_abs(data->y) >= y_th;

        if (!x_ready && !y_ready) {
            break;
        }

        /* Prefer the axis that has covered the larger fraction of its
         * threshold, so a diagonal does not alternate between axes. */
        const bool use_x = x_ready && (!y_ready || (int64_t)axis_tap_abs(data->x) * y_th >=
                                                      (int64_t)axis_tap_abs(data->y) * x_th);

        if (use_x) {
            if (data->x > 0) {
                axis_tap_tap(cfg, state, AXIS_TAP_RIGHT);
                data->x -= x_th;
            } else {
                axis_tap_tap(cfg, state, AXIS_TAP_LEFT);
                data->x += x_th;
            }
        } else {
            if (data->y > 0) {
                axis_tap_tap(cfg, state, AXIS_TAP_DOWN);
                data->y -= y_th;
            } else {
                axis_tap_tap(cfg, state, AXIS_TAP_UP);
                data->y += y_th;
            }
        }

        tapped = true;
    }

    return tapped ? ZMK_INPUT_PROC_STOP : ZMK_INPUT_PROC_CONTINUE;
}

static int axis_tap_init(const struct device *dev) { return 0; }

static struct zmk_input_processor_driver_api axis_tap_driver_api = {
    .handle_event = axis_tap_handle_event,
};

#define AXIS_TAP_INST(n)                                                                           \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, bindings) == AXIS_TAP_BINDINGS,                               \
                 "axis-tap needs exactly four bindings: left, right, up, down");                    \
    BUILD_ASSERT(DT_INST_PROP(n, x_threshold) > 0 && DT_INST_PROP(n, y_threshold) > 0,             \
                 "axis-tap thresholds must be positive");                                           \
    static const struct zmk_behavior_binding axis_tap_bindings_##n[] = {                            \
        LISTIFY(DT_INST_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(n))};  \
    static const struct axis_tap_config axis_tap_config_##n = {                                     \
        .index = n,                                                                                 \
        .x_threshold = DT_INST_PROP(n, x_threshold),                                                \
        .y_threshold = DT_INST_PROP(n, y_threshold),                                                \
        .bindings = axis_tap_bindings_##n,                                                          \
    };                                                                                              \
    static struct axis_tap_data axis_tap_data_##n = {};                                             \
    DEVICE_DT_INST_DEFINE(n, axis_tap_init, NULL, &axis_tap_data_##n, &axis_tap_config_##n,         \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &axis_tap_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AXIS_TAP_INST)
