/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef DT_BINDINGS_ZMK_DISPLAY_BRIGHTNESS_H_
#define DT_BINDINGS_ZMK_DISPLAY_BRIGHTNESS_H_

/* Command codes for the zmk,behavior-jd9613-brightness behavior. */
#define JD_BRT_INC_CMD 0
#define JD_BRT_DEC_CMD 1

/* Keymap bindings: &jd_brt JD_BRT_INC / &jd_brt JD_BRT_DEC
 * (single-cell macros, binding-cells = 1). */
#define JD_BRT_INC JD_BRT_INC_CMD
#define JD_BRT_DEC JD_BRT_DEC_CMD

#endif /* DT_BINDINGS_ZMK_DISPLAY_BRIGHTNESS_H_ */
