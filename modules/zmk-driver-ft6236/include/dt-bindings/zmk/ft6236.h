/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * FT6236 gesture codes, used as the INPUT_EV_KEY codes the driver emits for a
 * classified gesture, so a gesture can be matched directly in devicetree, e.g.
 *
 *   codes = <FT6236_GESTURE_ZOOM_IN>;
 *
 * The one-finger and pinch values are the ones the controller's own GEST_ID
 * register would report (references/FT6XX6_Datasheet.md section 3.1.2), even
 * though this panel never populates that register: the driver classifies
 * gestures in firmware from the contact geometry and reuses those ids. The
 * two-finger swipe ids have no hardware equivalent and are ours, sitting in a
 * value range the datasheet leaves free.
 */

#pragma once

/** No gesture. */
#define FT6236_GESTURE_NONE 0x00

/* One finger: the datasheet's "Move" gestures. These also scroll, so they are
 * off by default in the keymap. */
#define FT6236_GESTURE_UP    0x10
#define FT6236_GESTURE_RIGHT 0x14
#define FT6236_GESTURE_DOWN  0x18
#define FT6236_GESTURE_LEFT  0x1C

/* Two fingers: pinch (datasheet values) and swipe (ours). */
#define FT6236_GESTURE_ZOOM_IN          0x48
#define FT6236_GESTURE_ZOOM_OUT         0x49
#define FT6236_GESTURE_TWO_FINGER_UP    0x50
#define FT6236_GESTURE_TWO_FINGER_DOWN  0x51
#define FT6236_GESTURE_TWO_FINGER_LEFT  0x52
#define FT6236_GESTURE_TWO_FINGER_RIGHT 0x53
