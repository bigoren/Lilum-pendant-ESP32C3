
#include <time_manager.h>

#include <Arduino.h>

TimeManager::TimeManager(QueueHandle_t epoch_time_update_queue)
    : m_epoch_time_update_queue(epoch_time_update_queue)
{
    m_timesync.updateConfiguration(15, 1000 * 60 * 10, 250, 1000 * 60 * 2);
}

void TimeManager::begin() {
    IPAddress ntpServerIp;
    Serial.print("Time sync server IP: ");
    Serial.println(TIME_SERVER_IP);
    ntpServerIp.fromString(TIME_SERVER_IP);
    m_timesync.setup(ntpServerIp, 12321);
}

void TimeManager::loop()
{
    bool isTimeChanged, isFirstClockUpdate;
    m_timesync.loop(&isTimeChanged, &isFirstClockUpdate);
    if (isFirstClockUpdate)
    {
        Serial.println("TIME IS NOW VALID. the esp clock was not valid and now it is");
    }
    else if (isTimeChanged)
    {
        Serial.println("TIME CHANGED. new synced clock is available to the esp");
    }

    if (isTimeChanged || isFirstClockUpdate)
    {
        // when esp millis clock showed 0, this was the epoch time in ms
        int64_t espStartTime = m_timesync.getEspStartTimeMs();
        // Use timeout instead of portMAX_DELAY to avoid blocking indefinitely
        xQueueSend(m_epoch_time_update_queue, &espStartTime, pdMS_TO_TICKS(100));
    }
}
