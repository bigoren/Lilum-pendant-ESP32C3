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
#include "esp_wifi.h"
#include "led_engine.h"
#include "app_mode.h"
#include "battery_service.h"

static const char *TAG = "KIVSEE";

#include "secrets.h"
#include "segment_store.h"
#include "sequence.h"
#include "renderer.h"
#include "mqtt_managers/mqtt_manager.h"
#include "fs_manager.h"
#include "time_manager.h"
#include "queue_manager.h"

// MQTT failures are soft (cached render still works) and do NOT trigger this.
#define WIFI_FIRST_CONNECT_TIMEOUT_MS  180000

// Keep-on stops the IP5306 idle auto-off but also blocks its double-press off,
// so power-off is done by soft off below. false = plain IP5306 behaviour.
#define KIVSEE_BOOST_KEEP_ON  true

// Soft off: on a double-click, go dark, stop WiFi and drop keep-on so the
// IP5306 light-load shutdown powers off for real (one click then turns it on).
// A click while waiting cancels; presses in the grace window are the off
// gesture itself.
#define SOFT_OFF_GRACE_MS     1500
#define SOFT_OFF_LOG_MS       10000
static bool          soft_off          = false;
static unsigned long soft_off_start_ms = 0;
static unsigned long soft_off_log_ms   = 0;
static uint32_t      soft_off_presses  = 0;

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
    ESP_LOGI(TAG, "TriggerInvoked: %u bytes queued for processing", length);
    // Copy into the trigger queue; the HTTP/decode work happens in the loop,
    // not inside the MQTT callback (which would block the mqtt loop).
    QueueManager::TriggerMessage msg;
    if (length > sizeof(msg.payload)) length = sizeof(msg.payload);
    msg.length = (uint16_t)length;
    memcpy(msg.payload, payload, msg.length);
    xQueueSend(queueManager.trigger_queue, &msg, 0);
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

  if (!fsManager.ReadThingName(thing_name, MAX_THING_NAME_LENGTH)) {
    ESP_LOGE(TAG, "Thing name not configured — upload 'thing_info' to SPIFFS "
                  "(pio run -e esp32c3_kivsee -t uploadfs). Falling back to STANDALONE.");
    app_mode_switch_to_standalone();   // does not return
    return;
  }
  ESP_LOGI(TAG, "Thing name: %s", thing_name);

  // Idle kivsee draws too little to keep the IP5306 awake; keep-on stops the
  // auto-off. Set explicitly either way: the register survives a restart.
  battery_set_boost_keep_on(KIVSEE_BOOST_KEEP_ON);
  battery_set_key_soft_off(true);
  // Off the charger a kivsee pendant keeps running (it reboots onto battery);
  // standalone sets this to off, and an in-place switch arrives from there.
  battery_set_boost_on_unplug(true);

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

// dBm thresholds: >=-55 excellent, -55..-67 good, -67..-75 fair, <-75 poor.
static const char *rssi_quality(long rssi)
{
  if (rssi >= -55) return "excellent";
  if (rssi >= -67) return "good";
  if (rssi >= -75) return "fair";
  return "poor";
}

static void soft_off_enter(unsigned long now)
{
  ESP_LOGW(TAG, "Soft off: LEDs + WiFi off, keep-on dropped; waiting for IP5306 "
                "shutdown (click to cancel)");
  soft_off          = true;
  soft_off_start_ms = now;
  soft_off_log_ms   = now;
  soft_off_presses  = battery_key_press_count();
  led_engine_set_soft_off(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  battery_set_light_load_fast(true);
  battery_set_boost_keep_on(false);
}

static void soft_off_cancel(unsigned long now)
{
  ESP_LOGW(TAG, "Soft off cancelled after %lu ms — restoring",
           now - soft_off_start_ms);
  soft_off = false;
  battery_set_boost_keep_on(KIVSEE_BOOST_KEEP_ON);
  battery_set_light_load_fast(false);
  battery_key_off_request();            // drop any stale request
  led_engine_set_soft_off(false);
  if (!wifi_ever_connected) wifi_first_attempt_ms = now;  // restart the fallback timer
  lastWiFiCheckTime = now - 10000;      // reconnect on this loop pass
}

// Returns true while soft off owns the loop.
static bool soft_off_loop(unsigned long now)
{
  if (!soft_off) {
    if (battery_key_off_request()) soft_off_enter(now);
    return soft_off;
  }

  uint32_t presses = battery_key_press_count();
  if (now - soft_off_start_ms < SOFT_OFF_GRACE_MS) {
    soft_off_presses = presses;         // still part of the off gesture
  } else if (presses != soft_off_presses) {
    soft_off_cancel(now);
    return false;
  }

  if (now - soft_off_log_ms >= SOFT_OFF_LOG_MS) {
    soft_off_log_ms = now;
    ESP_LOGI(TAG, "Soft off: still powered after %lu s (USB connected?)",
             (now - soft_off_start_ms) / 1000);
  }
  return true;
}

void kivsee_app_loop(void)
{
  unsigned long current_millis = millis();

  if (soft_off_loop(current_millis)) {
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }

  // Latch wifi_ever_connected the moment status flips — independent of the
  // 10 s ConnectToWifi cadence and any MQTT-bringup blocking inside it.
  bool wifi_connected = (WiFi.status() == WL_CONNECTED);
  if (wifi_connected && !wifi_ever_connected) {
    wifi_ever_connected = true;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi connected  IP=%s  GW=%s  DNS=%s",
             WiFi.localIP().toString().c_str(),
             WiFi.gatewayIP().toString().c_str(),
             WiFi.dnsIP().toString().c_str());
  }

  // Keep the blue connecting-blink running until MQTT is up, not just WiFi —
  // a WiFi link without a broker connection still can't receive triggers.
  led_engine_set_connecting_blink(!mqttManager->connected());

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
    unsigned long s = current_millis / 1000;
    unsigned h = s / 3600;
    unsigned m = (s / 60) % 60;
    unsigned sec = s % 60;
    long rssi = (long)WiFi.RSSI();
    ESP_LOGI(TAG, "uptime=%02u:%02u:%02u cpu=%uMHz wifi=%s mqtt=%s signal=%s",
             h, m, sec, (unsigned)getCpuFrequencyMhz(),
             wifi_connected ? "con" : "uncon",
             mqttManager->connected() ? "con" : "uncon",
             wifi_connected ? rssi_quality(rssi) : "n/a");
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
