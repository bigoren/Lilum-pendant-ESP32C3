#include "renderer.h"
#include <FastLED.h>
#include "esp_log.h"
#include "led_engine.h"

static const char *TAG = "KIVSEE_RND";

// Standard HSB -> RGB, the same conversion the rings use (NeoPixelBus
// HsbColor -> RgbColor), so a given hue looks the same on both. FastLED's
// CHSV uses its own "rainbow" hue scale, which shifts hues noticeably (e.g. in
// hue-step effects). h is wrapped into [0, 1), so negative hues work too.
static CRGB hsbToRgb(float h, float s, float v)
{
    s = constrain(s, 0.0f, 1.0f);
    v = constrain(v, 0.0f, 1.0f);
    float r, g, b;
    if (s == 0.0f) {
        r = g = b = v;
    } else {
        h = fmodf(h, 1.0f);
        if (h < 0.0f) h += 1.0f;
        h *= 6.0f;
        int sector = (int)h;
        float f = h - sector;
        float p = v * (1.0f - s);
        float q = v * (1.0f - s * f);
        float t = v * (1.0f - s * (1.0f - f));
        switch (sector) {
            case 0:  r = v; g = t; b = p; break;
            case 1:  r = q; g = v; b = p; break;
            case 2:  r = p; g = v; b = t; break;
            case 3:  r = p; g = q; b = v; break;
            case 4:  r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
    }
    return CRGB((uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f), (uint8_t)(b * 255.0f));
}

namespace esp32animations
{

    Renderer::Renderer(const QueueManager &queueManager, uint16_t number_of_leds)
        : m_queueManager(queueManager),
          m_number_of_leds(number_of_leds),
          m_leds_hsv(new kivsee_render::HSV[number_of_leds])
    {
        // led_engine_setup() owns FastLED.addLeds(); show() just fills the buffer.
    }

    void Renderer::loop(unsigned long current_millis)
    {
        readRuntimeAnimationFromQueue();
        readEpochTimeUpdateFromQueue();
        reportMetricsIfNeeded();

        // Idle flag: no animation loaded means no trigger is playing, so the LED
        // engine may use the main LEDs for its connecting indicator.
        led_engine_set_idle_blink(runtime_animation.animation == nullptr);

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
        // Write into the shadow buffer; led_engine_anim_commit() marks it ready
        // so ledTask snaps it atomically at the start of the next LED frame.
        CRGB *out = led_engine_anim_buffer();
        const int hw_max = led_engine_num_anim_leds();
        const int n = (m_number_of_leds < (uint16_t)hw_max) ? m_number_of_leds : hw_max;
        for (int i = 0; i < n; i++)
        {
            const kivsee_render::HSV &hsvVal = m_leds_hsv[i];
            // The Lilum is dimmed solely by its own button-set workingBrightness
            // (applied via FastLED.setBrightness() in ledTask); it deliberately does
            // not honor the MQTT global brightness, which is tuned for the much larger
            // installations and would leave this 27-LED pendant far too dim.
            float normalizedBrightness = hsvVal.val * hsvVal.val;
            out[i] = hsbToRgb(hsvVal.hue, hsvVal.sat, normalizedBrightness);
        }
        led_engine_anim_commit();
    }

    kivsee_render::HSV *Renderer::hsv_painting_array() const
    {
        return m_leds_hsv;
    }

} // namespace esp32animations
