#pragma once

#include <stdint.h>
#include <lvgl.h>
#include <zephyr/kernel.h>

static void anim_update(const uint8_t *src, uint8_t *dst, uint8_t threshold, uint8_t opacity) {
  for (int i = 0; i < 16; i++) {
    uint8_t r = src[i * 4];
    uint8_t g = src[i * 4 + 1];
    uint8_t b = src[i * 4 + 2];
    uint8_t a = src[i * 4 + 3];
    if (r == g && g == b) {
      dst[i * 4] = 0;
      dst[i * 4 + 1] = 0;
      dst[i * 4 + 2] = 0;
      dst[i * 4 + 3] = r <= threshold ? a : opacity;
    }
  }
}

#define DEFINE_ANIMATION(name)                                          \
  struct name##_state {                                                 \
    lv_image_dsc_t dsc;                                                 \
    uint8_t buffer[DATA_LEN_##name];                                    \
  };                                                                    \
  static inline void name##_state_init(struct name##_state *s) {        \
    __ASSERT(name.header.cf == LV_COLOR_FORMAT_I4, "must be I4 image"); \
    memcpy(s->buffer, name.data, name.data_size);                       \
    s->dsc = name;                                                      \
    s->dsc.data = s->buffer;                                            \
  }                                                                     \
  static inline void name##_update(struct name##_state *s, uint8_t percent, \
                                   uint8_t opacity) {                   \
    int threshold = ((int)percent) * ANIM_STEPS_##name / 100;           \
    anim_update(name.data, s->buffer, threshold << 4, opacity);         \
  }
