#ifndef __MQTT_MANAGERS_MOSQUITTO_H__
#define __MQTT_MANAGERS_MOSQUITTO_H__

#include <mqtt_managers/mqtt_manager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "WiFiClient.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <functional>

static const char *MQTT_TAG = "KIVSEE_MQTT";

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif //MQTT_BROKER_PORT

const char *triggerTopic = "trigger";

class MosquittoManager : public MqttManager
{
public:
    MosquittoManager(MqttManagerCallbacks *callback) : client(net), callback(callback) {}

public:
    void connectToMessageBroker(const char *thing_name) override
    {
        if (client.connected())
            return;

        // MQTT client IDs must be unique per connection, but the thing name in
        // topics is intentionally shared so same-named devices play in sync.
        // Append the chip's MAC suffix so two "ring1"s get distinct client IDs
        // while still subscribing to the same ring1 topics.
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        char clientId[40];
        snprintf(clientId, sizeof(clientId), "%s-%02X%02X", thing_name, mac[4], mac[5]);

        char statusTopic[64];
        snprintf(statusTopic, 64, "thing/%s/status", thing_name);

        StaticJsonDocument<128> alive_will_doc;
        alive_will_doc["thingName"] = thing_name;
        alive_will_doc["alive"] = false;
        char willMsg[128];
        serializeJson(alive_will_doc, willMsg);

        client.setServer(MQTT_BROKER_IP, MQTT_BROKER_PORT);
        client.setCallback(std::bind(&MosquittoManager::mqtt_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, thing_name));
        ESP_LOGI(MQTT_TAG, "connecting to mqtt broker %s:%d as '%s' (thing '%s')",
                 MQTT_BROKER_IP, MQTT_BROKER_PORT, clientId, thing_name);
        if (client.connect(clientId, statusTopic, 1, true, willMsg))
        {
            ESP_LOGI(MQTT_TAG, "connected to message broker");

            // publish alive message
            StaticJsonDocument<128> alive_status_doc;
            alive_status_doc["thingName"] = thing_name;
            alive_status_doc["alive"] = true;
            uint8_t aliveStatusMsg[128];
            size_t aliveStatusMsgSize = serializeJson(alive_status_doc, aliveStatusMsg);
            client.publish(statusTopic, aliveStatusMsg, aliveStatusMsgSize, true);

            client.subscribe((String("animations/") + String(thing_name) + String("/#")).c_str(), 1);
            client.subscribe((String("obj/") + String(thing_name) + String("/guid")).c_str(), 1);
            client.subscribe(triggerTopic, 1);
        }
        else
        {
            ESP_LOGW(MQTT_TAG, "mqtt connect failed. error state: %d", client.state());
        }
    }

    bool publish(const char *payload) override
    {
        return true;
    }

    bool connected() override
    {
        return client.connected();
    }

    bool loop() override
    {
        return client.loop();
    }


private:
    WiFiClient net;
    PubSubClient client;
    MqttManagerCallbacks *callback;

    void mqtt_callback(char *topic, uint8_t* payload, unsigned int length, const char *thing_name)
    {
        // Log topic + payload (truncate payload to fit a reasonable buffer).
        char buf[128];
        unsigned int copyLen = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
        memcpy(buf, payload, copyLen);
        buf[copyLen] = '\0';
        ESP_LOGI(MQTT_TAG, "msg [%s] (%u bytes): %s%s",
                 topic, length, buf, copyLen < length ? "..." : "");

        if(strncmp("obj/", topic, 4) == 0) {
            callback->NewConfigGuidReceived(payload, length, thing_name);
        } else if(strncmp(triggerTopic, topic, sizeof(triggerTopic) + 1) == 0) {
            callback->TriggerInvoked(payload, length);
        } else {
            ESP_LOGW(MQTT_TAG, "unknown topic: %s", topic);
        }
    }
};

#endif // __MQTT_MANAGERS_MOSQUITTO_H__