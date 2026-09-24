# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO behavior_dynamic_macro.c)

# Must be built on BOTH halves: the shield's shared kp-rgb.dtsi lists this
# overlay on &kprgb, so the device has to exist wherever the list is expanded.
zephyr_library_sources_ifdef(CONFIG_ZMK_RGB_OVERLAY_DYNAMIC_MACRO rgb_overlay_dynamic_macro.c)

# So <drivers/behavior.h> and <zmk/...> resolve from this module's library.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
