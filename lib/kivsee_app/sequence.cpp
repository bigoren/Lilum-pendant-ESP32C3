#define ARDUINOJSON_USE_LONG_LONG 1

#include <sequence.h>

#include <HTTPClient.h>
#include <pb_decode.h>
#include <ArduinoJson.h>

#include <animation.h>

#include "secrets.h"
#include "protobuf_infra.h"
#include "segment_store.h"
#include "runtime_animation.h"

SequenceManager::SequenceManager(QueueHandle_t runtime_animation_queue, QueueHandle_t runtime_animation_delete_queue)
        :
    m_runtime_animation_queue(runtime_animation_queue),
    m_runtime_animation_delete_queue(runtime_animation_delete_queue)
{
}

void SequenceManager::loop()
{
    esp32animations::RuntimeAnimation animation_from_del_q;
    while (xQueueReceive(m_runtime_animation_delete_queue, &animation_from_del_q, 0) == pdTRUE)
    {
        deleteRuntimeAnimation(animation_from_del_q.animation);
    }
}

::kivsee_render::Animation *SequenceManager::httpGetSequence(const char *triggerName, uint32_t guid, const char *thing_name)
{
    char uri[128];
    int uriLen = snprintf(uri, sizeof(uri), "/triggers/%s/objects/%s/guid/%lu", triggerName, thing_name, guid);
    if (uriLen < 0 || uriLen >= sizeof(uri))
    {
        Serial.println("cannot format seq uri");
        return nullptr;
    }

    Serial.print(F("fetching sequence from uri: "));
    Serial.println(uri);

    uint16_t port = (uint16_t)strtoul(LED_SEQ_SERVICE_PORT, nullptr, 10);
    if (port == 0)
    {
        Serial.println(F("could not parse sequence service port"));
        return nullptr;
    }

    HTTPClient http;
    http.setTimeout(5000); // Set 5 second timeout to prevent indefinite blocking
    http.begin(LED_SEQ_SERVICE_IP, port, uri);
    http.addHeader("Accept", "application/x-protobuf");

    int httpResponseCode = http.GET();
    if (httpResponseCode <= 0)
    {
        Serial.print(F("sequence service GET error code: "));
        Serial.println(httpResponseCode);
        http.end();
        return nullptr;
    }

    if (httpResponseCode >= 400)
    {
        Serial.println(F("failed to GET led sequence from service"));
        http.end();
        return nullptr;
    }

    int payloadSize = http.getSize();
    if (payloadSize < 0)
    {
        Serial.println(F("failed to GET led sequence payload in http response"));
        http.end();
        return nullptr;
    }

    WiFiClient *httpStream = http.getStreamPtr();
    pb_istream_t nanopbStream = StreamToPbStream(httpStream, payloadSize);

    kivsee_render::DecodeAnimationArgs args = {
        getSegmentsMap()};
    void *decodeArgs = &args;

    uint32_t preDecodeHeapSize = esp_get_free_heap_size();
    Serial.print(F("decoding trigger sequence. heap size: "));
    Serial.println(preDecodeHeapSize);

    bool decodeSuccess = kivsee_render::DecodeAnimationFromPbStream(&nanopbStream, nullptr, &decodeArgs);
    if (!decodeSuccess)
    {
        Serial.print(F("failed to decode sequence proto. error: "));
        Serial.println(nanopbStream.errmsg);
        http.end();
        return nullptr;
    }

    uint32_t heapUsed = preDecodeHeapSize - esp_get_free_heap_size();

    ::kivsee_render::Animation *animation = (::kivsee_render::Animation *)decodeArgs;
    Serial.print(F("successfully decoded sequence from protobuf. found "));
    Serial.print(animation->effects.size());
    Serial.print(F(" effects consuming "));
    Serial.print(heapUsed);
    Serial.println(F(" bytes."));

    http.end();

    return animation;
}

::kivsee_render::Animation *SequenceManager::loadSequence(const char *triggerName, uint32_t guid, const char *thing_name)
{
    // we used to have another option here to read from FS as a fast caching,
    // but the FS write were sooooo slow (~5 seconds)
    // and until the data was saved to FS we did not render.
    // so it was removed in order to not block the starting of new trigger.
    //
    // when we have lots of controllers, this might overload the network / service,
    // which will need to be tested and verified to work properly

    bool sameTrigger = strcmp(m_lastTriggerName.c_str(), triggerName) == 0;
    bool sameGuid = (guid != 0) && (m_lastTriggerGuid == guid);
    if (sameTrigger && sameGuid && m_lastDecodedAnimation)
    {
        Serial.println(F("got the same trigger and guid again"));
        return m_lastDecodedAnimation;
    }

    // before loading new sequence, reclame previous one
    sendEmptyAnimationToRenderer();
    waitForMemoryReclame(2000);

    ::kivsee_render::Animation *animation = this->httpGetSequence(triggerName, guid, thing_name);
    if (animation != nullptr)
    {
        // store last value into the state to return it if needed again
        m_lastTriggerName = triggerName;
        m_lastTriggerGuid = guid;
        m_lastDecodedAnimation = animation;
    }
    Serial.print(F("free heap after http: "));
    Serial.println(esp_get_free_heap_size());
    return animation;
}

bool SequenceManager::waitForMemoryReclame(uint maxMsToWait)
{

    if (!hasCachedValue())
    {
        return true;
    }

    esp32animations::RuntimeAnimation animation_from_del_q;
    TickType_t xTicksToWait = maxMsToWait / portTICK_PERIOD_MS;
    while (hasCachedValue() && xQueueReceive(m_runtime_animation_delete_queue, &animation_from_del_q, xTicksToWait) == pdTRUE)
    {
        deleteRuntimeAnimation(animation_from_del_q.animation);
    }

    return hasCachedValue();
}

void SequenceManager::deleteRuntimeAnimation(kivsee_render::Animation *animationToDelete)
{
    if (animationToDelete == m_lastDecodedAnimation)
    {
        // if we delete this memory, we can no longer use it
        m_lastTriggerName.clear();
        m_lastTriggerGuid = 0;
        m_lastDecodedAnimation = nullptr;
    }
    delete animationToDelete;
}

void SequenceManager::sendEmptyAnimationToRenderer()
{
    esp32animations::RuntimeAnimation new_timed_animation = {
        .animation = nullptr,
        .start_time_ms_since_epoch = 0
    };
    // Use timeout instead of portMAX_DELAY to avoid blocking indefinitely
    xQueueSend(m_runtime_animation_queue, &new_timed_animation, pdMS_TO_TICKS(100));
}

void SequenceManager::handleTriggerInvokedMessage(const byte *payload, unsigned int length, const char *thing_name)
{
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error)
    {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return;
    }

    const char *triggerName = doc["trigger_name"].as<const char *>();
    if (!triggerName)
    {
        Serial.println("no active trigger");
        sendEmptyAnimationToRenderer();
        return;
    }

    uint32_t guid = doc["guid"].as<uint32_t>();
    uint64_t startTimeMsSinceEpoch = doc["start_time_ms_since_epoch"].as<uint64_t>();

    char buf[200];
    snprintf(buf, sizeof(buf), "got trigger: %s. guid: %d, start time: %lld", triggerName ? triggerName : "NONE", guid, startTimeMsSinceEpoch);
    Serial.println(buf);

    ::kivsee_render::Animation *animation = loadSequence(triggerName, guid, thing_name);
    esp32animations::RuntimeAnimation new_timed_animation = {
        .animation = animation,
        .start_time_ms_since_epoch = startTimeMsSinceEpoch
    };
    // Use timeout instead of portMAX_DELAY to avoid blocking indefinitely
    xQueueSend(m_runtime_animation_queue, &new_timed_animation, pdMS_TO_TICKS(100));
}