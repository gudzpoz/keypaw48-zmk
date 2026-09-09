# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

# The effect renders via the core module's public API (<zmk/rgb_matrix.h>) and so
# needs the application (and core module) include dirs visible.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZMK_RGB_EFFECT_EXAMPLE effect.c)
