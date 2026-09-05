# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

# The input processor / HID transport consume ZMK app headers such as
# <drivers/input_processor.h>; make the application include dir visible here.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_FT6236 ft6236.c)

# The PTP (Digitizer 0x0D) transport + input processor only run on the half that owns
# USB (the split central). FT6236_TOUCHPAD is Kconfig-gated on the input-processor
# devicetree node + ZMK_USB, so this is automatically central-only; on the peripheral
# the symbol is off and these files are not compiled (HID_1 does not exist there).
zephyr_library_sources_ifdef(CONFIG_FT6236_TOUCHPAD touchpad_hid.c touchpad_gestures.c)
