/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * FT6236 gesture codes, used as the INPUT_EV_KEY codes the driver emits for a
 * classified gesture, so a gesture can be matched directly in devicetree, e.g.
 *
 *   codes = <FT6236_GESTURE_ZOOM_IN>;
 *
 * A gesture is held from the moment it is recognized until the contact that
 * produced it lifts, so a value-1 event is a press and the matching value-0
 * event, emitted on lift, is the release. That is what lets a gesture drive a
 * momentary behaviour such as &mkp LCLK as a real button hold rather than a tap.
 *
 * The pinch values are the ones the controller's own GEST_ID register would
 * report (references/FT6XX6_Datasheet.md section 3.1.2), even though this panel
 * never populates that register: the driver classifies gestures in firmware
 * from the contact geometry and reuses those ids. The two-finger swipe values,
 * and the press / two-finger tap values, have no hardware equivalent and are
 * ours, sitting in a value range the datasheet leaves free.
 */

#pragma once

/** No gesture. */
#define FT6236_GESTURE_NONE 0x00

/* One finger, held from first contact until lift: a plain press. Bound to a
 * mouse button (LCLK) it behaves like a physical button, so a drag while held
 * is a click-drag. The driver only reports it after gesture-press-settle-ms, so
 * a second finger arriving quickly cancels it instead of clicking. */
#define FT6236_GESTURE_PRESS 0x60

/* One finger, flicked: ours. The controller's own "Move" gestures (datasheet
 * values) are reused, but they are recognized as flicks, not as slow drags: the
 * finger has to cross gesture-swipe-distance inside the settle window, before
 * FT6236_GESTURE_PRESS is committed. A finger that dwells first and then moves
 * is a press/drag, so a flick can never emit a stray click. */
#define FT6236_GESTURE_UP    0x10
#define FT6236_GESTURE_RIGHT 0x14
#define FT6236_GESTURE_DOWN  0x18
#define FT6236_GESTURE_LEFT  0x1C

/* Two fingers, tapped and released without a pinch or swipe: ours. */
#define FT6236_GESTURE_TWO_FINGER_TAP 0x54

/* Two fingers: pinch (datasheet values) and swipe (ours). */
#define FT6236_GESTURE_ZOOM_IN          0x48
#define FT6236_GESTURE_ZOOM_OUT         0x49
#define FT6236_GESTURE_TWO_FINGER_UP    0x50
#define FT6236_GESTURE_TWO_FINGER_DOWN  0x51
#define FT6236_GESTURE_TWO_FINGER_LEFT  0x52
#define FT6236_GESTURE_TWO_FINGER_RIGHT 0x53
