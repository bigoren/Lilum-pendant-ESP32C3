#ifndef __QUEUE_MANAGER_H__
#define __QUEUE_MANAGER_H__

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class QueueManager {

public:
    QueueManager();

public:

    // queue for updating on a new runtime animation to execute in renderer
    QueueHandle_t runtime_animation_queue;

    // queue for updating on esp time synchronization event.
    // the payload is int64_t representing the epoch time of the esp start, e.g.
    // when esp millis() returned 0, this was the epoch time
    QueueHandle_t epoch_time_update_queue;

    // queue for renderer to signal that it is done with this runtime animation
    // and it can be disposed of (release memory, invalidate cache etc)
    QueueHandle_t runtime_animation_delete_queue;

    // queue for periodically sending metrics about rendering for reporting
    // Note: Originally named core1_metrics_queue for dual-core ESP32, kept for compatibility
    QueueHandle_t core1_metrics_queue;

    // queue for trigger messages received via MQTT. Payload is copied into
    // TriggerMessage and handled from the main loop to avoid blocking MQTT callbacks
    typedef struct {
        uint16_t length;
        uint8_t payload[512];
    } TriggerMessage;

    QueueHandle_t trigger_queue;

};

#endif //__QUEUE_MANAGER_H__