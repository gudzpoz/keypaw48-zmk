/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * FT6x36 gesture ids, used as the INPUT_EV_KEY codes the ft6236 driver emits
 * for a classified gesture, so a gesture can be matched directly in
 * devicetree, e.g.
 *
 *   codes = <FT6236_GESTURE_ZOOM_IN>;
 *
 * The values are the ones the controller's own GEST_ID register would report
 * (references/FT6XX6_Datasheet.md section 3.1.2), even though this panel never
 * populates that register: the driver classifies gestures in firmware from the
 * contact geometry and reuses these ids as its codes.
 */

#pragma once

#define FT6236_GESTURE_NONE     0x00
#define FT6236_GESTURE_UP       0x10
#define FT6236_GESTURE_RIGHT    0x14
#define FT6236_GESTURE_DOWN     0x18
#define FT6236_GESTURE_LEFT     0x1C
#define FT6236_GESTURE_ZOOM_IN  0x48
#define FT6236_GESTURE_ZOOM_OUT 0x49
