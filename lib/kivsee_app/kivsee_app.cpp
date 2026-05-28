// Networked-animation app wrapper (kivsee env only). Logs via ESP_LOG so
// they reach USB-Serial/JTAG; the upstream vendored files still use Arduino
// Serial (UART0) and are silent on the monitor.

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

// MQTT failures are soft (cached render still works) and do NOT trigger this.
#define WIFI_FIRST_CONNECT_TIMEOUT_MS  180000

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

// Timer only arms before the first connect; later drop-outs are soft.
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

  if (!fsManager.ReadThingName(thing_name, MAX_THING_NAME_LENGTH)) {
    ESP_LOGE(TAG, "Thing name not configured — upload 'thing_info' to SPIFFS "
                  "(pio run -e esp32c3_kivsee -t uploadfs). Falling back to STANDALONE.");
    app_mode_switch_to_standalone();   // does not return
    return;
  }
  ESP_LOGI(TAG, "Thing name: %s", thing_name);

  wifi_first_attempt_ms = millis();
  wifi_ever_connected   = false;
  led_engine_set_connecting_blink(true);

  // Width is fixed by the hardware ring; ignore SPIFFS data/num_pixels.
  const uint16_t number_of_leds = (uint16_t)led_engine_num_anim_leds();
  ESP_LOGI(TAG, "Renderer init: %u LEDs", (unsigned)number_of_leds);

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
