#include "log_serial.hpp"
#include <stdarg.h>

typedef struct {
  uint32_t  ms;                         // timestamp (millis)
  LogLevel  lvl;                        // nivel
  char      text[LOGSERIAL_MAX_CHARS];  // mensaje (truncado si excede)
} LogMsg;

static QueueHandle_t gLogQ = nullptr;
static TaskHandle_t  gLogTask = nullptr;
static LogLevel      gMinLogLevel = LOG_DEBUG; // Nivel mínimo de log (por defecto: todos)

static void logTask(void*);

// Mapeo de nivel -> letra
static inline char lvlChar(LogLevel l) {
  switch (l) {
    case LOG_DEBUG: return 'D';
    case LOG_INFO:  return 'I';
    case LOG_WARN:  return 'W';
    case LOG_ERROR: return 'E';
    default:        return '?';
  }
}

bool logTaskStart(size_t queue_len, uint32_t task_stack, UBaseType_t task_prio, int8_t core)
{
  if (!gLogQ) {
    gLogQ = xQueueCreate(queue_len, sizeof(LogMsg));
    if (!gLogQ) return false;
  }
  if (gLogTask) return true; // ya creada

#if CONFIG_FREERTOS_UNICORE
  (void)core; // ignorar
  BaseType_t ok = xTaskCreate(
      logTask, "log_serial", task_stack, nullptr, task_prio, &gLogTask);
#else
  if (core < 0) {
    BaseType_t ok = xTaskCreate(
        logTask, "log_serial", task_stack, nullptr, task_prio, &gLogTask);
    return ok == pdPASS;
  }
  BaseType_t ok = xTaskCreatePinnedToCore(
      logTask, "log_serial", task_stack, nullptr, task_prio, &gLogTask, core);
#endif
  return ok == pdPASS;
}

void logSetLevel(LogLevel level)
{
  gMinLogLevel = level;
}

LogLevel logGetLevel()
{
  return gMinLogLevel;
}

bool logEnqueue(LogLevel lvl, const char* cstr, TickType_t timeout)
{
  if (!gLogQ || !cstr) return false;
  
  // Filtrar por nivel de log
  if (lvl < gMinLogLevel) return true; // Se considera "exitoso" pero no se procesa

  LogMsg msg;
  msg.ms  = millis();
  msg.lvl = lvl;

  // Copia segura con truncado y null-terminación
  size_t n = strnlen(cstr, LOGSERIAL_MAX_CHARS - 1);
  memcpy(msg.text, cstr, n);
  msg.text[n] = '\0';

  return xQueueSend(gLogQ, &msg, timeout) == pdTRUE;
}

bool logPrintf(LogLevel lvl, const char* fmt, ...)
{
  if (!gLogQ || !fmt) return false;
  
  // Filtrar por nivel de log
  if (lvl < gMinLogLevel) return true; // Se considera "exitoso" pero no se procesa

  LogMsg msg;
  msg.ms  = millis();
  msg.lvl = lvl;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg.text, LOGSERIAL_MAX_CHARS, fmt, ap);
  va_end(ap);

  // Garantiza terminación
  msg.text[LOGSERIAL_MAX_CHARS - 1] = '\0';

  // No bloqueamos; si la cola está llena, se descarta (o pon un timeout)
  return xQueueSend(gLogQ, &msg, 0) == pdTRUE;
}

static void logTask(void*)
{
  // Esta tarea corre con prioridad baja; imprimirá cuando el sistema esté ocioso
  for (;;) {
    LogMsg m;
    // Espera hasta 250 ms por un mensaje; si no hay, cede CPU
    if (xQueueReceive(gLogQ, &m, pdMS_TO_TICKS(250)) == pdTRUE) {
      // Formato: [ssssss.ms][I] mensaje
      // Nota: evita prints larguísimos para no bloquear demasiado tiempo
      Serial.print('[');
      Serial.print(m.ms);
      Serial.print(" ms][");
      Serial.print(lvlChar(m.lvl));
      Serial.print("] ");
      Serial.println(m.text);
    }
    // Pequeño yield para no monopolizar UART en ráfagas
    taskYIELD();
  }
}
