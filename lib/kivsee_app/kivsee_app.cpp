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

  bool hasThingName = fsManager.ReadThingName(thing_name, MAX_THING_NAME_LENGTH);
  while (!hasThingName)
  {
    strcpy(thing_name, "no name");
    ESP_LOGW(TAG, "Thing name not configured — upload 'thing_info' to SPIFFS (pio run -e esp32c3_kivsee -t uploadfs)");
    delay(5000);
    hasThingName = fsManager.ReadThingName(thing_name, MAX_THING_NAME_LENGTH);
  }
  ESP_LOGI(TAG, "Thing name: %s", thing_name);

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
