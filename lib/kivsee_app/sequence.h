#ifndef __SEQUENCE_H__
#define __SEQUENCE_H__

#include <Arduino.h>

#include "animation.h"

class SequenceManager {

    public:
        SequenceManager(QueueHandle_t runtime_animation_queue, QueueHandle_t runtime_animation_delete_queue);
        void loop();
        void handleTriggerInvokedMessage(const byte *payload, unsigned int length, const char *thing_name);
        
    private:
        ::kivsee_render::Animation *loadSequence(const char *triggerName, uint32_t guid, const char *thing_name);
        ::kivsee_render::Animation *httpGetSequence(const char *triggerName, uint32_t guid, const char *thing_name);
        void deleteRuntimeAnimation(kivsee_render::Animation *animationToDelete);
        bool hasCachedValue() { return m_lastDecodedAnimation != nullptr; }
        bool waitForMemoryReclame(uint maxMsToWait);
        void sendEmptyAnimationToRenderer();

    private:

        QueueHandle_t m_runtime_animation_queue;
        QueueHandle_t m_runtime_animation_delete_queue;

        // cache the last values we received.
        // if we get the same message, return it from cache instead of allocating again on heap
        String m_lastTriggerName;
        uint32_t m_lastTriggerGuid;
        ::kivsee_render::Animation *m_lastDecodedAnimation = nullptr;
};

#endif // __SEQUENCE_H__
