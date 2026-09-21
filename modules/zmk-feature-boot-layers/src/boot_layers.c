/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Activate a configured set of keymap layers once at boot. Central-only: layer
 * state lives only where the keymap does, and no layer event crosses the split
 * link.
 */

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/keymap.h>

LOG_MODULE_REGISTER(keypaw_boot_layers, CONFIG_ZMK_LOG_LEVEL);

#define BOOT_LAYERS_NODE DT_INST(0, keypaw_boot_layers)

/* DT_PROP_BY_IDX token-pastes the index, so it only accepts a literal. Expand
 * the devicetree array into a C array once and loop over that instead. */
#define BOOT_LAYER_VALUE(node_id, prop, idx) DT_PROP_BY_IDX(node_id, prop, idx),

static const uint32_t boot_layers[] = {
    DT_FOREACH_PROP_ELEM(BOOT_LAYERS_NODE, layers, BOOT_LAYER_VALUE)};

static int boot_layers_init(void) {
    for (size_t i = 0; i < ARRAY_SIZE(boot_layers); i++) {
        uint32_t layer = boot_layers[i];

        if (layer >= ZMK_KEYMAP_LAYERS_LEN) {
            LOG_WRN("Ignoring boot layer %u: keymap has only %d layers", layer,
                    ZMK_KEYMAP_LAYERS_LEN);
            continue;
        }

        /* Not locked: fn1_layer uses &to (which deactivates non-locked layers),
         * so a locked boot layer could never be turned off. */
        int ret = zmk_keymap_layer_activate((zmk_keymap_layer_id_t)layer, false);
        if (ret < 0) {
            LOG_WRN("Failed to activate boot layer %u (%d)", layer, ret);
        }
    }

    return 0;
}

SYS_INIT(boot_layers_init, APPLICATION, CONFIG_KEYPAW_BOOT_LAYERS_INIT_PRIORITY);
