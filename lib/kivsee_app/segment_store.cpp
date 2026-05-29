
#include "segment_store.h"
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_log.h"
#include "protobuf_infra.h"
#include "hsv.h"
#include <kivsee/proto/render/v1/segments.pb.h>
#include "secrets.h"

static const char *TAG = "KIVSEE_SEG";

#ifndef LED_OBJECT_SERVICE_PORT
#define LED_OBJECT_SERVICE_PORT 80
#endif //LED_OBJECT_SERVICE_PORT

const char *objectFileName = "/objects-config";

// data structures to use during segments map construction
kivsee_render::segments::SegmentsMap *segments_map = nullptr;

kivsee_render::segments::SegmentsMap *initDefaultSegmentStore(kivsee_render::HSV *leds, uint16_t number_of_leds)
{
    kivsee_render::segments::SegmentsMap *segmentsStore = new kivsee_render::segments::SegmentsMap();
    // kivsee_render::segments::Segment segment;
    // strncpy(segment.first, "all", 4);
    // for(int i=0; i<number_of_leds; i++) {
    //     segment.second.push_back(&leds[i]);
    // }
    // segmentsStore->segments.push_back(segment);
    return segmentsStore;
}

void initSegmentStore(kivsee_render::HSV *leds, uint16_t number_of_leds)
{
    File file = SPIFFS.open(objectFileName, "r");
    if (!file || file.available() == 0)
    {
        segments_map = initDefaultSegmentStore(leds, number_of_leds);
        ESP_LOGW(TAG, "no objects-config on FS; using empty default segment store");
        return;
    }

    pb_istream_t pbInputStream = FileToPbStream(file);
    ::kivsee_render::segments::SegmentsMapDecodeArgs segments_map_decode_args;
    segments_map_decode_args.out_segments_map = &segments_map;
    segments_map_decode_args.leds_array = leds;
    void *arg = &segments_map_decode_args;

    // decode
    bool decodeSuccess = ::kivsee_render::segments::DecodeSegmentsMapFromPbStream(&pbInputStream, nullptr, &arg);
    if (decodeSuccess)
    {
        ESP_LOGI(TAG, "segment store initialized: guid=%u pixels=%u segments=%u",
                 (unsigned)segments_map->guid,
                 (unsigned)segments_map->number_of_pixels,
                 (unsigned)segments_map->segments.size());
    }
    else
    {
        ESP_LOGE(TAG, "failed to decode segment store: %s",
                 pbInputStream.errmsg ? pbInputStream.errmsg : "(none)");
        segments_map = initDefaultSegmentStore(leds, number_of_leds);
    }
    file.close();
}

void handleSegmentsGuidMessage(const byte *payload, unsigned int length, const char *thing_name)
{
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error)
    {
        ESP_LOGE(TAG, "deserializeJson() failed: %s", error.f_str());
        return;
    }

    uint32_t currentGuid = doc["guid"].as<uint32_t>();
    if (currentGuid != segments_map->guid)
    {
        ESP_LOGI(TAG, "config guid changed (%u -> %u); refetching",
                 (unsigned)segments_map->guid, (unsigned)currentGuid);
        httpGetConfig(thing_name);
    }
}

void httpGetConfig(const char *thing_name)
{
    char uri[32];
    int uriLen = snprintf(uri, sizeof(uri), "/thing/%s", thing_name);
    if (uriLen < 0 || uriLen >= sizeof(uri))
    {
        ESP_LOGE(TAG, "cannot format led object uri");
        return;
    }

    uint16_t port = (uint16_t)strtoul(LED_OBJECT_SERVICE_PORT, nullptr, 10);
    if(port == 0) {
        ESP_LOGE(TAG, "could not parse object service port");
        return;
    }

    HTTPClient http;
    http.setTimeout(5000); // Set 5 second timeout to prevent indefinite blocking
    http.begin(LED_OBJECT_SERVICE_IP, port, uri);
    http.addHeader("Accept", "application/x-protobuf");
    if (segments_map)
    {
        http.addHeader("If-None-Match", String(segments_map->guid));
    }

    int httpResponseCode = http.GET();
    if (httpResponseCode <= 0)
    {
        ESP_LOGE(TAG, "object service GET error code: %d", httpResponseCode);
        http.end();
        return;
    }
    if (httpResponseCode == 304)
    {
        ESP_LOGI(TAG, "object config is current (304), no update needed");
        http.end();
        return;
    }
    if (httpResponseCode >= 400)
    {
        ESP_LOGE(TAG, "failed to GET object config from service (HTTP %d)", httpResponseCode);
        http.end();
        return;
    }

    File file = SPIFFS.open(objectFileName, FILE_WRITE);
    if (!file)
    {
        ESP_LOGE(TAG, "could not open objects-config for writing");
        http.end();
        return;
    }

    int bytesWritten = http.writeToStream(&file);
    if (bytesWritten < 0 || bytesWritten != http.getSize())
    {
        ESP_LOGE(TAG, "did not write all bytes (wrote=%d, expected=%d)",
                 bytesWritten, http.getSize());
        file.close();
        http.end();
        return;
    }

    file.close();
    http.end();
    ESP_LOGI(TAG, "object config updated on FS, restarting...");
    ESP.restart();
}

kivsee_render::segments::SegmentsMap *getSegmentsMap() {
    return segments_map;
}

uint16_t readNumberOfPixels() {
    File file = SPIFFS.open(objectFileName, "r");
    if (!file || file.available() == 0)
    {
        ESP_LOGW(TAG, "no objects-config file (readNumberOfPixels)");
        return 0;
    }

    pb_istream_t pbInputStream = FileToPbStream(file);

    uint16_t number_of_pixels = ::kivsee_render::segments::GetNumberOfPixels(&pbInputStream, nullptr, nullptr);
    if (number_of_pixels == 0)
    {
        ESP_LOGW(TAG, "failed to read number of pixels from config");
        return 0;
    }

    ESP_LOGI(TAG, "read number of pixels from config: %u", (unsigned)number_of_pixels);
    file.close();
    return number_of_pixels;
}