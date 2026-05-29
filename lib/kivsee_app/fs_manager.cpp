#include "fs_manager.h"

#include "SPIFFS.h"
#include "esp_log.h"

#define THING_FILE_NAME "thing_info"

static const char *TAG = "KIVSEE_FS";

bool FsManager::setup() {
  if (!SPIFFS.begin(true)) {
    ESP_LOGE(TAG, "SPIFFS mount failed");
    return false;
  }

  return true;
}

bool FsManager::ReadThingKey(char *destBuffer, int bufferLength) {
    File file = SPIFFS.open("/" THING_FILE_NAME, FILE_READ);
    if(!file)
    {
      ESP_LOGE(TAG, "thing name file not found: /%s", THING_FILE_NAME);
      return false;
    }

    String unused=file.readStringUntil('\n');
    size_t numOfChars = file.readBytesUntil('\n', (uint8_t *)destBuffer, bufferLength - 1);
    if(numOfChars == 0)
    {
      ESP_LOGE(TAG, "thing name file is empty");
      file.close();
      return false;
    }

    destBuffer[numOfChars] = '\0';
    destBuffer[bufferLength - 1] = '\0';

    file.close();
    return true;
}

bool FsManager::ReadThingName(char *destBuffer, int bufferLength)
{
    File file = SPIFFS.open("/" THING_FILE_NAME, FILE_READ);
    if(!file)
    {
      ESP_LOGE(TAG, "thing name file not found: /%s", THING_FILE_NAME);
      return false;
    }

    size_t numOfChars = file.readBytesUntil('\n', (uint8_t *)destBuffer, bufferLength - 1);
    if(numOfChars == 0)
    {
      ESP_LOGE(TAG, "thing name file is empty");
      file.close();
      return false;
    }

    destBuffer[numOfChars] = '\0';
    destBuffer[bufferLength - 1] = '\0';

    file.close();
    return true;
}

bool FsManager::SaveToFs(const char *path, const uint8_t *payload, unsigned int length) {
    ESP_LOGI(TAG, "writing file: %s (%u bytes)", path, length);
    File file = SPIFFS.open(path, FILE_WRITE);
    if (!file) {
      ESP_LOGE(TAG, "could not open %s for writing", path);
      return false;
    }

    size_t bytesWritten = file.write(payload, length);
    if(bytesWritten != length) {
      ESP_LOGE(TAG, "short write to %s: wrote %u of %u", path, (unsigned)bytesWritten, length);
      return false;
    }
    file.close();
    return true;
}

unsigned int FsManager::ReadFromFs(const char *path, uint8_t *buffer, unsigned int length) {
    File file = SPIFFS.open(path);
    if(!file){
        ESP_LOGE(TAG, "could not open %s for reading", path);
        return 0;
    }
    unsigned int bytesRead = file.read(buffer, length);
    file.close();
    return bytesRead;
}