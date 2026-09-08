/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "icons/icons.h"
#include "animation.h"

#if IS_ENABLED(CONFIG_ZMK_WIDGET_ANIMATION_MODE_ON_KEYPRESS)
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#endif

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

#if IS_ENABLED(CONFIG_ZMK_WIDGET_ANIMATION_MODE_ON_KEYPRESS)

/* lv_animimg has no "is running" or "stop" API, and lv_anim_start() copies the
 * widget's animation template into LVGL's running list, so the running
 * animation has to be looked up to tell whether a loop is still in flight. */
static bool animation_is_playing(lv_obj_t *obj) {
  lv_anim_t *tpl = lv_animimg_get_anim(obj);
  return lv_anim_get(tpl->var, tpl->exec_cb) != NULL;
}

/* A completed loop leaves its last frame on screen; snap back to the first. */
static void animation_completed_cb(lv_anim_t *anim) {
  const void **frames = lv_animimg_get_src(anim->var);
  lv_image_set_src(anim->var, frames[0]);
}

struct animation_trigger_state {
  bool pressed;
};

static struct animation_trigger_state
animation_trigger_get_state(const zmk_event_t *eh) {
  const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
  return (struct animation_trigger_state){.pressed = ev != NULL && ev->state};
}

static void animation_trigger_cb(struct animation_trigger_state state) {
  if (!state.pressed) {
    return;
  }

  struct zmk_widget_animation *widget;
  SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
    if (!animation_is_playing(widget->obj)) {
      lv_animimg_start(widget->obj);
    }
  }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_animation_trigger, struct animation_trigger_state,
                            animation_trigger_cb, animation_trigger_get_state)
ZMK_SUBSCRIPTION(widget_animation_trigger, zmk_position_state_changed);

#endif // CONFIG_ZMK_WIDGET_ANIMATION_MODE_ON_KEYPRESS

int zmk_widget_animation_init(struct zmk_widget_animation *widget, lv_obj_t *parent) {
  widget->obj = lv_animimg_create(parent);
#ifdef ZMK_WIDGET_ANIMATION_ICON
  lv_animimg_set_src(widget->obj, (const void **)ZMK_WIDGET_ANIMATION_ICON,
                     ARRAY_SIZE(ZMK_WIDGET_ANIMATION_ICON));
  lv_animimg_set_duration(widget->obj, CONFIG_ZMK_WIDGET_ANIMATION_DURATION);
#else
#error "Animation requires defining Kconfig CONFIG_ZMK_WIDGET_ANIMATION_SOURCE"
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_ANIMATION_MODE_ON_KEYPRESS)
  lv_animimg_set_repeat_count(widget->obj, 1);
  lv_anim_set_completed_cb(lv_animimg_get_anim(widget->obj), animation_completed_cb);
#else
  lv_animimg_set_repeat_count(widget->obj, LV_ANIM_REPEAT_INFINITE);
#endif

  // Play once to initialize layout
  lv_animimg_start(widget->obj);

  sys_slist_append(&widgets, &widget->node);

#if IS_ENABLED(CONFIG_ZMK_WIDGET_ANIMATION_MODE_ON_KEYPRESS)
  widget_animation_trigger_init();
#endif
  return 0;
}

lv_obj_t *zmk_widget_animation_obj(struct zmk_widget_animation *widget) { return widget->obj; }
