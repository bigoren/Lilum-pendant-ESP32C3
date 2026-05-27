#ifndef __CORE1_METRICS_H__
#define __CORE1_METRICS_H__

#ifndef METRICS_REPORT_INTERVAL_MS
#define METRICS_REPORT_INTERVAL_MS 5000
#endif // METRICS_REPORT_INTERVAL_MS

namespace esp32animations
{
    struct Core1Metrics
    {
        unsigned int totalFrames;
        unsigned long maxFrameRenderTime;
        unsigned int numEffectsRendered;
    };

}

#endif // __CORE1_METRICS_H__