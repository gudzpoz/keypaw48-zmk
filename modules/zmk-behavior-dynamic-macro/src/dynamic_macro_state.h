/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Internal accessor for the recorder state, shared between the behavior and the
 * RGB overlay kind in this module. Not public API: the module publishes no
 * other module's contract.
 */

#pragma once

#include <stdbool.h>

/* True while &dm is recording a take into a scratch buffer. Written from the
 * behavior handler on the system workqueue; read from the RGB matrix's
 * low-priority workqueue by the overlay. */
bool zmk_dynamic_macro_is_recording(void);
