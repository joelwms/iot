#pragma once
#include <time.h>

bool getHourMinute(int *hh, int *mm);
bool initializeTime();

bool getTimestamp(time_t *timestamp);
bool timestampToString(time_t timestamp, char *buffer, size_t bufferSize);