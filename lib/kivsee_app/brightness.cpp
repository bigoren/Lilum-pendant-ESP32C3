#include <brightness.h>

#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>


bool handleGlobalBrightnessMessage(const byte *payload, unsigned int length, float *new_global_brightness)
{
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error)
    {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return false;
    }

    *new_global_brightness = doc["global_brightness"].as<float>();

    if ((*new_global_brightness < 0.0) || (*new_global_brightness > 1.0))
    {
        Serial.print(F("Recieved brightness is outside range 0.0 to 1.0: "));
        Serial.println(*new_global_brightness);
        return false;
    }

    // Serial.print(F("Recieved new global brightness value: "));
    // Serial.println(*new_global_brightness);
    return true;

}