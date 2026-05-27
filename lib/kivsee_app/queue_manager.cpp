#include "queue_manager.h"

#include "runtime_animation.h"
#include "core1_metrics.h"

QueueManager::QueueManager() : runtime_animation_queue(xQueueCreate(5, sizeof(esp32animations::RuntimeAnimation))),
                               epoch_time_update_queue(xQueueCreate(5, sizeof(int64_t))),
                               global_brightness_queue(xQueueCreate(5, sizeof(float))),
                               runtime_animation_delete_queue(xQueueCreate(5, sizeof(esp32animations::RuntimeAnimation))),
                               core1_metrics_queue(xQueueCreate(5, sizeof(esp32animations::Core1Metrics))),
                               trigger_queue(xQueueCreate(5, sizeof(QueueManager::TriggerMessage)))
{
}
