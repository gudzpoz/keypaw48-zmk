# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

# So <zmk/keymap.h> resolves from this module's library.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_KEYPAW_BOOT_LAYERS boot_layers.c)
