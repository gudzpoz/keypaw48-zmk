/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * FT6236 / FT6x36 register map and bitfields.
 *
 * The FT6x36 family stores touch point 1 at registers 0x03..0x06 and touch
 * point 2 at 0x09..0x0C. Each point occupies XH, XL, YH, YL (4 bytes of usable
 * coordinate data); the weight/misc bytes between them are reserved on this
 * part.
 */

#ifndef ZMK_DRIVER_FT6236_REGS_H_
#define ZMK_DRIVER_FT6236_REGS_H_

#include <zephyr/sys/util.h>

/* ---- Register addresses ---- */

/* 0x00 DEVICE_MODE */
#define FT6236_REG_DEVICE_MODE 0x00U
/* 0x01 GEST_ID (on-chip gesture result; unused in 2-point mode) */
#define FT6236_REG_GEST_ID 0x01U
/* 0x02 TD_STATUS: number of active touch points */
#define FT6236_REG_TD_STATUS 0x02U
/* 0x03 TOUCH1_XH (start of point 1 coordinate block) */
#define FT6236_REG_P1_XH 0x03U
/* 0x09 TOUCH2_XH (start of point 2 coordinate block) */
#define FT6236_REG_P2_XH 0x09U
/* 0x80 TH_GROUP: touch threshold */
/* 0x88 PERIODACTIVE: active report rate */
/* 0xA3 CHIP_ID / CIPHER */
#define FT6236_REG_CHIP_ID 0xA3U
/* 0xA4 G_MODE: interrupt / polling mode */
#define FT6236_REG_G_MODE 0xA4U
/* 0xA5 G_PMODE: power mode */
#define FT6236_REG_G_PMODE 0xA5U
/* 0xA8 FOCALTECH_ID (0x11) */
#define FT6236_REG_FOCALTECH_ID 0xA8U

#define FT6236_FOCALTECH_ID 0x11U
#define FT6236_CHIP_ID_FAMILY 0x36U

/* ---- Bitfields ---- */

/* REG_TD_STATUS: touch point count in bits [3:0]. */
#define FT6236_TOUCH_POINTS_POS 0U
#define FT6236_TOUCH_POINTS_MSK 0x0FU

/* REG_Pn_XH: event flags in bits [7:6]. */
#define FT6236_EVENT_POS 6U
#define FT6236_EVENT_MSK 0x03U

#define FT6236_EVENT_PRESS_DOWN 0x00U
#define FT6236_EVENT_LIFT_UP 0x01U
#define FT6236_EVENT_CONTACT 0x02U
#define FT6236_EVENT_NONE 0x03U

/* REG_Pn_YH: touch id in bits [7:4]. */
#define FT6236_TOUCH_ID_POS 4U
#define FT6236_TOUCH_ID_MSK 0x0FU

#define FT6236_TOUCH_ID_INVALID 0x0FU

/* REG_Pn_XH / REG_Pn_YH: high byte of the 12-bit coordinate. */
#define FT6236_POSITION_H_MSK 0x0FU

/* REG_G_PMODE: power consume mode. */
#define FT6236_PMOD_HIBERNATE 0x03U

#endif /* ZMK_DRIVER_FT6236_REGS_H_ */
