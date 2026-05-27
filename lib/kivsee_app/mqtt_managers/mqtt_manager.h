#ifndef MQTT_MANAGER_H_INCLUDED
#define MQTT_MANAGER_H_INCLUDED

#include <fs_manager.h>

class MqttManagerCallbacks {

    public:
        virtual void NewConfigGuidReceived(const byte *payload, unsigned int length, const char *thing_name) = 0;
        virtual void TriggerInvoked(const byte *payload, unsigned int length) = 0;
        virtual void NewGlobalBrightnessReceived(const byte *payload, unsigned int length) = 0;
};

class MqttManager {

public:
    MqttManager() {}

public:
    virtual void connectToMessageBroker(const char * thing_name) = 0;
    virtual bool publish(const char* payload) = 0;
    virtual bool connected() = 0;
    virtual bool loop() = 0;    
};

MqttManager *createMqttManager(MqttManagerCallbacks *callback, FsManager *fsManager);

#endif // MQTT_MANAGER_H_INCLUDED
