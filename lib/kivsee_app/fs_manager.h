#ifndef __FS_MANAGER_H__
#define __FS_MANAGER_H__

#include <stdint.h>

class FsManager 
{

public:
    bool setup();

public:
    bool ReadThingName(char *destBuffer, int bufferLength);
    bool ReadThingKey(char *destBuffer, int bufferLength);
    bool SaveToFs(const char *path, const uint8_t *payload, unsigned int length);
    unsigned int ReadFromFs(const char *path, uint8_t *buffer, unsigned int length);
};

#endif // __FS_MANAGER_H__