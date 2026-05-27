#include "fs_manager.h"

#include "SPIFFS.h"

#define THING_FILE_NAME "thing_info"

bool FsManager::setup() {
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return false;
  }

  return true;
}

bool FsManager::ReadThingKey(char *destBuffer, int bufferLength) {
    File file = SPIFFS.open("/" THING_FILE_NAME, FILE_READ);
    if(!file)
    {
      Serial.print("could not find thing name file at "); Serial.println(THING_FILE_NAME);
      return false;
    }

    // size_t numOfChars = file.read((uint8_t *)destBuffer, bufferLength - 1);
    String unused=file.readStringUntil('\n');
    size_t numOfChars = file.readBytesUntil('\n', (uint8_t *)destBuffer, bufferLength - 1);
    if(numOfChars == 0)
    {
      Serial.println("thing name file is empty");
      file.close();
      return false;
    }

    // make sure the file is '\0' terminated
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
      Serial.print("could not find thing name file at "); Serial.println(THING_FILE_NAME);
      return false;
    }

    // size_t numOfChars = file.read((uint8_t *)destBuffer, bufferLength - 1);
    size_t numOfChars = file.readBytesUntil('\n', (uint8_t *)destBuffer, bufferLength - 1);
    if(numOfChars == 0)
    {
      Serial.println("thing name file is empty");
      file.close();
      return false;
    }

    // make sure the file is '\0' terminated
    destBuffer[numOfChars] = '\0';
    destBuffer[bufferLength - 1] = '\0';

    file.close();
    return true;
}

bool FsManager::SaveToFs(const char *path, const uint8_t *payload, unsigned int length) {
    Serial.println("writing file to FS");
    File file = SPIFFS.open(path, FILE_WRITE);
    if (!file) {
      Serial.println("There was an error opening the file for writing");
      return false;
    }

    size_t bytesWritten = file.write(payload, length);
    if(bytesWritten != length) {
      Serial.println("did not write all bytes to file");
      return false;
    }
    file.close();
    return true;
}

unsigned int FsManager::ReadFromFs(const char *path, uint8_t *buffer, unsigned int length) {
    File file = SPIFFS.open(path);
    if(!file){
        Serial.println("Failed to open file for reading");
        return 0;
    }
    unsigned int bytesRead = file.read(buffer, length);
    file.close();
    return bytesRead;
}