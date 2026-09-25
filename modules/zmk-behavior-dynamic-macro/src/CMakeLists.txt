# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO behavior_dynamic_macro.c)

# Built on both halves: no extra dependency and zero cost if no DTS is defined.
zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_MATRIX condition_dynamic_macro.c)

# So <drivers/behavior.h> and <zmk/...> resolve from this module's library.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
