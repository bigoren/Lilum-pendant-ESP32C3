#ifndef __BRIGHTNESS_H__
#define __BRIGHTNESS_H__

#include <Arduino.h>

#include "renderer.h"

bool handleGlobalBrightnessMessage(const byte *payload, unsigned int length, float *new_global_brightness);


#endif // __BRIGHTNESS_H__
