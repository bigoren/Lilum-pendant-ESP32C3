#ifndef __SEGMENTS_STORE_H__
#define __SEGMENTS_STORE_H__

#include "hsv.h"
#include "Arduino.h"
#include "segments/segments_map.h"

void initSegmentStore(kivsee_render::HSV *leds, uint16_t number_of_leds);
void handleSegmentsGuidMessage(const byte *payload, unsigned int length, const char *thing_name);
void httpGetConfig(const char *thing_name);
uint16_t readNumberOfPixels();
kivsee_render::segments::SegmentsMap *getSegmentsMap();

#endif // __SEGMENTS_STORE_H__