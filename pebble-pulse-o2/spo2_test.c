/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Pulse O2 - Pebble Time 2 blood oxygen spot-check app.
 *
 * Built in-tree because SpO2 is currently exposed to PebbleOS firmware
 * applications through the HRM manager, but not yet through the public app SDK.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "applib/app.h"
#include "applib/battery_state_service.h"
#include "applib/health_service.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/click.h"
#include "applib/ui/text_layer.h"
#include "applib/ui/window.h"
#include "apps/system_app_ids.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "pbl/services/activity/activity.h"
#include "pbl/services/hrm/hrm_manager.h"
#include "util/time/time.h"

#define HISTORY_LEN 10
#define VALUE_LEN 12
#define STATUS_LEN 48
#define METRICS_LEN 64
#define DETAIL_LEN 196

typedef struct {
  HRMSessionRef session;
  EventServiceInfo hrm_event_info;
  bool event_subscribed;
  bool show_details;

  uint32_t sample_count;
  uint8_t last_percent;
  HRMQuality last_quality;
  uint8_t last_confidence;
  uint8_t last_valid_level;
  bool last_invalid;

  uint8_t history[HISTORY_LEN];
  uint8_t history_count;

  Window window;
  TextLayer title_layer;
  TextLayer value_layer;
  TextLayer status_layer;
  TextLayer metrics_layer;
  TextLayer detail_layer;
  TextLayer footer_layer;

  char value_string[VALUE_LEN];
  char status_string[STATUS_LEN];
  char metrics_string[METRICS_LEN];
  char detail_string[DETAIL_LEN];
} AppData;

static const char *prv_quality_string(HRMQuality quality) {
  switch (quality) {
    case HRMQuality_OffWrist:
      return "Off wrist";
    case HRMQuality_Worst:
      return "Very poor";
    case HRMQuality_Poor:
      return "Poor";
    case HRMQuality_Acceptable:
      return "Acceptable";
    case HRMQuality_Good:
      return "Good";
    case HRMQuality_Excellent:
      return "Excellent";
    default:
      return "Unknown";
  }
}

static void prv_push_history(AppData *data, uint8_t percent) {
  if ((data->history_count > 0) && (data->history[data->history_count - 1] == percent)) {
    return;
  }

  if (data->history_count < HISTORY_LEN) {
    data->history[data->history_count++] = percent;
    return;
  }

  for (uint8_t i = 1; i < HISTORY_LEN; ++i) {
    data->history[i - 1] = data->history[i];
  }
  data->history[HISTORY_LEN - 1] = percent;
}

static void prv_build_history_string(AppData *data, char *buffer, size_t buffer_size) {
  if (buffer_size == 0) {
    return;
  }

  buffer[0] = '\0';
  size_t used = 0;
  for (uint8_t i = 0; i < data->history_count; ++i) {
    int written = snprintf(buffer + used, buffer_size - used, "%s%" PRIu8,
                           (i == 0) ? "" : " ", data->history[i]);
    if (written < 0) {
      break;
    }
    if ((size_t)written >= (buffer_size - used)) {
      used = buffer_size - 1;
      break;
    }
    used += (size_t)written;
  }

  if (data->history_count == 0) {
    snprintf(buffer, buffer_size, "none yet");
  }
}

static void prv_refresh_side_data(AppData *data) {
  HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
  BatteryChargeState battery = battery_state_service_peek();

  if (hr > 0) {
    snprintf(data->metrics_string, sizeof(data->metrics_string),
             "HR %" PRIi32 " bpm   BAT %" PRIu8 "%%", (int32_t)hr, battery.charge_percent);
  } else {
    snprintf(data->metrics_string, sizeof(data->metrics_string),
             "HR -- bpm   BAT %" PRIu8 "%%", battery.charge_percent);
  }
  text_layer_set_text(&data->metrics_layer, data->metrics_string);
}

static void prv_refresh_detail(AppData *data) {
  char history[64];
  prv_build_history_string(data, history, sizeof(history));

  if (data->show_details) {
    snprintf(data->detail_string, sizeof(data->detail_string),
             "Signal: %s\n"
             "Confidence: %" PRIu8 "  Valid: %" PRIu8 "\n"
             "Invalid: %s  Samples: %" PRIu32 "\n"
             "Accepted history: %s",
             prv_quality_string(data->last_quality), data->last_confidence, data->last_valid_level,
             data->last_invalid ? "yes" : "no", data->sample_count, history);
  } else if (data->history_count > 0) {
    snprintf(data->detail_string, sizeof(data->detail_string),
             "Keep the watch snug and stay still.\n"
             "Recent accepted readings: %s", history);
  } else {
    snprintf(data->detail_string, sizeof(data->detail_string),
             "Keep the watch snug and stay still.\n"
             "A spot reading can take a little while.");
  }

  text_layer_set_text(&data->detail_layer, data->detail_string);
  text_layer_set_text(&data->footer_layer,
                      data->show_details ? "SELECT retry   DOWN summary"
                                         : "SELECT retry   DOWN details");
}

static void prv_set_measuring(AppData *data) {
  snprintf(data->value_string, sizeof(data->value_string), "--");
  snprintf(data->status_string, sizeof(data->status_string), "Measuring...");
  text_layer_set_text(&data->value_layer, data->value_string);
  text_layer_set_text(&data->status_layer, data->status_string);
  data->last_percent = 0;
  data->last_quality = HRMQuality_Worst;
  data->last_confidence = 0;
  data->last_valid_level = 0;
  data->last_invalid = false;
  data->sample_count = 0;
  prv_refresh_side_data(data);
  prv_refresh_detail(data);
  layer_mark_dirty(&data->window.layer);
}

static void prv_update_from_spo2(AppData *data, const HRMSpO2Data *spo2) {
  data->sample_count++;
  data->last_quality = spo2->quality;
  data->last_confidence = spo2->confidence;
  data->last_valid_level = spo2->valid_level;
  data->last_invalid = spo2->invalid;

  const bool off_wrist = (spo2->quality == HRMQuality_OffWrist);
  const bool accepted = !off_wrist && !spo2->invalid && (spo2->percent > 0);

  if (off_wrist) {
    snprintf(data->value_string, sizeof(data->value_string), "--");
    snprintf(data->status_string, sizeof(data->status_string), "Off wrist");
  } else if (!accepted) {
    if (spo2->percent > 0) {
      snprintf(data->value_string, sizeof(data->value_string), "%" PRIu8 "%%?", spo2->percent);
    } else {
      snprintf(data->value_string, sizeof(data->value_string), "--");
    }
    snprintf(data->status_string, sizeof(data->status_string), "Rejected C%" PRIu8 " V%" PRIu8,
             spo2->confidence, spo2->valid_level);
    // Automatically expose the diagnostic data on a rejected sample. The percent above is the
    // algorithm's raw estimate only and is deliberately marked with '?' because invalid=true means
    // PebbleOS must not treat it as a usable SpO2 reading.
    data->show_details = true;
  } else {
    snprintf(data->value_string, sizeof(data->value_string), "%" PRIu8 "%%", spo2->percent);
    snprintf(data->status_string, sizeof(data->status_string), "%s signal",
             prv_quality_string(spo2->quality));
    if ((data->last_percent == 0) || (data->last_percent != spo2->percent)) {
      prv_push_history(data, spo2->percent);
    }
    data->last_percent = spo2->percent;
  }

  text_layer_set_text(&data->value_layer, data->value_string);
  text_layer_set_text(&data->status_layer, data->status_string);
  prv_refresh_side_data(data);
  prv_refresh_detail(data);
  layer_mark_dirty(&data->window.layer);
}

static void prv_subscribe_sensor(AppData *data) {
  if (data->session != HRM_INVALID_SESSION_REF) {
    sys_hrm_manager_unsubscribe(data->session);
    data->session = HRM_INVALID_SESSION_REF;
  }

  data->session = sys_hrm_manager_app_subscribe(
      APP_ID_SPO2_TEST, 1 /* update_interval_s */, SECONDS_PER_HOUR, HRMFeature_SpO2);

  if (data->session == HRM_INVALID_SESSION_REF) {
    snprintf(data->status_string, sizeof(data->status_string), "Sensor start failed");
    text_layer_set_text(&data->status_layer, data->status_string);
  }
}

static void prv_restart_measurement(AppData *data) {
  prv_set_measuring(data);
  prv_subscribe_sensor(data);
}

static void prv_handle_hrm_data(PebbleEvent *e, void *context) {
  (void)context;
  AppData *data = app_state_get_user_data();
  PebbleHRMEvent *hrm = &e->hrm;

  if (hrm->event_type == HRMEvent_SpO2) {
    prv_update_from_spo2(data, &hrm->spo2);
  } else if (hrm->event_type == HRMEvent_SubscriptionExpiring) {
    prv_subscribe_sensor(data);
  }
}

static void prv_enable_spo2(AppData *data) {
  data->hrm_event_info = (EventServiceInfo){
    .type = PEBBLE_HRM_EVENT,
    .handler = prv_handle_hrm_data,
  };
  event_service_client_subscribe(&data->hrm_event_info);
  data->event_subscribed = true;
  prv_restart_measurement(data);
}

static void prv_disable_spo2(AppData *data) {
  if (data->event_subscribed) {
    event_service_client_unsubscribe(&data->hrm_event_info);
    data->event_subscribed = false;
  }
  if (data->session != HRM_INVALID_SESSION_REF) {
    sys_hrm_manager_unsubscribe(data->session);
    data->session = HRM_INVALID_SESSION_REF;
  }
}

static void prv_select_click(ClickRecognizerRef recognizer, Window *window) {
  (void)recognizer;
  AppData *data = window_get_user_data(window);
  prv_restart_measurement(data);
}

static void prv_down_click(ClickRecognizerRef recognizer, Window *window) {
  (void)recognizer;
  AppData *data = window_get_user_data(window);
  data->show_details = !data->show_details;
  prv_refresh_detail(data);
  layer_mark_dirty(&data->window.layer);
}

static void prv_click_config(Window *window) {
  window_single_click_subscribe(BUTTON_ID_SELECT, (ClickHandler)prv_select_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, (ClickHandler)prv_down_click);
}

static void prv_style_text(TextLayer *layer, GFont font, GTextAlignment alignment) {
  text_layer_set_font(layer, font);
  text_layer_set_text_alignment(layer, alignment);
  text_layer_set_text_color(layer, GColorWhite);
  text_layer_set_background_color(layer, GColorBlack);
}

static void prv_init(void) {
  AppData *data = app_malloc_check(sizeof(*data));
  *data = (AppData){
    .session = HRM_INVALID_SESSION_REF,
    .last_quality = HRMQuality_Worst,
  };
  app_state_set_user_data(data);

  Window *window = &data->window;
  window_init(window, "Pulse O2");
  window_set_user_data(window, data);
  window_set_fullscreen(window, true);
  window_set_background_color(window, GColorBlack);
  window_set_click_config_provider(window, (ClickConfigProvider)prv_click_config);

  const GRect bounds = window->layer.bounds;

  GRect frame = GRect(0, 4, bounds.size.w, 24);
  text_layer_init(&data->title_layer, &frame);
  prv_style_text(&data->title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                 GTextAlignmentCenter);
  text_layer_set_text(&data->title_layer, "PULSE O2");
  layer_add_child(&window->layer, &data->title_layer.layer);

  frame = GRect(0, 24, bounds.size.w, 58);
  text_layer_init(&data->value_layer, &frame);
  prv_style_text(&data->value_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
                 GTextAlignmentCenter);
  text_layer_set_text(&data->value_layer, "--");
  layer_add_child(&window->layer, &data->value_layer.layer);

  frame = GRect(0, 76, bounds.size.w, 25);
  text_layer_init(&data->status_layer, &frame);
  prv_style_text(&data->status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                 GTextAlignmentCenter);
  text_layer_set_text(&data->status_layer, "Starting...");
  layer_add_child(&window->layer, &data->status_layer.layer);

  frame = GRect(0, 102, bounds.size.w, 22);
  text_layer_init(&data->metrics_layer, &frame);
  prv_style_text(&data->metrics_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                 GTextAlignmentCenter);
  layer_add_child(&window->layer, &data->metrics_layer.layer);

  frame = GRect(8, 127, bounds.size.w - 16, bounds.size.h - 157);
  text_layer_init(&data->detail_layer, &frame);
  prv_style_text(&data->detail_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                 GTextAlignmentLeft);
  layer_add_child(&window->layer, &data->detail_layer.layer);

  frame = GRect(0, bounds.size.h - 26, bounds.size.w, 22);
  text_layer_init(&data->footer_layer, &frame);
  prv_style_text(&data->footer_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                 GTextAlignmentCenter);
  layer_add_child(&window->layer, &data->footer_layer.layer);

  app_window_stack_push(window, true);

  prv_refresh_side_data(data);
  prv_refresh_detail(data);

  if (!sys_hrm_manager_is_hrm_present()) {
    text_layer_set_text(&data->status_layer, "No optical sensor");
  } else if (!activity_prefs_heart_rate_is_enabled() &&
             !activity_prefs_blood_oxygen_is_enabled() &&
             !activity_prefs_blood_oxygen_activity_tracking_is_enabled()) {
    text_layer_set_text(&data->status_layer, "Monitoring is disabled");
    text_layer_set_text(&data->detail_layer,
                        "Enable heart rate or blood oxygen\nmonitoring in Settings first.");
  } else {
    prv_enable_spo2(data);
  }
}

static void prv_deinit(void) {
  AppData *data = app_state_get_user_data();
  prv_disable_spo2(data);
  app_free(data);
}

static void prv_main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}

const PebbleProcessMd *spo2_test_get_app_info(void) {
  static const PebbleProcessMdSystem s_pulse_o2_app_info = {
    .name = "Pulse O2",
    .common.uuid =
        {0x6d, 0x6c, 0x3b, 0x0e, 0x2a, 0x4f, 0x4c, 0x91, 0x9b, 0x2d, 0x1f, 0x77, 0x84, 0x3c, 0x5e,
         0xa1},
    .common.main_func = &prv_main,
    .common.visibility = ProcessVisibilityShown,
  };
  return (sys_hrm_manager_is_hrm_present()) ? (const PebbleProcessMd *)&s_pulse_o2_app_info : NULL;
}
