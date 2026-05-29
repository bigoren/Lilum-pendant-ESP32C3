#include "renderer.h"
#include <FastLED.h>
#include "esp_log.h"
#include "led_engine.h"

static const char *TAG = "KIVSEE_RND";

namespace esp32animations
{

    Renderer::Renderer(const QueueManager &queueManager, uint16_t number_of_leds)
        : m_queueManager(queueManager),
          m_number_of_leds(number_of_leds),
          m_leds_hsv(new kivsee_render::HSV[number_of_leds]),
          m_global_brightness(1.0)
    {
        // led_engine_setup() owns FastLED.addLeds(); show() just fills the buffer.
    }

    void Renderer::loop(unsigned long current_millis)
    {
        readRuntimeAnimationFromQueue();
        readEpochTimeUpdateFromQueue();
        readGlobalBrightnessFromQueue();
        reportMetricsIfNeeded();

        clear();
        if (runtime_animation.animation != nullptr)
        {
            unsigned long current_animation_time = getAnimationTime(current_millis, runtime_animation);
            if (current_animation_time)
            {
                unsigned long start_render_time = millis();
                kivsee_render::RenderStats renderStats = runtime_animation.animation->Render(current_animation_time);
                unsigned long render_time = millis() - start_render_time;

                // update metrics for current frame rendering
                m_metrics.numEffectsRendered = renderStats.num_effects_rendered;
                if (render_time > m_metrics.maxFrameRenderTime)
                {
                    m_metrics.maxFrameRenderTime = render_time;
                }
            }
        }
        else
        {
            m_metrics.numEffectsRendered = 0;
        }
        show();
        m_metrics.totalFrames++;
    }

    void Renderer::readRuntimeAnimationFromQueue()
    {
        RuntimeAnimation new_runtime_animation;
        if (xQueueReceive(m_queueManager.runtime_animation_queue, &new_runtime_animation, 0) == pdTRUE)
        {
            ESP_LOGI(TAG, "received new animation with %u effects",
                     (unsigned)(new_runtime_animation.animation ? new_runtime_animation.animation->effects.size() : 0));
            const bool animationChanged = runtime_animation.animation != new_runtime_animation.animation;
            if (animationChanged)
            {
                xQueueSend(m_queueManager.runtime_animation_delete_queue, &runtime_animation, 0);
            }
            runtime_animation = new_runtime_animation;
        }
    }

    void Renderer::readEpochTimeUpdateFromQueue()
    {
        // unblocking consume all from queue and update value into esp_start_time.
        // finish when no more things in the queue.
        while (xQueueReceive(m_queueManager.epoch_time_update_queue, &esp_start_time, 0) == pdTRUE)
            ;
    }

    void Renderer::readGlobalBrightnessFromQueue()
    {
        while (xQueueReceive(m_queueManager.global_brightness_queue, &m_global_brightness, 0) == pdTRUE)
            ;
    }

    void Renderer::reportMetricsIfNeeded()
    {
        if (millis() - m_last_metrics_report_time < METRICS_REPORT_INTERVAL_MS)
        {
            return;
        }

        m_last_metrics_report_time = millis();
        xQueueSend(m_queueManager.core1_metrics_queue, &m_metrics, 0);
        m_metrics.maxFrameRenderTime = 0;
    }

    // returns the relative time, in ms, of the current rendered animation.
    // 0 means its just started, 1000 means it started 1 second ago
    unsigned long Renderer::getAnimationTime(unsigned long current_millis, const RuntimeAnimation &runtime_animation)
    {
        if (!esp_start_time || !runtime_animation.start_time_ms_since_epoch)
            return 0;

        // this is the time in esp millis in which the animation started
        unsigned long animation_start_time_esp_millis = (unsigned long)(runtime_animation.start_time_ms_since_epoch - esp_start_time);

        return current_millis - animation_start_time_esp_millis;
    }

    void Renderer::clear()
    {
        memset(m_leds_hsv, 0, sizeof(kivsee_render::HSV) * m_number_of_leds);
    }

    void Renderer::show()
    {
        // Fill led_engine's shared buffer; the ledTask calls FastLED.show().
        CRGB *out = led_engine_anim_buffer();
        const int hw_max = led_engine_num_anim_leds();
        const int n = (m_number_of_leds < (uint16_t)hw_max) ? m_number_of_leds : hw_max;
        for (int i = 0; i < n; i++)
        {
            const kivsee_render::HSV &hsvVal = m_leds_hsv[i];
            float normalizedBrightness = hsvVal.val * hsvVal.val * m_global_brightness;
            uint8_t h = (uint8_t)(fmodf(hsvVal.hue, 1.0f) * 255.0f);
            uint8_t s = (uint8_t)(hsvVal.sat * 255.0f);
            uint8_t v = (uint8_t)(normalizedBrightness * 255.0f);
            out[i] = CHSV(h, s, v);
        }
    }

    kivsee_render::HSV *Renderer::hsv_painting_array() const
    {
        return m_leds_hsv;
    }

} // namespace esp32animations
