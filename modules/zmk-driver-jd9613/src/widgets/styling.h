#pragma once

#include <lvgl.h>

#define COLOR_INACTIVE lv_color_hex(0x773333)
#define COLOR_ACTIVE lv_color_hex(0x00FFFF)

static inline void lv_aux_flat_container(lv_obj_t *obj) {
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static inline void lv_aux_flex_dir(lv_obj_t *obj, lv_flex_align_t align) {
  lv_aux_flat_container(obj);
  lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_layout(obj, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(obj, align, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static inline void lv_aux_flex_left(lv_obj_t *obj) {
  lv_aux_flex_dir(obj, LV_FLEX_ALIGN_START);
}

static inline void lv_aux_flex_right(lv_obj_t *obj) {
  lv_aux_flex_dir(obj, LV_FLEX_ALIGN_END);
}
