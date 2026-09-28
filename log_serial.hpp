#pragma once
#include <Arduino.h>

extern "C" {
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "freertos/queue.h"
}

// ================= Configuración =================
#ifndef LOGSERIAL_MAX_CHARS
#define LOGSERIAL_MAX_CHARS 160   // tamaño máx. del texto por mensaje (se trunca)
#endif

// ================= Tipos =================
enum LogLevel : uint8_t {
  LOG_DEBUG = 0,
  LOG_INFO  = 1,
  LOG_WARN  = 2,
  LOG_ERROR = 3
};

// ================= API =================
// Crea la cola y lanza la tarea logger (baja prioridad).
//  - queue_len: profundidad de la cola (p.ej. 64)
//  - task_stack: stack en bytes (p.ej. 3072~4096)
//  - task_prio: baja prioridad (p.ej. 0 o 1)
//  - core: usa -1 para no fijar core; 0/1 para ESP32 (opcional)
bool logTaskStart(size_t queue_len = 64,
                  uint32_t task_stack = 4096,
                  UBaseType_t task_prio = 0,
                  int8_t core = -1);

// Encola un mensaje ya formateado (texto C-string).
bool logEnqueue(LogLevel lvl, const char* cstr,
                TickType_t timeout = 0);

// printf-like (formatea y encola).
bool logPrintf(LogLevel lvl, const char* fmt, ...)
  __attribute__((format(printf, 2, 3)));

// Configurar el nivel mínimo de log
void logSetLevel(LogLevel level);

// Obtener el nivel mínimo de log actual
LogLevel logGetLevel();

// Helpers rápidos
#define LOGD(fmt, ...) logPrintf(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) logPrintf(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) logPrintf(LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) logPrintf(LOG_ERROR, fmt, ##__VA_ARGS__)
