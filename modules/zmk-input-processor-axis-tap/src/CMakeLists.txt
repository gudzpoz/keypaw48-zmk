# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

# Consumes <drivers/input_processor.h>, <zmk/behavior.h> and
# <zmk/virtual_key_position.h> from the application include dir, which is not
# inherited by module libraries automatically.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_AXIS_TAP axis_tap.c)
