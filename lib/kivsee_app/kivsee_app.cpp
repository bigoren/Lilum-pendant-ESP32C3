// Kivsee networked-animation app implementation (esp32c3_kivsee env only).
// Adapted from the esp32-animations project's main.cpp setup()/loop(), minus
// Influxdb metrics reporting (no-op on C3). LED output writes into FastLED's
// shared CRGB buffer in led_engine (see Renderer::show()) instead of a local
// NeoPixelBus instance.
//
// All logging here uses ESP_LOG* (not the upstream Arduino Serial.print*) so
// it lands on the same USB-Serial/JTAG console as the rest of the firmware —
// the Arduino `Serial` object wraps UART0 in this build and goes to physical
// pins, not the monitor. The deeply-vendored files (fs_manager, mqtt_manager,
// segment_store, etc.) are left untouched and still use Serial; their output
// is silent on the monitor for now (acceptable trade vs. modifying upstream).

#include "kivsee_app.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <SPIFFS.h>
#include <cstring>
#include "esp_log.h"
#include "led_engine.h"
#include "app_mode.h"

static const char *TAG = "KIVSEE";

#include "secrets.h"
#include "segment_store.h"
#include "sequence.h"
#include "brightness.h"
#include "renderer.h"
#include "mqtt_managers/mqtt_manager.h"
#include "fs_manager.h"
#include "time_manager.h"
#include "queue_manager.h"

// No-hang guarantee (see plan inherited-popping-marshmallow.md, Phase 5):
// if the kivsee path cannot come up, we must fall back to standalone mode
// (persist + reboot) rather than spin. The two failure paths are:
//   - missing thing_info on SPIFFS (handled in kivsee_app_setup)
//   - WiFi never connects (this timeout, enforced in kivsee_app_loop)
// MQTT failures are intentionally soft — kivsee can render cached without
// MQTT — so they do NOT contribute to this timeout.
#define WIFI_FIRST_CONNECT_TIMEOUT_MS  180000  // 180 s

#define MAX_THING_NAME_LENGTH 16
static char thing_name[MAX_THING_NAME_LENGTH];

static const QueueManager queueManager;
static esp32animations::Renderer *renderer = nullptr; // created after pixel count is known
static SequenceManager sequenceManager(queueManager.runtime_animation_queue,
                                       queueManager.runtime_animation_delete_queue);
static FsManager fsManager;
static TimeManager timeManager(queueManager.epoch_time_update_queue);

static unsigned int lastWiFiCheckTime = 0;
static unsigned int lastReportTime = 0;

// First-connect tracking for the no-hang WiFi timeout. Anchor is set to
// millis() at kivsee_app_setup; the 180 s clock runs from there until the
// first successful WL_CONNECTED transition. After the first success this
// flag is cleared and the timeout never re-arms — subsequent drop-outs are
// handled by ConnectToWifi's normal 10 s retry without ever forcing a
// fallback (a kivsee device that briefly loses WiFi should not reboot
// itself out of kivsee mode).
static unsigned long wifi_first_attempt_ms = 0;
static bool          wifi_ever_connected   = false;

class MqttCallbacks : public MqttManagerCallbacks
{
public:
  void NewConfigGuidReceived(const byte *payload, unsigned int length, const char *thing_name)
  {
    handleSegmentsGuidMessage(payload, length, thing_name);
  }

  void TriggerInvoked(const byte *payload, unsigned int length)
  {
    // Copy into the trigger queue; the HTTP/decode work happens in the loop,
    // not inside the MQTT callback (which would block the mqtt loop).
    QueueManager::TriggerMessage msg;
    if (length > sizeof(msg.payload)) length = sizeof(msg.payload);
    msg.length = (uint16_t)length;
    memcpy(msg.payload, payload, msg.length);
    xQueueSend(queueManager.trigger_queue, &msg, 0);
  }

  void NewGlobalBrightnessReceived(const byte *payload, unsigned int length)
  {
    float new_global_brightness;
    bool success = handleGlobalBrightnessMessage(payload, length, &new_global_brightness);
    if (success) {
      xQueueSend(queueManager.global_brightness_queue, &new_global_brightness, pdMS_TO_TICKS(100));
    }
  }
};

static MqttCallbacks mqttCallbacks;
static MqttManager *mqttManager = nullptr;

static void ConnectToWifi()
{
  if (WiFi.status() == WL_CONNECTED)
    return;

  static unsigned int connectStartTime = 0;
  static bool connecting = false;

  if (!connecting) {
    connectStartTime = millis();
    connecting = true;
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(SSID, WIFI_PASSWORD);
    ESP_LOGI(TAG, "Attempting to connect to SSID: %s", SSID);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    ESP_LOGI(TAG, "connected to wifi");
    connecting = false;
    if (!wifi_ever_connected) {
      wifi_ever_connected = true;
      led_engine_set_connecting_blink(false);
      ESP_LOGI(TAG, "first WiFi connect — clearing connecting-blink overlay");
    }
    httpGetConfig(thing_name);
    return;
  }

  if (millis() - connectStartTime >= 10000)
  {
    ESP_LOGW(TAG, "could not connect for 10 seconds. retry");
    connecting = false;
  }
}

void kivsee_app_setup(void)
{
  ESP_LOGI(TAG, "kivsee_app_setup: mounting SPIFFS");
  if (!SPIFFS.begin(true))
  {
    ESP_LOGE(TAG, "SPIFFS mount failed");
    return;
  }

  // No-hang fallback #1: missing thing_info. Previously this spun forever
  // waiting for the file. Now: log a clear message, persist STANDALONE,
  // reboot. The user can fix the SPIFFS image, re-flash, and triple-press
  // to opt back into kivsee mode.
  if (!fsManager.ReadThingName(thing_name, MAX_THING_NAME_LENGTH)) {
    ESP_LOGE(TAG, "Thing name not configured — upload 'thing_info' to SPIFFS "
                  "(pio run -e esp32c3_kivsee -t uploadfs). Falling back to STANDALONE.");
    app_mode_switch_to_standalone();   // does not return
    return;                            // unreachable; keeps the compiler happy
  }
  ESP_LOGI(TAG, "Thing name: %s", thing_name);

  // Arm the no-hang WiFi timer and start the connecting-blink overlay.
  // ConnectToWifi clears the blink on the first WL_CONNECTED transition;
  // kivsee_app_loop enforces the 180 s timeout if that never happens.
  wifi_first_attempt_ms = millis();
  wifi_ever_connected   = false;
  led_engine_set_connecting_blink(true);

  // The physical ring is fixed in this firmware (27 animation LEDs after the
  // status pixel). Ignore data/num_pixels — we drive FastLED's shared buffer
  // and must match its size.
  const uint16_t number_of_leds = (uint16_t)led_engine_num_anim_leds();
  ESP_LOGI(TAG, "Renderer init: %u LEDs (hardware ring)", (unsigned)number_of_leds);

  renderer = new esp32animations::Renderer(queueManager, number_of_leds);
  initSegmentStore(renderer->hsv_painting_array(), number_of_leds);
  fsManager.setup();

  mqttManager = createMqttManager(&mqttCallbacks, &fsManager);

  ConnectToWifi();
  ArduinoOTA.setHostname(thing_name);
  ArduinoOTA
      .onStart([]() {
        const char *type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        ESP_LOGI(TAG, "OTA: start updating %s", type);
      })
      .onEnd([]() { ESP_LOGI(TAG, "OTA: end"); })
      .onProgress([](unsigned int progress, unsigned int total) {
        ESP_LOGI(TAG, "OTA progress: %u%%", (progress / (total / 100)));
      })
      .onError([](ota_error_t error) {
        const char *err =
            (error == OTA_AUTH_ERROR)    ? "Auth Failed"    :
            (error == OTA_BEGIN_ERROR)   ? "Begin Failed"   :
            (error == OTA_CONNECT_ERROR) ? "Connect Failed" :
            (error == OTA_RECEIVE_ERROR) ? "Receive Failed" :
            (error == OTA_END_ERROR)     ? "End Failed"     : "Unknown";
        ESP_LOGE(TAG, "OTA error[%u]: %s", error, err);
      });

  ArduinoOTA.begin();
  timeManager.begin();
  ESP_LOGI(TAG, "kivsee_app_setup: complete");
}

void kivsee_app_loop(void)
{
  unsigned long current_millis = millis();

  // No-hang fallback #2: WiFi never connects. Only arms before the first
  // successful connect; after that, drop-outs are soft (ConnectToWifi
  // retries every 10 s without forcing a fallback).
  if (!wifi_ever_connected &&
      current_millis - wifi_first_attempt_ms >= WIFI_FIRST_CONNECT_TIMEOUT_MS) {
    ESP_LOGE(TAG, "WiFi did not connect within %u ms — falling back to STANDALONE",
             (unsigned)WIFI_FIRST_CONNECT_TIMEOUT_MS);
    led_engine_set_connecting_blink(false);
    app_mode_switch_to_standalone();   // does not return
  }

  // WiFi + MQTT connection upkeep (every 10s).
  if (current_millis - lastWiFiCheckTime >= 10000)
  {
    ConnectToWifi();
    mqttManager->connectToMessageBroker(thing_name);
    lastWiFiCheckTime = current_millis;
  }

  ArduinoOTA.handle();

  // Status report (every 5s).
  if (current_millis - lastReportTime >= 5000)
  {
    ESP_LOGI(TAG, "millis=%lu wifi=%d mqtt=%d rssi=%ld",
             (unsigned long)millis(),
             WiFi.status() == WL_CONNECTED,
             mqttManager->connected(),
             (long)WiFi.RSSI());
    lastReportTime = current_millis;
  }

  timeManager.loop();
  mqttManager->loop();

  // Drain at most one trigger per loop (HTTP + protobuf decode is heavy).
  QueueManager::TriggerMessage trigMsg;
  if (xQueueReceive(queueManager.trigger_queue, &trigMsg, 0) == pdTRUE) {
    sequenceManager.handleTriggerInvokedMessage(trigMsg.payload, trigMsg.length, thing_name);
  }

  sequenceManager.loop();

  if (renderer) {
    renderer->loop(current_millis);
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
