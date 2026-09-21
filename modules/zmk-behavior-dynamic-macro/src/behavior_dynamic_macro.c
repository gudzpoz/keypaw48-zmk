/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Runtime dynamic macros, in the Emacs sense: press &dm DM_REC, type a
 * sequence, press a slot key to save it there, then press that slot key to
 * replay it.
 *
 * Scope and limitations, all deliberate:
 *
 *  - Slots live in RAM only. Recordings are lost on reboot; nothing is ever
 *    written to flash.
 *  - Only resolved HID keycodes are captured, because the recorder listens to
 *    zmk_keycode_state_changed. Behaviors that raise no keycode event -- &mo,
 *    &lt, &bt, RGB, other &dm presses -- are not recorded, so a recording of
 *    "hold layer + key" replays the key without the layer.
 *  - Replay uses a fixed delay (CONFIG_ZMK_DYNAMIC_MACRO_TAP_DELAY_MS) between
 *    events. Original inter-key timing is not stored.
 *  - A modifier held across the recording boundary (pressed before DM_REC,
 *    released after the commit) is not captured. The reverse case -- pressed
 *    during the recording and never released before it stops -- is handled by
 *    dm_balance_scratch(), which appends the missing release.
 *  - Keys pressed while a macro is playing go live and interleave with it.
 *    Playback runs to completion and is not interruptible.
 *
 * The module's own control keys raise no keycode events, so they can never be
 * recorded and a self-recursive macro is impossible by construction.
 */

#define DT_DRV_COMPAT zmk_behavior_dynamic_macro

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#include <dt-bindings/zmk/dynamic_macro.h>

#include "dynamic_macro_state.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct dm_event {
  uint32_t keycode;
  uint16_t usage_page;
  uint8_t implicit_modifiers;
  uint8_t explicit_modifiers;
};

struct dm_slot {
  uint16_t start;
  uint16_t count;
};

/* Everything below is touched only from the system workqueue: behavior
 * handlers, kscan and split position events, and the playback pacer are all
 * delivered there. That workqueue is single threaded and event dispatch is
 * synchronous, so no locking is needed -- do not "fix" this with a mutex.
 *
 * The one exception is dm_recording, which the RGB matrix's indicator kind reads
 * from the low-priority workqueue. It is a single naturally aligned word, so the
 * read cannot tear; it may merely lag one render tick, which is immaterial for
 * an indicator. Hence `volatile`, and still no lock.
 */
static struct dm_slot dm_slots[CONFIG_ZMK_DYNAMIC_MACRO_SLOTS];
static struct dm_event dm_heap[CONFIG_ZMK_DYNAMIC_MACRO_MAX_EVENTS];
static uint8_t dm_heap_states[(CONFIG_ZMK_DYNAMIC_MACRO_MAX_EVENTS + 7) / 8];

static volatile bool dm_recording;
static struct dm_slot dm_scratch;
static uint16_t dm_dropped;

static bool dm_playback_active;
static uint8_t dm_play_slot;
static uint16_t dm_play_index;

#define DM_HEAP_STATE_FOR(n) (dm_heap_states[(n) / 8] & (1 << ((n) & 7)))
#define SET_DM_HEAP_STATE_FOR(n, v)                                            \
  dm_heap_states[(n) / 8] = (dm_heap_states[(n) / 8] & ~(1 << ((n) & 7))) |    \
                            ((v) ? (1 << ((n) & 7)) : 0)

/* Read by the RGB indicator kind (rgb_indicator_dynamic_macro.c), which runs on
 * the low-priority workqueue. See the note above the state. */
bool zmk_dynamic_macro_is_recording(void) { return dm_recording; }

static void dm_play_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(dm_play_work, dm_play_work_handler);
static void dm_play_work_handler(struct k_work *work) {
  const struct dm_slot *slot = &dm_slots[dm_play_slot];

  if (!dm_playback_active || dm_play_index >= slot->count) {
    dm_playback_active = false;
    return;
  }

  size_t i = slot->start + dm_play_index++;
  const struct dm_event *e = &dm_heap[i];
  bool state = DM_HEAP_STATE_FOR(i);

  /* Exactly the call &kp makes, with a fresh timestamp. */
  raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
      .usage_page = e->usage_page,
      .keycode = e->keycode,
      .implicit_modifiers = e->implicit_modifiers,
      .explicit_modifiers = e->explicit_modifiers,
      .state = state,
      .timestamp = k_uptime_get(),
    });

  k_work_schedule(&dm_play_work, K_MSEC(CONFIG_ZMK_DYNAMIC_MACRO_TAP_DELAY_MS));
}

/* Append a synthetic release for anything still held when the recording
 * stopped, so replay cannot leave a modifier stuck down on the host.
 */
static void dm_balance_scratch(void) {
  const uint16_t end = dm_scratch.start + dm_scratch.count;

  for (uint16_t i = dm_scratch.start; i < end; i++) {
    if (!DM_HEAP_STATE_FOR(i)) {
      continue;
    }

    bool released = false;
    for (uint16_t j = i + 1; j < end; j++) {
      if (!DM_HEAP_STATE_FOR(j) && dm_heap[j].keycode == dm_heap[i].keycode &&
          dm_heap[j].usage_page == dm_heap[i].usage_page) {
        released = true;
        break;
      }
    }

    if (released) {
      continue;
    }

    uint16_t n = dm_scratch.start + dm_scratch.count;
    if (n >= ARRAY_SIZE(dm_heap)) {
      LOG_WRN("Dynamic macro full; cannot balance held key 0x%04x",
              dm_heap[i].keycode);
      break;
    }

    dm_heap[n] = dm_heap[i];
    SET_DM_HEAP_STATE_FOR(n, 0);
    dm_scratch.count++;
  }
}

static void dm_commit_scratch(uint8_t slot) {
  if (slot >= ARRAY_SIZE(dm_slots)) {
    LOG_WRN("Dynamic macro slot %d out of range (max %d)", slot,
            ARRAY_SIZE(dm_slots) - 1);
    return;
  }

  dm_balance_scratch();

  if (dm_dropped > 0) {
    LOG_WRN("Dynamic macro truncated: %d events dropped while recording", dm_dropped);
  }

  dm_slots[slot] = dm_scratch;
  LOG_DBG("Dynamic macro committed %d events(:%d) to slot %d", dm_scratch.count,
          dm_scratch.start, slot);
  dm_dropped = 0;
}

static int dm_keycode_listener(const zmk_event_t *eh);
ZMK_LISTENER(dynamic_macro, dm_keycode_listener);
ZMK_SUBSCRIPTION(dynamic_macro, zmk_keycode_state_changed);

static int dm_keycode_listener(const zmk_event_t *eh) {
  const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

  /* Skip our own playback output, and capture only while recording. Always
   * bubble so hid_listener still sends the key. */
  if (ev == NULL || !dm_recording || dm_playback_active) {
    return ZMK_EV_EVENT_BUBBLE;
  }

  uint16_t n = dm_scratch.start + dm_scratch.count;
  if (n < ARRAY_SIZE(dm_heap)) {
    dm_heap[n] = (struct dm_event){
      .usage_page = ev->usage_page,
      .keycode = ev->keycode,
      .implicit_modifiers = ev->implicit_modifiers,
      .explicit_modifiers = ev->explicit_modifiers,
    };
    SET_DM_HEAP_STATE_FOR(n, ev->state);
    dm_scratch.count++;
  } else if (dm_dropped++ == 0) {
    LOG_WRN("Dynamic macro scratch full at %d events; dropping further input",
            ARRAY_SIZE(dm_heap));
  }

  return ZMK_EV_EVENT_BUBBLE;
}

/* Re-pack every live slot against the front of the heap and return the first
 * free index.
 */
static uint16_t dm_compact_heap(void) {
  uint16_t used = 0;
  uint16_t floor = 0;

  for (;;) {
    int best = -1;
    for (uint8_t s = 0; s < ARRAY_SIZE(dm_slots); s++) {
      if (dm_slots[s].count == 0 || dm_slots[s].start < floor) {
        continue;
      }
      if (best < 0 || dm_slots[s].start < dm_slots[best].start) {
        best = s;
      }
    }
    if (best < 0) {
      break;
    }

    const uint16_t from = dm_slots[best].start;
    const uint16_t count = dm_slots[best].count;

    for (uint16_t i = 0; i < count; i++) {
      dm_heap[used + i] = dm_heap[from + i];
      SET_DM_HEAP_STATE_FOR(used + i, DM_HEAP_STATE_FOR(from + i));
    }

    dm_slots[best].start = used;
    used += count;
    /* from + 1, not used + 1: the slot we just moved now has start == used,
     * which is below floor, so it cannot be picked again. */
    floor = from + 1;
  }

  return used;
}

static void dm_toggle_recording(void) {
  if (dm_recording) {
    dm_recording = false;
    LOG_DBG("Dynamic macro recording cancelled with %d events", dm_scratch.count);
    return;
  }

  dm_scratch.start = dm_compact_heap();
  dm_scratch.count = 0;
  dm_dropped = 0;
  dm_recording = true;
  LOG_DBG("Dynamic macro recording started at heap index %d", dm_scratch.start);
}

static void dm_play(uint8_t slot) {
  if (slot >= ARRAY_SIZE(dm_slots) || dm_slots[slot].count == 0) {
    LOG_DBG("Dynamic macro slot %d is empty", slot);
    return;
  }

  dm_play_slot = slot;
  dm_play_index = 0;

  /* Set the guard before scheduling: the listener must already see it when
   * the first event is raised. */
  dm_playback_active = true;
  k_work_schedule(&dm_play_work, K_NO_WAIT);

  LOG_DBG("Dynamic macro playing slot %d (%d events)", slot, dm_slots[slot].count);
}

static int dm_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);

  const uint32_t command = binding->param1;
  const uint8_t slot = (uint8_t)binding->param2;

  if (dm_playback_active) {
    LOG_DBG("Dynamic macro busy playing; ignoring command %u", command);
    return ZMK_BEHAVIOR_OPAQUE;
  }

  switch (command) {
  case DM_REC_CMD:
    dm_toggle_recording();
    break;

  case DM_PLAY_CMD:
    if (dm_recording) {
      /* While recording a slot key saves the take instead of playing it,
       * so one key per slot covers both saving and replaying. */
      dm_recording = false;
      dm_commit_scratch(slot);
      break;
    }
    dm_play(slot);
    break;

  default:
    LOG_ERR("Unknown dynamic macro command: %u", command);
    return -ENOTSUP;
  }

  return ZMK_BEHAVIOR_OPAQUE;
}

static int dm_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
  ARG_UNUSED(binding);
  ARG_UNUSED(event);

  return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api dm_behavior_driver_api = {
  .binding_pressed = dm_binding_pressed,
  .binding_released = dm_binding_released,
  /* locality is left at the default BEHAVIOR_LOCALITY_CENTRAL: this runs
   * where keycode events are raised, which is the central half. */
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &dm_behavior_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
