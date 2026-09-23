# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

# The driver consumes ZMK app headers such as <dt-bindings/zmk/ft6236.h>; make
# the application include dir visible here.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_FT6236 ft6236.c)
