/*
 * Copyright (c) 2024
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * FT6236 Virtual Touchpad — HID transport.
 *
 * Registers a USB HID device (HID_1) that presents itself as a Windows
 * Precision Touchpad (PTP). Adapted from the zmk-trackball-gestures-module
 * transport, reduced to the FT6236's 2-finger layout.
 */

#include <zmk/ft6236_touchpad.h>
#include "touchpad_hid_descriptor.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#endif

LOG_MODULE_REGISTER(ft6236_touchpad, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_USB)

/* HID report descriptor instantiation */
static const uint8_t touchpad_hid_report_desc[] = {
  FT6236_TP_HID_REPORT_DESC
};

/* Internal state */
static const struct device *hid_dev;
static bool touchpad_ready;
static K_MUTEX_DEFINE(send_mutex);

/* Feature report data */
static const struct ft6236_tp_feature_max_count feature_max_count = {
  .report_id = FT6236_TP_FEATURE_MAX_COUNT_ID,
  .max_count = FT6236_TOUCHPAD_MAX_CONTACTS,
  .pad_type  = 0x00,
};
static uint8_t ptphqa_response[257] = {
  FT6236_TP_FEATURE_PTPHQA_ID,
  0xfc, 0x28, 0xfe, 0x84, 0x40, 0xcb, 0x9a, 0x87, 0x0d, 0xbe, 0x57, 0x3c, 0xb6, 0x70, 0x09, 0x88, 0x07,
  0x97, 0x2d, 0x2b, 0xe3, 0x38, 0x34, 0xb6, 0x6c, 0xed, 0xb0, 0xf7, 0xe5, 0x9c, 0xf6, 0xc2, 0x2e, 0x84,
  0x1b, 0xe8, 0xb4, 0x51, 0x78, 0x43, 0x1f, 0x28, 0x4b, 0x7c, 0x2d, 0x53, 0xaf, 0xfc, 0x47, 0x70, 0x1b,
  0x59, 0x6f, 0x74, 0x43, 0xc4, 0xf3, 0x47, 0x18, 0x53, 0x1a, 0xa2, 0xa1, 0x71, 0xc7, 0x95, 0x0e, 0x31,
  0x55, 0x21, 0xd3, 0xb5, 0x1e, 0xe9, 0x0c, 0xba, 0xec, 0xb8, 0x89, 0x19, 0x3e, 0xb3, 0xaf, 0x75, 0x81,
  0x9d, 0x53, 0xb9, 0x41, 0x57, 0xf4, 0x6d, 0x39, 0x25, 0x29, 0x7c, 0x87, 0xd9, 0xb4, 0x98, 0x45, 0x7d,
  0xa7, 0x26, 0x9c, 0x65, 0x3b, 0x85, 0x68, 0x89, 0xd7, 0x3b, 0xbd, 0xff, 0x14, 0x67, 0xf2, 0x2b, 0xf0,
  0x2a, 0x41, 0x54, 0xf0, 0xfd, 0x2c, 0x66, 0x7c, 0xf8, 0xc0, 0x8f, 0x33, 0x13, 0x03, 0xf1, 0xd3, 0xc1, 0x0b,
  0x89, 0xd9, 0x1b, 0x62, 0xcd, 0x51, 0xb7, 0x80, 0xb8, 0xaf, 0x3a, 0x10, 0xc1, 0x8a, 0x5b, 0xe8, 0x8a,
  0x56, 0xf0, 0x8c, 0xaa, 0xfa, 0x35, 0xe9, 0x42, 0xc4, 0xd8, 0x55, 0xc3, 0x38, 0xcc, 0x2b, 0x53, 0x5c,
  0x69, 0x52, 0xd5, 0xc8, 0x73, 0x02, 0x38, 0x7c, 0x73, 0xb6, 0x41, 0xe7, 0xff, 0x05, 0xd8, 0x2b, 0x79,
  0x9a, 0xe2, 0x34, 0x60, 0x8f, 0xa3, 0x32, 0x1f, 0x09, 0x78, 0x62, 0xbc, 0x80, 0xe3, 0x0f, 0xbd, 0x65,
  0x20, 0x08, 0x13, 0xc1, 0xe2, 0xee, 0x53, 0x2d, 0x86, 0x7e, 0xa7, 0x5a, 0xc5, 0xd3, 0x7d, 0x98, 0xbe,
  0x31, 0x48, 0x1f, 0xfb, 0xda, 0xaf, 0xa2, 0xa8, 0x6a, 0x89, 0xd6, 0xbf, 0xf2, 0xd3, 0x32, 0x2a, 0x9a,
  0xe4, 0xcf, 0x17, 0xb7, 0xb8, 0xf4, 0xe1, 0x33, 0x08, 0x24, 0x8b, 0xc4, 0x43, 0xa5, 0xe5, 0x24, 0xc2
};

static uint8_t current_input_mode = 0;
static uint8_t current_device_index = 0;
static uint8_t surface_switch = 1;
static uint8_t button_switch = 1;

static struct ft6236_tp_feature_input_mode input_mode_response = {
  .report_id    = FT6236_TP_FEATURE_CONFIG_ID,
  .input_mode   = 3,
  .device_index = 0,
};

static uint8_t fn_switch_response[2] = { FT6236_TP_FEATURE_FUNCTION_SWITCH_ID, 0x03 };

/* HID class callbacks */
static int touchpad_get_report(const struct device *dev,
                               struct usb_setup_packet *setup,
                               int32_t *len, uint8_t **data) {
  uint8_t report_id = (uint8_t)(setup->wValue & 0xFF);
  uint8_t report_type = (uint8_t)(setup->wValue >> 8);

  if (report_type != 3) {
    LOG_WRN("Unexpected GET_REPORT type %u for report %u", report_type, report_id);
    return -ENOTSUP;
  }

  switch (report_id) {
  case FT6236_TP_FEATURE_MAX_COUNT_ID:
    *data = (uint8_t *)&feature_max_count;
    *len = sizeof(feature_max_count);
    break;
  case FT6236_TP_FEATURE_PTPHQA_ID:
    *data = ptphqa_response;
    *len = sizeof(ptphqa_response);
    break;
  case FT6236_TP_FEATURE_CONFIG_ID:
    input_mode_response.input_mode = current_input_mode;
    input_mode_response.device_index = current_device_index;
    *data = (uint8_t *)&input_mode_response;
    *len = sizeof(input_mode_response);
    break;
  case FT6236_TP_FEATURE_FUNCTION_SWITCH_ID: {
    fn_switch_response[0] = FT6236_TP_FEATURE_FUNCTION_SWITCH_ID;
    fn_switch_response[1] = (surface_switch & 0x01) | ((button_switch & 0x01) << 1);
    *data = fn_switch_response;
    *len = sizeof(fn_switch_response);
    break;
  }
  default:
    LOG_WRN("GET_REPORT: unknown report ID %u", report_id);
    return -ENOTSUP;
  }

  return 0;
}

static int touchpad_set_report(const struct device *dev,
                               struct usb_setup_packet *setup,
                               int32_t *len, uint8_t **data) {
  uint8_t report_id = (uint8_t)(setup->wValue & 0xFF);

  if (*len < 1 || !*data) {
    LOG_WRN("SET_REPORT: empty payload for report %u", report_id);
    return 0;
  }

  switch (report_id) {
  case FT6236_TP_FEATURE_CONFIG_ID: {
    int offset = ((*len > 0 && (*data)[0] == report_id) ? 1 : 0);
    if (*len > offset) {
      current_input_mode = (*data)[offset];
    }
    if (*len > offset + 1) {
      current_device_index = (*data)[offset + 1];
    }
    LOG_INF("SET_REPORT: input mode set to %u", current_input_mode);
    break;
  }
  case FT6236_TP_FEATURE_FUNCTION_SWITCH_ID: {
    int offset = ((*len > 0 && (*data)[0] == report_id) ? 1 : 0);
    if (*len > offset) {
      surface_switch = (*data)[offset] & 0x01;
      button_switch = ((*data)[offset] >> 1) & 0x01;
    }
    LOG_INF("SET_REPORT: surface_switch=%u, button_switch=%u",
            surface_switch, button_switch);
    break;
  }
  default:
    LOG_WRN("SET_REPORT: unhandled report ID %u", report_id);
    break;
  }

  return 0;
}

static void touchpad_int_in_ready(const struct device *dev) {
  /* Reports are sent on demand. */
}

static const struct hid_ops touchpad_hid_ops = {
  .get_report   = touchpad_get_report,
  .set_report   = touchpad_set_report,
  .int_in_ready = touchpad_int_in_ready,
};

/* Public API */

int ft6236_touchpad_init(void) {
  if (touchpad_ready) {
    LOG_WRN("FT6236 touchpad already initialized");
    return 0;
  }

  hid_dev = device_get_binding("HID_1");
  if (!hid_dev) {
    LOG_ERR("Failed to get HID_1 device binding. Touchpad disabled.");
    return 0;
  }

  usb_hid_register_device(hid_dev, touchpad_hid_report_desc,
                          sizeof(touchpad_hid_report_desc), &touchpad_hid_ops);
  usb_hid_set_proto_code(hid_dev, 0);

  int ret = usb_hid_init(hid_dev);
  if (ret) {
    LOG_ERR("usb_hid_init failed: %d", ret);
    return ret;
  }

  touchpad_ready = true;
  LOG_INF("FT6236 virtual touchpad initialized (descriptor %zu bytes)",
          sizeof(touchpad_hid_report_desc));
  return 0;
}

int ft6236_touchpad_send_report(const struct ft6236_touchpad_report *report) {
  if (!report) {
    return -EINVAL;
  }

  if (!ft6236_touchpad_is_ready()) {
    LOG_WRN("Touchpad not ready — dropping report");
    return -ENODEV;
  }

  k_mutex_lock(&send_mutex, K_FOREVER);

  static struct ft6236_tp_input_report hid_reports[4];
  static uint8_t report_idx = 0;

  struct ft6236_tp_input_report *hid_report = &hid_reports[report_idx];
  report_idx = (report_idx + 1) % 4;

  memset(hid_report, 0, sizeof(*hid_report));

  hid_report->report_id = FT6236_TP_REPORT_ID;

  for (int i = 0; i < FT6236_TOUCHPAD_MAX_CONTACTS; i++) {
    struct ft6236_tp_contact_report *dst = &hid_report->contacts[i];
    const struct ft6236_touchpad_contact *src = &report->contacts[i];

    /*
     * Every slot is reported: confidence is always set for real
     * contact slots and the tip switch carries up/down. Lifted
     * contacts keep their last known position and contact id, as
     * the PTP spec requires for final frames.
     */
    dst->flags = (uint8_t)(1U                               /* confidence */
                           | ((src->active ? 1U : 0U) << 1) /* tip switch */
                          );
    dst->x = src->x;
    dst->y = src->y;
    dst->contact_id = src->id;
  }

  static uint32_t last_scan_time = 0;
  uint32_t current_scan = k_uptime_get_32() * 10;
  if ((int32_t)(current_scan - last_scan_time) <= 0) {
    current_scan = last_scan_time + 10;
  }
  last_scan_time = current_scan;
  hid_report->scan_time = (uint16_t)(current_scan & 0xFFFF);

  hid_report->contact_count = report->contact_count;
  hid_report->buttons = report->button ? 0x01 : 0x00;

  int ret = hid_int_ep_write(hid_dev, (const uint8_t *)hid_report,
                             sizeof(*hid_report), NULL);

  k_mutex_unlock(&send_mutex);

  if (ret) {
    LOG_WRN("hid_int_ep_write failed: %d", ret);
    return ret;
  }

  return 0;
}

bool ft6236_touchpad_is_ready(void) {
  return touchpad_ready && (hid_dev != NULL);
}

bool ft6236_touchpad_surface_enabled(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
  /*
   * PTP host controls: Surface Switch 0 = touch disabled; Input Mode
   * 0 = the host asked for (PS/2-style) mouse emulation, which we do
   * not implement — stay silent rather than report wrong modes.
   * Without USB there is no host to ask: keep the touch surface live.
   */
  return surface_switch && (current_input_mode != 0);
#else
  return true;
#endif
}

SYS_INIT(ft6236_touchpad_init, APPLICATION, 49);

#else /* !IS_ENABLED(CONFIG_ZMK_USB) */

int ft6236_touchpad_send_report(const struct ft6236_touchpad_report *report) {
  ARG_UNUSED(report);
  return 0;
}

bool ft6236_touchpad_is_ready(void) {
  return false;
}

bool ft6236_touchpad_surface_enabled(void) {
  return true;
}

#endif /* IS_ENABLED(CONFIG_ZMK_USB) */
