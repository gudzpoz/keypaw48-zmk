/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Bad Apple playback widget. Compressed data extracted from
 * https://www.lexaloffle.com/bbs/?pid=40010
 */

#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "bad_apple_data.h"
#include "bad_apple.h"
#include "styling.h"

/* MSB-first bit reader over a byte range. */
typedef struct {
  const uint8_t *data;
  size_t len;
  size_t pos;
  uint32_t acc; /* bit accumulator, next bits in the low `nbits` bits */
  unsigned nbits;
} ba_bitreader;

/* Read `n` (1..16) bits into *out. Returns 1 on success, 0 if the stream
 * is exhausted (final short read of padding). */
static inline int ba_take(ba_bitreader *br, unsigned n, uint32_t *out) {
  while (br->nbits < n) {
    if (br->pos >= br->len) {
      return 0;
    }
    br->acc = (br->acc << 8) | br->data[br->pos++];
    br->nbits += 8;
  }
  br->nbits -= n;
  *out = (br->acc >> br->nbits) & ((1u << n) - 1u);
  return 1;
}

/* RLE-decode a window/run stream into out[0..out_max), capped.
 * Returns the number of values written. */
static inline size_t ba_rl_decode(const uint8_t *src, size_t len,
                                  unsigned window, unsigned run, uint8_t *out,
                                  size_t out_max) {
  ba_bitreader br = { src, len, 0, 0, 0 };
  size_t count = 0;
  int last = -1;

  while (count < out_max) {
    uint32_t v;

    if (!ba_take(&br, window, &v)) {
      break;
    }
    out[count++] = (uint8_t)v;
    if ((int)v == last) {
      uint32_t repeat = 0;

      (void)ba_take(&br, run, &repeat); /* 0 on padding */
      while (repeat-- > 0 && count < out_max) {
        out[count++] = (uint8_t)v;
      }
      last = -1;
    } else {
      last = (int)v;
    }
  }
  return count;
}

/* Decode the tile dictionary into `tiles` (32 x 8 bytes, 1bpp rows,
 * MSB = leftmost pixel). Returns the number of tiles decoded. */
static inline size_t
bad_apple_decode_dict(uint8_t tiles[BAD_APPLE_DICT_TILES][8]) {
  return ba_rl_decode(bad_apple_dict, BAD_APPLE_DICT_LEN, 8, 5,
            (uint8_t *)tiles, BAD_APPLE_DICT_TILES * 8u) / 8u;
}

/* Decode frame `frame` (0-based) into `tile_idx` (256 five-bit tile
 * indices; tile t goes at x = (t / 16) * 8, y = (t % 16) * 8).
 * Returns the number of tile indices decoded, 0 on bad frame number. */
static inline size_t
bad_apple_decode_frame(unsigned frame,
                       uint8_t tile_idx[BAD_APPLE_FRAME_TILES]) {
  if (frame >= BAD_APPLE_FRAME_COUNT) {
    return 0;
  }
  const uint16_t begin = bad_apple_frame_offs[frame];
  const uint16_t end = bad_apple_frame_offs[frame + 1u];

  return ba_rl_decode(&bad_apple_frames[begin],
                      (size_t)(end - begin), 5, 5,
                      tile_idx, BAD_APPLE_FRAME_TILES);
}

#define BAD_APPLE_TILE_ROWS (BAD_APPLE_HEIGHT / 8)
#define BAD_APPLE_TILE_COLS (BAD_APPLE_WIDTH / 8)

/* l8_runs[tile_pixel_row][tile] = the 8 L8 bytes that tile `tile` contributes
 * to a screen row equal to that pixel row: source bit j (MSB first)
 * becomes byte j, 0xFF when set (white), 0x00 otherwise. */
static uint8_t l8_runs[8][BAD_APPLE_DICT_TILES][8];

static void expand_dictionary(void) {
  uint8_t tiles[BAD_APPLE_DICT_TILES][8];

  bad_apple_decode_dict(tiles);
  for (uint32_t r = 0; r < 8; r++) {
    for (uint32_t tile = 0; tile < BAD_APPLE_DICT_TILES; tile++) {
      const uint8_t bits = tiles[tile][r];
      for (uint32_t j = 0; j < 8; j++) {
        l8_runs[r][tile][j] = (bits & (0x80 >> j)) ? 0xFF : 0x00;
      }
    }
  }
}

static void render_frame(struct zmk_widget_bad_apple *widget) {
  static uint8_t idx[BAD_APPLE_FRAME_TILES];
  lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(widget->canvas);
  uint8_t *buf = draw_buf->data;
  const uint32_t stride = draw_buf->header.stride;
  const size_t n = bad_apple_decode_frame(widget->frame, idx);

  if (n != BAD_APPLE_FRAME_TILES) {
    return;
  }

  for (uint32_t y = 0; y < BAD_APPLE_HEIGHT; y++) {
    const uint8_t (*runs)[8] = l8_runs[y % 8];
    uint8_t *row = &buf[y * stride];
    for (uint32_t x = 0; x < BAD_APPLE_TILE_COLS; x++) {
      /* Tile at (column x, pixel row y) is frame index (y / 8) + x * 16. */
      memcpy(&row[x * 8], runs[idx[(y / 8) + x * 16]], 8);
    }
  }

  lv_obj_invalidate(widget->canvas);
}

static void bad_apple_timer_cb(lv_timer_t *timer) {
  struct zmk_widget_bad_apple *widget = lv_timer_get_user_data(timer);

  render_frame(widget);
  widget->frame = (widget->frame + 1) % BAD_APPLE_FRAME_COUNT;
}

int zmk_widget_bad_apple_init(struct zmk_widget_bad_apple *widget, lv_obj_t *parent) {
  expand_dictionary();

  widget->obj = lv_obj_create(parent);
  lv_aux_flat_container(widget->obj);
  lv_obj_set_size(widget->obj, BAD_APPLE_WIDTH, BAD_APPLE_HEIGHT);
  widget->canvas = lv_canvas_create(widget->obj);
  lv_canvas_set_buffer(widget->canvas, widget->cbuf, BAD_APPLE_WIDTH,
                       BAD_APPLE_HEIGHT, LV_COLOR_FORMAT_L8);
  lv_obj_center(widget->canvas);

  widget->frame = 0;
  render_frame(widget);
  widget->timer = lv_timer_create(bad_apple_timer_cb, 100, widget);

  return 0;
}

lv_obj_t *zmk_widget_bad_apple_obj(struct zmk_widget_bad_apple *widget) { return widget->obj; }
