#include <Arduino.h>
#include <mqtt_managers/mqtt_manager.h>

#include <mqtt_managers/mosquitto_mqtt/mosquitto_manager.h>

MqttManager *createMqttManager(MqttManagerCallbacks *callback, FsManager *fsManager)
{
    return new MosquittoManager(callback);
}
