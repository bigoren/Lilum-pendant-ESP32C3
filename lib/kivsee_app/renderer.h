#ifndef __RENDERER_H__
#define __RENDERER_H__

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "animation.h"
#include "runtime_animation.h"
#include "hsv.h"
#include "queue_manager.h"
#include "core1_metrics.h"

namespace esp32animations
{

    /*
    All the rendering stuff.
    Runs in the main loop on ESP32-C3 (single core).
    Interactions with other components are via queues.
    */
    class Renderer
    {
    public:
        Renderer(const QueueManager &queueManager, uint16_t number_of_leds);
        void loop(unsigned long current_millis);
        kivsee_render::HSV *hsv_painting_array() const;

    private:
        void clear();
        void show();

    private:
        const QueueManager m_queueManager;

    private:
        void readRuntimeAnimationFromQueue();
        void readEpochTimeUpdateFromQueue();
        void readGlobalBrightnessFromQueue();
        void reportMetricsIfNeeded();
        void updateAnimationEspStartTime(RuntimeAnimation *runtime_animation);
        unsigned long getAnimationTime(unsigned long current_millis, const RuntimeAnimation &runtime_animation);

    private:
        RuntimeAnimation runtime_animation = {
            .animation = nullptr,
            .start_time_ms_since_epoch = 0
        };
        int64_t esp_start_time = 0;

    private:
        uint16_t m_number_of_leds;
        kivsee_render::HSV *m_leds_hsv;
        // The output buffer is FastLED's shared CRGB[] inside led_engine; we
        // only fill it here. The FastLED ledTask calls FastLED.show() on its
        // 16 ms tick (single owner of the RMT peripheral).
        float m_global_brightness;
        unsigned long m_last_metrics_report_time = 0;
        Core1Metrics m_metrics = {
            .totalFrames = 0,
            .maxFrameRenderTime = 0,
            .numEffectsRendered = 0,
        };
    };

} // namespace esp32animations

#endif // __RENDERER_H__