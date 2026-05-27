#ifndef __RUNTIME_ANIMATION_H__
#define __RUNTIME_ANIMATION_H__

#include "animation.h"

namespace esp32animations
{

    struct RuntimeAnimation
    {
        // the animation ptr holds a pointer to an animations object
        // which has all the effects to render with their configuration.
        // it is created and deleted in the main loop and consumed by the renderer
        kivsee_render::Animation *animation;

        // this is the ephoc time at which animation started.
        // it is a very large number (ms since 1970)
        // and is combined with esp epoch start time and current millis
        // to derive song offset time in millis
        uint64_t start_time_ms_since_epoch;
    };

}

#endif // __RUNTIME_ANIMATION_H__