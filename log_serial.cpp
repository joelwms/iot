#include "log_serial.hpp"
#include <stdarg.h>
#include "syncTime.hpp"

typedef struct
{
  time_t ts;                      // timestamp
  LogLevel lvl;                   // nivel
  char text[LOGSERIAL_MAX_CHARS]; // mensaje (truncado si excede)
} LogMsg;

static QueueHandle_t gLogQ = nullptr;
static TaskHandle_t gLogTask = nullptr;
static LogLevel gMinLogLevel = LOG_DEBUG; // Nivel mínimo de log (por defecto: todos)
// static LogLevel gMinLogLevel = LOG_INFO; // Nivel mínimo de log (por defecto: INFO)

static void logTask(void *);

// Mapeo de nivel -> letra
static inline char lvlChar(LogLevel l)
{
  switch (l)
  {
  case LOG_DEBUG:
    return 'D';
  case LOG_INFO:
    return 'I';
  case LOG_WARN:
    return 'W';
  case LOG_ERROR:
    return 'E';
  default:
    return '?';
  }
}

bool logTaskStart(size_t queue_len, uint32_t task_stack, UBaseType_t task_prio, int8_t core)
{
  if (!gLogQ)
  {
    gLogQ = xQueueCreate(queue_len, sizeof(LogMsg));
    if (!gLogQ)
      return false;
  }
  if (gLogTask)
    return true; // ya creada

#if CONFIG_FREERTOS_UNICORE
  (void)core; // ignorar
  BaseType_t ok = xTaskCreate(
      logTask, "log_serial", task_stack, nullptr, task_prio, &gLogTask);
#else
  if (core < 0)
  {
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

bool logEnqueue(LogLevel lvl, const char *cstr, TickType_t timeout)
{
  if (!gLogQ || !cstr)
    return false;

  // Filtrar por nivel de log
  if (lvl < gMinLogLevel)
    return true; // Se considera "exitoso" pero no se procesa

  LogMsg msg;
  // bool getTimestamp(time_t* timestamp)
  getTimestamp(&msg.ts);
  msg.lvl = lvl;

  // Copia segura con truncado y null-terminación
  size_t n = strnlen(cstr, LOGSERIAL_MAX_CHARS - 1);
  memcpy(msg.text, cstr, n);
  msg.text[n] = '\0';

  return xQueueSend(gLogQ, &msg, timeout) == pdTRUE;
}

static inline bool inISR()
{
#if CONFIG_IDF_TARGET_ESP32 || defined(ARDUINO_ARCH_ESP32)
  return xPortInIsrContext();
#else
  return false;
#endif
}

bool logPrintf(LogLevel lvl, const char *fmt, ...)
{
  if (!gLogQ || !fmt)
    return false;

  // Filtrar por nivel de log
  if (lvl < gMinLogLevel)
    return true; // Se considera "exitoso" pero no se procesa

  // Serial.print("logTask HWM: ");
  // Serial.println( (unsigned)uxTaskGetStackHighWaterMark(gLogTask) );

  LogMsg msg;
  getTimestamp(&msg.ts);
  msg.lvl = lvl;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg.text, LOGSERIAL_MAX_CHARS, fmt, ap);
  va_end(ap);

  // Garantiza terminación
  msg.text[LOGSERIAL_MAX_CHARS - 1] = '\0';

  UBaseType_t n_palabras = uxQueueMessagesWaiting(gLogQ);
  if (n_palabras > 50)
  {
    Serial.print("Log queue size: ");
    Serial.println((unsigned)n_palabras);
  }

  // No bloqueamos; si la cola está llena, se descarta (o pon un timeout)
  return xQueueSend(gLogQ, &msg, 0) == pdTRUE;
}

// bool logPrintf(LogLevel lvl, const char* fmt, ...){

//   if(inISR()){
//     Serial.print("[ISR] ");
//     // Serial.print(lvlChar(lvl));
//   }

//   Serial.println(fmt);
//   return true;
// }

// static void logTask(void*)
// {
//   // Esta tarea corre con prioridad baja; imprimirá cuando el sistema esté ocioso
//   for (;;) {
//     LogMsg m;
//     // Espera hasta 250 ms por un mensaje; si no hay, cede CPU
//     if (xQueueReceive(gLogQ, &m, pdMS_TO_TICKS(250)) == pdTRUE) {
//       // Formato: [ssssss.ms][I] mensaje
//       // Nota: evita prints larguísimos para no bloquear demasiado tiempo
//       Serial.print('[');
//       Serial.print(m.ms);
//       Serial.print(" ms][");
//       Serial.print(lvlChar(m.lvl));
//       Serial.print("] ");
//       Serial.println(m.text);
//     }
//     // Pequeño yield para no monopolizar UART en ráfagas
//     taskYIELD();
//   }
// }

static void logTask(void *)
{
  const uint32_t MAX_PER_SLICE = 16; // límite de líneas por “slice”
  for (;;)
  {
    LogMsg m;
    // Espera corto: no te quedes 250 ms si hay avalancha (reduce latencia) pero cede CPU
    if (xQueueReceive(gLogQ, &m, pdMS_TO_TICKS(50)) == pdTRUE)
    {

      uint32_t printed = 0;
      do
      {
        // Si el TX está lleno, no bloquees: cede un tick y deja respirar al Idle/WDT
        if (Serial.availableForWrite() <= 0)
        {
          vTaskDelay(1);
          break; // sal del do/while; ya seguirás en la próxima vuelta
        }

        // bool timestampToString(time_t timestamp, char* buffer, size_t bufferSize)
        char timeStr[20];
        timestampToString(m.ts, timeStr, sizeof(timeStr));
        // Construye la línea de una vez (menos llamadas a Serial)
        char line[LOGSERIAL_MAX_CHARS + 24];
        int len = snprintf(line, sizeof(line),
                           "[%s][%c] %s",
                           timeStr, lvlChar(m.lvl), m.text);
        if (len < 0)
          len = 0;
        if (len >= (int)sizeof(line))
          len = sizeof(line) - 1;
        line[len] = '\0';

        Serial.println(line);

        printed++;
        if (printed >= MAX_PER_SLICE)
        {
          // Evita monopolio en ráfaga: garantiza tiempo a Idle y otras tareas
          vTaskDelay(1);
          printed = 0;
        }

        // Encadena siguiente mensaje si hay, sin bloquear
      } while (xQueueReceive(gLogQ, &m, 0) == pdTRUE);

      // Serial.println();
    }
    else
    {
      // Sin mensajes: cede un tick para alimentar Idle/WDT
      vTaskDelay(1);
    }
  }
}
