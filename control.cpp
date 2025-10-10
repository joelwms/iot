#include "control.hpp"
#include "syncTime.hpp"     // debe darte h, m; si también da día mejor
#include "mqttCallback.hpp" // router con ctx
#include "mqtt_manager.hpp"
#include "log_serial.hpp"   // para logging
#include <ArduinoJson.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

#define DELAY_MIN 5
#define DELAY_CONTROL (DELAY_MIN * 60 * 1000)

// --- Prototipos internos ---
static void encenderApagarSegunHora(Control *control);
static void encenderRele(Control *control);
static void apagarRele(Control *control);

// --- Handlers MQTT por instancia ---
static void onControlCmd(void *ctx, const char *topic, const uint8_t *payload, unsigned int len)
{
    LOGD("Control command received on topic: %s, length: %u", topic, len);
    Control *control = static_cast<Control *>(ctx);
    String msg((const char *)payload, len);

    if (msg == "ON")
    {
        LOGI("Received ON command for control");
        encenderRele(control);
        return;
    }
    if (msg == "OFF")
    {
        LOGI("Received OFF command for control");
        apagarRele(control);
        return;
    }

    // No comandos extra aquí: la programación llega por schedule_topic_str (JSON).
    LOGW("Unknown command received on topic %s: %s", topic, msg.c_str());
}

static void onControlSchedule(void *ctx, const char *topic, const uint8_t *payload, unsigned int len)
{
    LOGD("Control schedule received on topic: %s, length: %u", topic, len);
    Control *control = static_cast<Control *>(ctx);

    // Esperamos JSON array de 96 enteros, p.ej: [ -1, -1, 1, ... ]
    // Reserva: 96 enteros + overhead -> 2 KB suele ir sobrado en ESP32
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err)
    {
        LOGE("Invalid JSON in schedule: %s", err.c_str());
        return;
    }

    if (!doc.is<JsonArray>())
    {
        LOGE("Schedule is not a JSON array");
        return;
    }

    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() != 96)
    {
        LOGE("Schedule has invalid size: %zu (expected 96)", arr.size());
        return;
    }

    // Copiamos al buffer pendiente sin tocar la programación activa
    for (int i = 0; i < 96; ++i)
    {
        int v = arr[i] | -1; // valor por defecto -1 si falta
        control->pendingProgramacion[i] = (int8_t)v;
    }
    control->pendingValid = true;

    LOGI("Schedule received and stored (pending) for topic: %s", topic);
}

// --- Tarea ---
void task_control(void *params)
{
    LOGD("Starting control task");
    Control *control = static_cast<Control *>(params);

    // Inicializa programación actual y pendiente
    for (int i = 0; i < 96; ++i)
    {
        control->programacion[i] = -1;
        control->pendingProgramacion[i] = -1;
    }
    control->pendingValid = false;
    control->appliedTodayLatch = false;

    // Configurar pin
    pinMode(control->pin, OUTPUT);
    apagarRele(control);

    LOGI("Control task initialized for pin %d", control->pin);

    Route cmdRoute;
    cmdRoute.topic = control->cmd_topic_str;
    cmdRoute.handler = onControlCmd;
    cmdRoute.qos = 1;
    cmdRoute.ctx = control;

    Route schedRoute;
    schedRoute.topic = control->schedule_topic_str;
    schedRoute.handler = onControlSchedule;
    schedRoute.qos = 1;
    schedRoute.ctx = control;

    // Registrar rutas de esta instancia (suscriben al vuelo si hay conexión)
    addRoute(cmdRoute);
    addRoute(schedRoute);

    LOGI("MQTT routes registered for cmd: %s, schedule: %s", 
         control->cmd_topic_str, control->schedule_topic_str);

    // Bucle cada 5 minutos
    while (1)
    {
        // 1) Aplicar programación pendiente a las 00:00 una sola vez
        //    (Sin fecha: usamos un latch seguro con umbrales de hora)
        //    - Latch se arma en última hora del día (>= 23:00).
        //    - Se dispara y consume a las 00:00..00:04 aprox.
        int hh, mm;
        if (getHourMinute(&hh, &mm)) {
            LOGI("Hora actual: %02d:%02d", hh, mm);
        } else {
            LOGW("Hora no válida todavía.");
            vTaskDelay(pdMS_TO_TICKS(DELAY_CONTROL));
            continue;
        }

        if (hh >= 23)
        {
            control->appliedTodayLatch = true; // pre-armado para el salto de día
        }
        if (hh == 0 && mm < (DELAY_MIN + 1))
        { // ventana segura tras medianoche
            if (control->appliedTodayLatch && control->pendingValid)
            {
                for (int i = 0; i < 96; ++i)
                {
                    control->programacion[i] = control->pendingProgramacion[i];
                }
                control->pendingValid = false; // consumido
                control->appliedTodayLatch = false;

                LOGI("Schedule applied at 00:00 for topic: %s", control->schedule_topic_str);
            }
            else
            {
                // Si no hay pendingValid o ya aplicado, soltamos el latch para no re-aplicar
                LOGW("No pending schedule to apply or already applied today");
                control->appliedTodayLatch = false;
            }
        }

        // 2) Aplicar lógica ON/OFF por cuarto de hora
        encenderApagarSegunHora(control);

        // 3) Espera
        vTaskDelay(pdMS_TO_TICKS(DELAY_CONTROL));
    }
}

// --- Lógica de control ---
static void encenderApagarSegunHora(Control *control)
{
    LOGD("Checking pump control based on current time");
    // syncTime *horaActual = obtenerHoraActual();
    int hh, mm;
    if (!getHourMinute(&hh, &mm)) {
        LOGW("Could not obtain current time for pump control");
        return;
    }

    const int idx = hh * 4 + (mm / 15);

    if (idx >= 0 && idx < 96 && control->programacion[idx] != -1)
    {
        LOGI("Activating pump based on schedule (time index: %d)", idx);
        encenderRele(control);
    }
    else
    {
        apagarRele(control);
    }
}

static void encenderRele(Control *control)
{
    LOGD("Turning relay ON for pin: %d", control->pin);
    digitalWrite(control->pin, HIGH);
    mqttEnqueuePublish(control->state_topic_str, "ON", true);
    LOGI("Relay turned ON, state published");
}

static void apagarRele(Control *control)
{
    LOGD("Turning relay OFF for pin: %d", control->pin);
    digitalWrite(control->pin, LOW);
    mqttEnqueuePublish(control->state_topic_str, "OFF", true);
    LOGI("Relay turned OFF, state published");
}
