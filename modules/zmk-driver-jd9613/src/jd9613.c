/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Driver for the JD9613 LTPS AMOLED display controller, exposed over
 * 4-wire SPI (MOSI, SCK, CS, DC, RESET).
 */

#define DT_DRV_COMPAT jadard_jd9613

#include <stdint.h>
#include <string.h>

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "jd9613_regs.h"

LOG_MODULE_REGISTER(jd9613, CONFIG_DISPLAY_LOG_LEVEL);

struct jd9613_cfg {
  struct spi_dt_spec bus;
  struct gpio_dt_spec dc;
  struct gpio_dt_spec reset;
  uint16_t width;
  uint16_t height;
};

struct jd9613_data {
  enum display_pixel_format pixel_format;
  bool blanking_on: 1;
  bool rounder_added: 1;
};

/* These helper macros are undefined later. */
#define NARG(...) (sizeof((int[]){__VA_ARGS__})/sizeof(int))
#define DATA(...) NARG(__VA_ARGS__), __VA_ARGS__
/*
 * JD9613 initialization sequence.
 *
 * Source (not in the datasheet):
 * - https://github.com/realthunder/esphome-t_touchbar_amoled
 * - https://github.com/Xinyuan-LilyGO/T-Track/blob/main/examples/factory/JD9613.cpp
 */
static const uint8_t jd9613_init_seq[] = {
  0xfe, DATA(0x01),
  0xf7, DATA(0x96, 0x13, 0xa9),
  0x90, DATA(0x01),
  JD9613_CMD_RAMWR, DATA(0x19, 0x0b, 0x24, 0x1b, 0x1b, 0x1b, 0xaa, 0x50, 0x01, 0x16, 0x04, 0x04, 0x04, 0xd7),
  0x2d, DATA(0x66, 0x56, 0x55),
  0x2e, DATA(0x24, 0x04, 0x3f, 0x30, 0x30, 0xa8, 0xb8, 0xb8, 0x07),
  0x33, DATA(0x03, 0x03, 0x03, 0x19, 0x19, 0x19, 0x13, 0x13, 0x13, 0x1a, 0x1a, 0x1a),
  JD9613_CMD_SLPIN, DATA(0x0b, 0x08, 0x64, 0xae, 0x0b, 0x08, 0x64, 0xae, 0x00, 0x80, 0x00, 0x00, 0x01),
  JD9613_CMD_SLPOUT, DATA(0x01, 0x1e, 0x01, 0x1e, 0x00),
  0x03, DATA(0x93, 0x1c, 0x00, 0x01, 0x7e),
  0x19, DATA(0x00),
  JD9613_CMD_VPTLAR, DATA(0x1b, 0x00, 0x06, 0x05, 0x05, 0x05),
  JD9613_CMD_TEON, DATA(0x00, 0x80, 0x80, 0x00),
  JD9613_CMD_PTLON, DATA(0x1b),
  0x1a, DATA(0x01, 0x20, 0x00, 0x08, 0x01, 0x06, 0x06, 0x06),
  0x74, DATA(0xbd, 0x00, 0x01, 0x08, 0x01, 0xbb, 0x98),
  0x6c, DATA(0xdc, 0x08, 0x02, 0x01, 0x08, 0x01, 0x30, 0x08, 0x00),
  0x6d, DATA(0xdc, 0x08, 0x02, 0x01, 0x08, 0x02, 0x30, 0x08, 0x00),
  0x76, DATA(0xda, 0x00, 0x02, 0x20, 0x39, 0x80, 0x80, 0x50, 0x05),
  0x6e, DATA(0xdc, 0x00, 0x02, 0x01, 0x00, 0x02, 0x4f, 0x02, 0x00),
  0x6f, DATA(0xdc, 0x00, 0x02, 0x01, 0x00, 0x01, 0x4f, 0x02, 0x00),
  0x80, DATA(0xbd, 0x00, 0x01, 0x08, 0x01, 0xbb, 0x98),
  0x78, DATA(0xdc, 0x08, 0x02, 0x01, 0x08, 0x01, 0x30, 0x08, 0x00),
  0x79, DATA(0xdc, 0x08, 0x02, 0x01, 0x08, 0x02, 0x30, 0x08, 0x00),
  0x82, DATA(0xda, 0x40, 0x02, 0x20, 0x39, 0x00, 0x80, 0x50, 0x05),
  0x7a, DATA(0xdc, 0x00, 0x02, 0x01, 0x00, 0x02, 0x4f, 0x02, 0x00),
  0x7b, DATA(0xdc, 0x00, 0x02, 0x01, 0x00, 0x01, 0x4f, 0x02, 0x00),
  0x84, DATA(0x01, 0x00, 0x09, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19),
  0x85, DATA(0x19, 0x19, 0x19, 0x03, 0x02, 0x08, 0x19, 0x19, 0x19, 0x19),
  0x20, DATA(0x20, 0x00, 0x08, 0x00, 0x02, 0x00, 0x40, 0x00, 0x10, 0x00, 0x04, 0x00),
  0x1e, DATA(0x40, 0x00, 0x10, 0x00, 0x04, 0x00, 0x20, 0x00, 0x08, 0x00, 0x02, 0x00),
  0x24, DATA(0x20, 0x00, 0x08, 0x00, 0x02, 0x00, 0x40, 0x00, 0x10, 0x00, 0x04, 0x00),
  JD9613_CMD_ALLPOFF, DATA(0x40, 0x00, 0x10, 0x00, 0x04, 0x00, 0x20, 0x00, 0x08, 0x00, 0x02, 0x00),
  JD9613_CMD_NORON, DATA(0x63, 0x52, 0x41),
  0x14, DATA(0x36, 0x25, 0x14),
  0x15, DATA(0x63, 0x52, 0x41),
  0x16, DATA(0x36, 0x25, 0x14),
  0x1d, DATA(0x10, 0x00, 0x00),
  JD9613_CMD_CASET, DATA(0x0d, 0x07),
  0x27, DATA(0x00, 0x01, 0x02, 0x03, 0x04, 0x05),
  JD9613_CMD_DISPOFF, DATA(0x00, 0x01, 0x02, 0x03, 0x04, 0x05),
  JD9613_CMD_GAMSET, DATA(0x01, 0x01),
  0x86, DATA(0x01, 0x01),
  0xfe, DATA(0x02),
  0x16, DATA(0x81, 0x43, 0x23, 0x1e, 0x03),
  0xfe, DATA(0x03),
  0x60, DATA(0x01),
  0x61, DATA(0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x0d, 0x26, 0x5a, 0x80, 0x80, 0x95, 0xf8, 0x3b, 0x75),
  0x62, DATA(0x21, 0x22, 0x32, 0x43, 0x44, 0xd7, 0x0a, 0x59, 0xa1, 0xe1, 0x52, 0xb7, 0x11, 0x64, 0xb1),
  0x63, DATA(0x54, 0x55, 0x66, 0x06, 0xfb, 0x3f, 0x81, 0xc6, 0x06, 0x45, 0x83),
  0x64, DATA(0x00, 0x00, 0x11, 0x11, 0x21, 0x00, 0x23, 0x6a, 0xf8, 0x63, 0x67, 0x70, 0xa5, 0xdc, 0x02),
  0x65, DATA(0x22, 0x22, 0x32, 0x43, 0x44, 0x24, 0x44, 0x82, 0xc1, 0xf8, 0x61, 0xbf, 0x13, 0x62, 0xad),
  0x66, DATA(0x54, 0x55, 0x65, 0x06, 0xf5, 0x37, 0x76, 0xb8, 0xf5, 0x31, 0x6c),
  0x67, DATA(0x00, 0x10, 0x22, 0x22, 0x22, 0x00, 0x37, 0xa4, 0x7e, 0x22, 0x25, 0x2c, 0x4c, 0x72, 0x9a),
  0x68, DATA(0x22, 0x33, 0x43, 0x44, 0x55, 0xc1, 0xe5, 0x2d, 0x6f, 0xaf, 0x23, 0x8f, 0xf3, 0x50, 0xa6),
  0x69, DATA(0x65, 0x66, 0x77, 0x07, 0xfd, 0x4e, 0x9c, 0xed, 0x39, 0x86, 0xd3),
  0xfe, DATA(0x05),
  0x61, DATA(0x00, 0x31, 0x44, 0x54, 0x55, 0x00, 0x92, 0xb5, 0x88, 0x19, 0x90, 0xe8, 0x3e, 0x71, 0xa5),
  0x62, DATA(0x55, 0x66, 0x76, 0x77, 0x88, 0xce, 0xf2, 0x32, 0x6e, 0xc4, 0x34, 0x8b, 0xd9, 0x2a, 0x7d),
  0x63, DATA(0x98, 0x99, 0xaa, 0x0a, 0xdc, 0x2e, 0x7d, 0xc3, 0x0d, 0x5b, 0x9e),
  0x64, DATA(0x00, 0x31, 0x44, 0x54, 0x55, 0x00, 0xa2, 0xe5, 0xcd, 0x5c, 0x94, 0xcf, 0x09, 0x4a, 0x72),
  0x65, DATA(0x55, 0x65, 0x66, 0x77, 0x87, 0x9c, 0xc2, 0xff, 0x36, 0x6a, 0xec, 0x45, 0x91, 0xd8, 0x20),
  0x66, DATA(0x88, 0x98, 0x99, 0x0a, 0x68, 0xb0, 0xfb, 0x43, 0x8c, 0xd5, 0x0e),
  0x67, DATA(0x00, 0x42, 0x55, 0x55, 0x55, 0x00, 0xcb, 0x62, 0xc5, 0x09, 0x44, 0x72, 0xa9, 0xd6, 0xfd),
  0x68, DATA(0x66, 0x66, 0x77, 0x87, 0x98, 0x21, 0x45, 0x96, 0xed, 0x29, 0x90, 0xee, 0x4b, 0xb1, 0x13),
  0x69, DATA(0x99, 0xaa, 0xba, 0x0b, 0x6a, 0xb8, 0x0d, 0x62, 0xb8, 0x0e, 0x54),
  0xfe, DATA(0x07),
  0x3e, DATA(0x00),
  0x42, DATA(0x03, 0x10),
  0x4a, DATA(0x31),
  0x5c, DATA(0x01),
  JD9613_CMD_WRMEMC, DATA(0x07, 0x00, 0x24, 0x04, 0x3f, 0xe2),
  JD9613_CMD_STESL, DATA(0x03, 0x40, 0x3f, 0x02),
  JD9613_CMD_PTLON, DATA(0xaa, 0xaa, 0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0, 0xf8),
  JD9613_CMD_SLPOUT, DATA(0xaa, 0xaa, 0xaa, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8),
  JD9613_CMD_SLPIN, DATA(0xaa, 0xaa, 0xaa, 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58),
  0x14, DATA(0x03, 0x1f, 0x3f, 0x5f, 0x7f, 0x9f, 0xbf, 0xdf, 0x03, 0x1f, 0x3f, 0x5f, 0x7f, 0x9f, 0xbf, 0xdf),
  0x18, DATA(0x70, 0x1a, 0x22, 0xbb, 0xaa, 0xff, 0x24, 0x71, 0x0f, 0x01, 0x00, 0x03),
  0xfe, DATA(0x00),
  JD9613_CMD_COLMOD, DATA(0x55),
  JD9613_CMD_DSPI, DATA(0x80),
  JD9613_CMD_CASET, DATA(0x00, 0x00, 0x00, 0x7d),
  JD9613_CMD_PASET, DATA(0x00, 0x00, 0x01, 0x25),
  JD9613_CMD_TEON, DATA(0x00),
  JD9613_CMD_WRCTRLD, DATA(0x28),
  JD9613_CMD_WRDISBV, DATA(0xff),
  JD9613_CMD_SLPOUT, 0x80, /* delay */
  JD9613_CMD_DISPON, 0x80, /* delay */

  0x00,
};
#undef NARG
#undef DATA

static inline int jd9613_write_cmd(const struct jd9613_cfg *cfg, uint8_t cmd,
                                   const uint8_t *data, size_t len) {
  struct spi_buf cmd_buf = {.buf = &cmd, .len = sizeof(cmd)};
  struct spi_buf_set cmd_set = {.buffers = &cmd_buf, .count = 1};
  struct spi_buf data_buf;
  struct spi_buf_set data_set;

  /* DC low = command */
  gpio_pin_set_dt(&cfg->dc, 0);
  int ret = spi_write_dt(&cfg->bus, &cmd_set);
  if (ret < 0) {
    LOG_ERR("command write failed: %d", ret);
    return -EIO;
  }

  /* DC high = data */
  gpio_pin_set_dt(&cfg->dc, 1);

  if (len > 0 && data != NULL) {
    data_buf.buf = (uint8_t *)data;
    data_buf.len = len;
    data_set.buffers = &data_buf;
    data_set.count = 1;
    ret = spi_write_dt(&cfg->bus, &data_set);
    if (ret < 0) {
      LOG_ERR("data write failed: %d", ret);
      return -EIO;
    }
  }

  return 0;
}

static inline int jd9613_write_data(const struct jd9613_cfg *cfg,
                                    const uint8_t *data, size_t len) {
  struct spi_buf buf = {.buf = (uint8_t *)data, .len = len};
  struct spi_buf_set buf_set = {.buffers = &buf, .count = 1};

  /* DC high = data */
  gpio_pin_set_dt(&cfg->dc, 1);
  if (spi_write_dt(&cfg->bus, &buf_set) < 0) {
    return -EIO;
  }

  return 0;
}

static void jd9613_hw_reset(const struct jd9613_cfg *cfg) {
  /* RESX is active-low; reset-gpios should be GPIO_ACTIVE_LOW in DT */
  gpio_pin_set_dt(&cfg->reset, 1);
  k_sleep(K_USEC(JD9613_RESET_PULSE_US));
  gpio_pin_set_dt(&cfg->reset, 0);
  k_sleep(K_MSEC(JD9613_RESET_DELAY_MS));
}

static int jd9613_set_window(const struct jd9613_cfg *cfg, uint16_t x0, uint16_t y0,
                             uint16_t x1, uint16_t y1) {
  uint8_t col_addr[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
  uint8_t row_addr[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
  int ret;

  if ((x0 & 1) || (y0 & 1) || !(x1 & 1) || !(y1 & 1)) {
    LOG_ERR("coords must be even: %d,%d %d,%d", x0, y0, x1, y1);
    return -EINVAL;
  }

  /* Column Address Set */
  ret = jd9613_write_cmd(cfg, JD9613_CMD_CASET, col_addr, sizeof(col_addr));
  if (ret < 0) {
    return ret;
  }

  /* Page/Row Address Set */
  ret = jd9613_write_cmd(cfg, JD9613_CMD_PASET, row_addr, sizeof(row_addr));
  if (ret < 0) {
    return ret;
  }

  /* Write to GRAM */
  ret = jd9613_write_cmd(cfg, JD9613_CMD_RAMWR, NULL, 0);
  if (ret < 0) {
    return ret;
  }

  return 0;
}

static void jd9613_lv_rounder_cb(lv_event_t *e) {
  lv_area_t *area = lv_event_get_param(e);
  /* JD9613 requires regions to be of even coords, see jd9613_set_window */
  area->x1 &= ~1;
  area->y1 &= ~1;
  area->x2 |= 1;
  area->y2 |= 1;
}

static int jd9613_blanking_on(const struct device *dev) {
  struct jd9613_data *data = dev->data;
  const struct jd9613_cfg *cfg = dev->config;
  int ret;

  data->blanking_on = true;
  ret = jd9613_write_cmd(cfg, JD9613_CMD_SLPIN, NULL, 0);
  if (ret < 0) {
    return ret;
  }

  k_sleep(K_MSEC(JD9613_SLPOUT_DELAY_MS));
  return 0;
}

static int jd9613_blanking_off(const struct device *dev) {
  struct jd9613_data *data = dev->data;
  const struct jd9613_cfg *cfg = dev->config;
  int ret;

  ret = jd9613_write_cmd(cfg, JD9613_CMD_SLPOUT, NULL, 0);
  if (ret < 0) {
    return ret;
  }

  if (!data->rounder_added) {
    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL) {
      LOG_ERR("lvgl not ready");
    } else {
      data->rounder_added = true;
      lv_display_add_event_cb(disp, jd9613_lv_rounder_cb, LV_EVENT_INVALIDATE_AREA, disp);
    }
  }

  k_sleep(K_MSEC(JD9613_SLPOUT_DELAY_MS));
  data->blanking_on = false;
  return 0;
}

static int jd9613_write(const struct device *dev, const uint16_t x, const uint16_t y,
                        const struct display_buffer_descriptor *desc, const void *buf) {
  const struct jd9613_cfg *cfg = dev->config;
  struct jd9613_data *data = dev->data;
  const uint8_t *write_data = buf;
  size_t len = desc->buf_size;
  int ret;

  LOG_INF("w %dx%d+%dx%d(%d:%d:%s)", (int)desc->width, (int)desc->height,
          (int)x, (int)y, (int)len, (int)desc->pitch,
          desc->frame_incomplete ? "cont"
                                 : "end");

  if (data->blanking_on) {
    return -EPERM;
  }

  if (buf == NULL) {
    LOG_WRN("Buffer is not available");
    return -EINVAL;
  }

  if ((x + desc->width) > cfg->width ||
      (y + desc->height) > cfg->height) {
    LOG_ERR("Write out of bounds");
    return -EINVAL;
  }

  ret = jd9613_set_window(cfg, x, y, x + desc->width - 1, y + desc->height - 1);
  if (ret < 0) {
    return ret;
  }

  /* Push the pixel data. The driver expects the data with DC high. */
  ret = jd9613_write_data(cfg, write_data, len);
  if (ret < 0) {
    return ret;
  }

  return 0;
}

static void jd9613_get_capabilities(const struct device *dev,
                                    struct display_capabilities *caps) {
  const struct jd9613_cfg *cfg = dev->config;
  struct jd9613_data *data = dev->data;

  memset(caps, 0, sizeof(struct display_capabilities));
  caps->x_resolution = cfg->width;
  caps->y_resolution = cfg->height;
  caps->supported_pixel_formats =
    PIXEL_FORMAT_RGB_888 | PIXEL_FORMAT_RGB_565 | PIXEL_FORMAT_BGR_565;
  caps->current_pixel_format = data->pixel_format;
  caps->screen_info = SCREEN_INFO_X_ALIGNMENT_WIDTH;
}

static int jd9613_set_brightness(const struct device *dev,
                                 const uint8_t brightness) {
  const struct jd9613_cfg *cfg = dev->config;
  return jd9613_write_cmd(cfg, JD9613_CMD_WRDISBV, &brightness, 1);
}

static int jd9613_set_pixel_format(const struct device *dev,
                                   const enum display_pixel_format pf) {
  struct jd9613_data *data = dev->data;
  const struct jd9613_cfg *cfg = dev->config;
  uint8_t colmod;
  uint8_t madctl;
  int ret;

  switch (pf) {
  case PIXEL_FORMAT_RGB_565:
    madctl = JD9613_MADCTL_RGB;
    colmod = JD9613_COLMOD_RGB565;
    break;
  case PIXEL_FORMAT_BGR_565:
    madctl = JD9613_MADCTL_BGR;
    colmod = JD9613_COLMOD_RGB565;
    break;
  case PIXEL_FORMAT_RGB_888:
    madctl = JD9613_MADCTL_RGB;
    colmod = JD9613_COLMOD_RGB888;
    break;
  default:
    return -ENOTSUP;
  }

  ret = jd9613_write_cmd(cfg, JD9613_CMD_COLMOD, &colmod, 1);
  if (ret < 0) {
    return ret;
  }
  ret = jd9613_write_cmd(cfg, JD9613_CMD_MADCTL, &madctl, 1);
  if (ret < 0) {
    return ret;
  }

  data->pixel_format = pf;
  return 0;
}

static int jd9613_set_rotation(const struct device *dev,
                               enum display_orientation orientation) {
  const struct jd9613_data *data = dev->data;
  const struct jd9613_cfg *cfg = dev->config;
  uint8_t madctl = JD9613_MADCTL_RGB;
  int ret;

  switch (orientation) {
  case DISPLAY_ORIENTATION_NORMAL:
    madctl = JD9613_MADCTL_RGB;
    break;
  case DISPLAY_ORIENTATION_ROTATED_90:
    madctl = JD9613_MADCTL_MX | JD9613_MADCTL_MV | JD9613_MADCTL_RGB;
    break;
  case DISPLAY_ORIENTATION_ROTATED_180:
    madctl = JD9613_MADCTL_MX | JD9613_MADCTL_MY | JD9613_MADCTL_RGB;
    break;
  case DISPLAY_ORIENTATION_ROTATED_270:
    madctl = JD9613_MADCTL_MV | JD9613_MADCTL_MY | JD9613_MADCTL_RGB;
    break;
  default:
    return -ENOTSUP;
  }
  if (data->pixel_format == PIXEL_FORMAT_BGR_565) {
    madctl |= JD9613_MADCTL_BGR;
  }

  ret = jd9613_write_cmd(cfg, JD9613_CMD_MADCTL, &madctl, 1);
  if (ret < 0) {
    return ret;
  }

  return 0;
}

static uint8_t LOGO[] = {
  0b11000011,
  0b11000110,
  0b11001100,
  0b11111000,
  0b11111000,
  0b11001100,
  0b11000110,
  0b11000011,
};
static int jd9613_logo(const struct device *dev) {
  const struct jd9613_cfg *cfg = dev->config;
  const struct jd9613_data *data = dev->data;

  const uint16_t a = 8;
  const uint16_t zoom = 8;
  const uint16_t px = a * 8;
  uint16_t x0 = (cfg->width / 2 - px / 2) & ~1;
  uint16_t y0 = (cfg->height / 2 - px / 2) & ~1;

  int ret = jd9613_set_window(cfg, x0, y0, x0 + px - 1, y0 + px - 1);
  if (ret != 0) {
    return ret;
  }

  uint8_t zeros[] = {
    0,0,0, 0,0,0, 0,0,0, 0,0,0,
    0,0,0, 0,0,0, 0,0,0, 0,0,0,
  };
  uint8_t ones[] = {
    0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,
  };
  int len = data->pixel_format == PIXEL_FORMAT_RGB_888 ? 3 : 2;
  for (int y = 0; y < a; y++) {
    for (int n = 0; n < zoom; n++) {
      for (int x = 0; x < a; x++) {
        uint8_t *pixel = (uint8_t *) ((LOGO[y] & (1 << (7 - x))) ? ones : zeros);
        ret = jd9613_write_cmd(cfg, JD9613_CMD_WRMEMC, pixel, len * zoom);
        if (ret != 0) {
          return ret;
        }
      }
    }
  }

  return 0;
}

static int jd9613_controller_init(const struct device *dev) {
  const struct jd9613_cfg *cfg = dev->config;
  int ret;

  jd9613_hw_reset(cfg);

  const uint8_t *addr = jd9613_init_seq;
  uint8_t cmd, len;
  while ((cmd = *addr++) != 0) {
    len = *addr++;
    ret = jd9613_write_cmd(cfg, cmd, addr, len & 0x7F);
    if (ret < 0) {
      return ret;
    }
    addr += (len & 0x7F);
    if (len & 0x80) {
      k_sleep(K_MSEC(JD9613_SLPOUT_DELAY_MS));
    }
  }

  return 0;
}

static int jd9613_init(const struct device *dev)
{
  const struct jd9613_cfg *cfg = dev->config;
  struct jd9613_data *data = dev->data;

  if (!spi_is_ready_dt(&cfg->bus)) {
    LOG_ERR("SPI device not ready");
    return -ENODEV;
  }

  if (gpio_pin_configure_dt(&cfg->dc, GPIO_OUTPUT_INACTIVE) < 0 ||
      gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE) < 0) {
    return -EIO;
  }

  data->pixel_format = PIXEL_FORMAT_RGB_888;
  data->blanking_on = false;
  data->rounder_added = false;

  int ret = jd9613_controller_init(dev);
  if (ret != 0) {
    return ret;
  }

  ret = jd9613_set_pixel_format(dev, data->pixel_format);
  if (ret != 0) {
    return ret;
  }

  ret = jd9613_logo(dev);
  if (ret != 0) {
    return ret;
  }
  return 0;
}
static const struct display_driver_api jd9613_driver_api = {
  .blanking_on = jd9613_blanking_on,
  .blanking_off = jd9613_blanking_off,
  .write = jd9613_write,
  .get_capabilities = jd9613_get_capabilities,
  .set_brightness = jd9613_set_brightness,
  .set_pixel_format = jd9613_set_pixel_format,
  .set_orientation = jd9613_set_rotation,
};

#define JD9613_DEFINE(inst)                                             \
  static struct jd9613_data jd9613_data_##inst;                         \
  static const struct jd9613_cfg jd9613_cfg_##inst = {                  \
    .bus = SPI_DT_SPEC_INST_GET(inst,                                   \
                                SPI_OP_MODE_MASTER | SPI_WORD_SET(8), 0), \
    .dc = GPIO_DT_SPEC_INST_GET(inst, dc_gpios),                        \
    .reset = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),                  \
    .width = DT_INST_PROP(inst, width),                                 \
    .height = DT_INST_PROP(inst, height),                               \
  };                                                                    \
  DEVICE_DT_INST_DEFINE(inst, jd9613_init, NULL,                        \
                        &jd9613_data_##inst, &jd9613_cfg_##inst,        \
                        POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,      \
                        &jd9613_driver_api);

DT_INST_FOREACH_STATUS_OKAY(JD9613_DEFINE)
