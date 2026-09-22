# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

# The input processor consumes ZMK app headers such as
# <drivers/input_processor.h>; make the application include dir visible here.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_FT6236 ft6236.c)

# The scroll input processor is Kconfig-gated on its devicetree node, which only
# the split central declares, so this is automatically central-only: on the
# peripheral the symbol is off and the file is not compiled.
zephyr_library_sources_ifdef(CONFIG_FT6236_SCROLL ft6236_scroll.c)
