#include "syncTime.hpp"
#include "log_serial.hpp" // para logging
#include <time.h>
#include <Arduino.h>
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
// #include "esp_sntp.h"
// #include "esp_netif_sntp.h"
#include <inttypes.h>

// Epoch mínima (1 Ene 2020) para considerar hora válida
constexpr time_t kValidEpochThreshold = 1577836800;

// Callback para cuando SNTP obtiene tiempo - No se usa
void time_sync_notification_cb(struct timeval *tv)
{
  LOGI("SNTP time sync callback triggered!");
  LOGI("Received timestamp: %" PRId64, (int64_t)tv->tv_sec);
  LOGD("Callback finished successfully");
}

// bool initializeTime() {
//   LOGI("Starting time synchronization initialization");

//   // 1 servidor (lo más simple)
//   LOGD("Configuring SNTP with single server (es.pool.ntp.org)");
//   esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("es.pool.ntp.org");
//   // esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("216.239.35.12"); // time.google.com
//   cfg.sync_cb = time_sync_notification_cb;      // opcional
//   // cfg.smooth_sync = true;                    // si quieres modo SMOOTH

//   LOGD("Initializing SNTP (netif helper)...");
//   esp_err_t err = esp_netif_sntp_init(&cfg);
//   if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
//     LOGE("esp_netif_sntp_init failed: %d", (int)err);
//     return false;
//   }

//   // IMPORTANTE: arrancar el servicio
//   err = esp_netif_sntp_start();
//   if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
//     LOGE("esp_netif_sntp_start failed: %d", (int)err);
//     return false;
//   }

//   LOGI("SNTP service initialized, waiting for synchronization (8000ms timeout)");
//   // Espera de forma bloqueante (opcional)
//   if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000)) == ESP_OK) {
//     time_t now = time(nullptr);
//     LOGI("Synced, epoch=%" PRId64, (int64_t)now);
//   } else {
//     LOGE("SNTP synchronization failed or timed out");
//   }

//   LOGD("Time initialization process finished");
//   return true;
// }

bool isTimeValid()
{
  time_t t = time(nullptr);
  return t >= kValidEpochThreshold;
}

bool initializeTime()
{

  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org");

  time_t now = time(NULL); // Epoch actual
  // struct tm* timeinfo = localtime(&now);  // Conversión a hora local

  int count = 0;

  do
  {
    LOGD("Esperando hora...");
    // delay(1000);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // now = time(NULL);
    count++;
    if (count > 100)
    {
      break;
    }
    // timeinfo = localtime(&now);
  } while (!isTimeValid()); // Espera hasta que el reloj sea razonable

  if (!isTimeValid())
  {
    LOGD("Invalid time");
    return false;
  }

  LOGD("Time recieved!");
  return true;
}

bool getHourMinute(int *hh, int *mm)
{
  if (!hh || !mm)
    return false;
  if (!isTimeValid())
    return false;
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  *hh = lt.tm_hour;
  *mm = lt.tm_min;
  return true;
}

// getTimestamp devuelve el timestamp actual
bool getTimestamp(time_t *timestamp)
{
  if (!timestamp)
    return false;
  if (!isTimeValid())
    return false;
  *timestamp = time(nullptr);
  return true;
}

// timestamp to string "YYYY-MM-DD HH:MM:SS"
bool timestampToString(time_t timestamp, char *buffer, size_t bufferSize)
{
  if (!buffer || bufferSize < 20)
    return false; // "YYYY-MM-DD HH:MM:SS" + null
  struct tm lt;
  localtime_r(&timestamp, &lt);
  int n = snprintf(buffer, bufferSize, "%04d-%02d-%02d %02d:%02d:%02d",
                   lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                   lt.tm_hour, lt.tm_min, lt.tm_sec);
  return n > 0 && (size_t)n < bufferSize;
}