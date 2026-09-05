/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Public API for the FT6236 Windows Precision Touchpad (PTP) transport.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of simultaneous contacts the PTP interface reports. */
#define FT6236_TOUCHPAD_MAX_CONTACTS 2

/** A single contact in a touchpad report. */
struct ft6236_touchpad_contact {
  /** Tip switch / pressed. */
  bool active;
  /** Contact identifier (unique per finger). */
  uint8_t id;
  /** Logical X (0..FT6236_TP_LOGICAL_MAX). */
  uint16_t x;
  /** Logical Y (0..FT6236_TP_LOGICAL_MAX). */
  uint16_t y;
};

/** A full touchpad report. */
struct ft6236_touchpad_report {
  /**
   * All FT6236_TOUCHPAD_MAX_CONTACTS entries must be filled: inactive
   * slots carry their last known coordinates and id (the PTP spec
   * requires final frames to report lifted contacts at their last
   * position), with @ref active clearing the tip switch.
   */
  struct ft6236_touchpad_contact contacts[FT6236_TOUCHPAD_MAX_CONTACTS];
  /** Number of active contacts (tip switch set). */
  uint8_t contact_count;
  /** Physical button (e.g. clickpad). */
  bool button;
};

/**
 * Send a touchpad report to the host over the PTP HID interface.
 *
 * @return 0 on success, negative errno otherwise (e.g. -ENODEV if the
 *         interface is not ready).
 */
int ft6236_touchpad_send_report(const struct ft6236_touchpad_report *report);

/** @return true once the HID_1 interface has been registered. */
bool ft6236_touchpad_is_ready(void);

/**
 * Whether the host currently accepts touch reports: the PTP Surface Switch
 * is on and the host did not request mouse emulation. When false, touch
 * events should be suppressed until it turns true again.
 *
 * @return true if touch reporting is allowed.
 */
bool ft6236_touchpad_surface_enabled(void);

#ifdef __cplusplus
}
#endif
