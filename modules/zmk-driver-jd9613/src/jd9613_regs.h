/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Command register definitions for the JD9613 LTPS AMOLED controller.
 * Values verified against JD9613_DS_Preliminary_V0.02.
 */

#ifndef ZEPHYR_DRIVERS_DISPLAY_JD9613_REGS_H_
#define ZEPHYR_DRIVERS_DISPLAY_JD9613_REGS_H_

/* Command codes (sans read commands as we don't have a MISO pin) */
#define JD9613_CMD_SWRESET  0x01 /* Software reset */
#define JD9613_CMD_SLPIN    0x10 /* Enter sleep */
#define JD9613_CMD_SLPOUT   0x11 /* Exit sleep */
#define JD9613_CMD_PTLON    0x12 /* Partial mode on */
#define JD9613_CMD_NORON    0x13 /* Normal display mode on */
#define JD9613_CMD_ALLPOFF  0x22 /* All pixel off */
#define JD9613_CMD_ALLPON   0x23 /* All pixel on */
#define JD9613_CMD_GAMSET   0x26 /* Set gamma */
#define JD9613_CMD_DISPOFF  0x28 /* Display off */
#define JD9613_CMD_DISPON   0x29 /* Display on */
#define JD9613_CMD_CASET    0x2A /* Column address set */
#define JD9613_CMD_PASET    0x2B /* Page/row address set */
#define JD9613_CMD_RAMWR    0x2C /* Write GRAM data */
#define JD9613_CMD_PTLAR    0x30 /* Partial area */
#define JD9613_CMD_VPTLAR   0x31 /* Vertical partial area */
#define JD9613_CMD_TEOFF    0x34 /* Tearing effect line off */
#define JD9613_CMD_TEON     0x35 /* Tearing effect line on */
#define JD9613_CMD_MADCTL   0x36 /* Memory data access control */
#define JD9613_CMD_IDMOFF   0x38 /* Idle mode off */
#define JD9613_CMD_IDMON    0x39 /* Idle mode on */
#define JD9613_CMD_COLMOD   0x3A /* Pixel format */
#define JD9613_CMD_WRMEMC   0x3C /* Write memory continue */
#define JD9613_CMD_STESL    0x44 /* Set tear scan line */
#define JD9613_CMD_PWSTATE  0x4F /* Power status setting */
#define JD9613_CMD_WRDISBV  0x51 /* Write brightness */
#define JD9613_CMD_WRCTRLD  0x53 /* Display brightness on/off/dim */
#define JD9613_CMD_DSPI     0xC4 /* Display SPI interface config */

/* MADCTL bits */
#define JD9613_MADCTL_MY   0x80 /* Page address order */
#define JD9613_MADCTL_MX   0x40 /* Column address order */
#define JD9613_MADCTL_MV   0x20 /* Page/column selection */
/* Bit 4 is RESERVED */
#define JD9613_MADCTL_RGB  0x00 /* RGB color order */
#define JD9613_MADCTL_BGR  0x08 /* BGR color order */
/* Bit 2 is RESERVED */
#define JD9613_MADCTL_SS   0x02 /* Source driver scan direction (flip horizontal) */
#define JD9613_MADCTL_GS   0x01 /* Gate driver scan direction (flip vertical) */

/* COLMOD values (bits[6:4]=DPI, bits[2:0]=DBI) */
#define JD9613_COLMOD_RGB332 0x02 /* 8-bit RGB332 */
#define JD9613_COLMOD_MONO8  0x03 /* 8-bit Grayscale */
#define JD9613_COLMOD_RGB565 0x05 /* 16-bit RGB565 */
#define JD9613_COLMOD_RGB888 0x07 /* 24-bit RGB888 */

/* Time constants */
#define JD9613_RESET_DELAY_MS   120U /* tRPWI >= 10ms after reset release */
#define JD9613_RESET_PULSE_US   50U  /* tRESETL >= 10us */
#define JD9613_SLPOUT_DELAY_MS  120U

#endif /* ZEPHYR_DRIVERS_DISPLAY_JD9613_REGS_H_ */
