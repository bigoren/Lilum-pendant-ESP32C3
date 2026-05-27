// Kivsee networked-animation app implementation (esp32c3_kivsee variant only).
// Adapted from the esp32-animations project's main.cpp setup()/loop(), with the
// Serial/SPIFFS bring-up that the Lilum boot prefix already handles removed, and
// Influxdb metrics reporting omitted (no-op on C3).

#include "kivsee_app.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <SPIFFS.h>
#include <cstring>

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
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(SSID);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("connected to wifi");
    connecting = false;
    httpGetConfig(thing_name);
    return;
  }

  if (millis() - connectStartTime >= 10000)
  {
    Serial.println(" could not connect for 10 seconds. retry");
    connecting = false;
  }
}

void kivsee_app_setup(void)
{
  // SPIFFS is mounted here (the Lilum boot prefix does not mount it).
  if (!SPIFFS.begin(true))
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  bool hasThingName = fsManager.ReadThingName(thing_name, MAX_THING_NAME_LENGTH);
  while (!hasThingName)
  {
    strcpy(thing_name, "no name");
    Serial.println("Thing name not configured - upload 'thing_info' file to continue");
    delay(5000);
    hasThingName = fsManager.ReadThingName(thing_name, MAX_THING_NAME_LENGTH);
  }
  Serial.print("Thing name: "); Serial.println(thing_name);

  uint16_t number_of_leds = readNumberOfPixels();
  if (number_of_leds == 0) {
    number_of_leds = 300;
  }

  renderer = new esp32animations::Renderer(queueManager, number_of_leds);
  initSegmentStore(renderer->hsv_painting_array(), number_of_leds);
  fsManager.setup();

  mqttManager = createMqttManager(&mqttCallbacks, &fsManager);

  ConnectToWifi();
  ArduinoOTA.setHostname(thing_name);
  ArduinoOTA
      .onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        Serial.println("Start updating " + type);
      })
      .onEnd([]() { Serial.println("\nEnd"); })
      .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      })
      .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)         Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)     Serial.println("End Failed");
      });

  ArduinoOTA.begin();
  timeManager.begin();
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
    Serial.printf("[kivsee] millis=%lu wifi=%d mqtt=%d rssi=%ld\n",
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
