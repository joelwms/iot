#include "control.hpp"
#include "syncTime.hpp"     // debe darte h, m; si también da día mejor
#include "mqttCallback.hpp" // router con ctx
#include "mqtt_manager.hpp"
#include "log_serial.hpp" // para logging
#include <ArduinoJson.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

#define DELAY_MINUTES 1
#define DELAY_CONTROL (DELAY_MINUTES * 60 * 1000)

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
        if (!control->manual_mode)
        {
            LOGW("Received ON command while not in manual mode for control %s", control->name);
            return;
        }
        LOGI("Received ON command for control");
        encenderRele(control);
        return;
    }
    if (msg == "OFF")
    {
        if (!control->manual_mode)
        {
            LOGW("Received OFF command while not in manual mode for control %s", control->name);
            return;
        }
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

static void onControlManual(void *ctx, const char *topic, const uint8_t *payload, unsigned int len)
{
    LOGD("Manual mode command received on topic: %s, length: %u", topic, len);
    Control *control = static_cast<Control *>(ctx);
    String msg((const char *)payload, len);

    if (msg == "ON")
    {
        LOGI("Manual mode enabled for control %s", control->name);
        control->manual_mode = true;
        return;
    }
    if (msg == "OFF")
    {
        LOGI("Manual mode disabled for control %s", control->name);
        control->manual_mode = false;
        encenderApagarSegunHora(control); // aplicar programación actual
        return;
    }

    LOGW("Unknown manual mode command received on topic %s: %s", topic, msg.c_str());
}

static void onControlAdvance(void *ctx, const char *topic, const uint8_t *payload, unsigned int len)
{
    LOGD("Advance minutes command received on topic: %s, length: %u", topic, len);
    Control *control = static_cast<Control *>(ctx);
    String msg((const char *)payload, len);

    // Convertir el mensaje a entero
    int minutes = msg.toInt();

    // Validar que sea un valor razonable (0-120 minutos)
    if (minutes < 0 || minutes > 120)
    {
        LOGW("Invalid advance minutes value: %d (must be between 0 and 120)", minutes);
        return;
    }

    control->advance_minutes = minutes;
    LOGI("Advance minutes set to %d for control %s", minutes, control->name);
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

    Route manualRoute;
    manualRoute.topic = control->manual_topic_str;
    manualRoute.handler = onControlManual;
    manualRoute.qos = 1;
    manualRoute.ctx = control;

    Route advanceRoute;
    advanceRoute.topic = control->advance_topic_str;
    advanceRoute.handler = onControlAdvance;
    advanceRoute.qos = 1;
    advanceRoute.ctx = control;

    // Registrar rutas de esta instancia (suscriben al vuelo si hay conexión)
    bool result_cmd = addRoute(cmdRoute);
    bool result_sched = addRoute(schedRoute);
    bool result_manual = addRoute(manualRoute);
    bool result_advance = addRoute(advanceRoute);

    if (!result_cmd || !result_sched || !result_manual || !result_advance)
    {
        LOGE("Failed to register one or more MQTT routes for control %s", control->name);
    }
    else
    {
        // LOGI("All MQTT routes registered successfully for control %s", control->name);
        LOGI("MQTT routes registered for cmd: %s, schedule: %s, manual: %s, advance: %s",
             control->cmd_topic_str, control->schedule_topic_str, control->manual_topic_str, control->advance_topic_str);
    }

    // Publicar estado "online" al iniciar
    mqttEnqueuePublish(control->status_topic_str, "online", true);
    LOGI("Control %s status published: online", control->name);

    // Bucle cada 5 minutos
    while (1)
    {
        // 1) Aplicar programación pendiente a las 00:00 una sola vez
        //    (Sin fecha: usamos un latch seguro con umbrales de hora)
        //    - Latch se arma en última hora del día (>= 23:00).
        //    - Se dispara y consume a las 00:00..00:04 aprox.
        int hh, mm;
        if (getHourMinute(&hh, &mm))
        {
            // LOGD("Actual time: %02d:%02d", hh, mm);
        }
        else
        {
            LOGW("Invalid time yet.");
            vTaskDelay(pdMS_TO_TICKS(DELAY_CONTROL));
            continue;
        }

        if (hh >= 23)
        {
            control->appliedTodayLatch = true; // pre-armado para el salto de día
        }
        if (hh == 0 && mm < (DELAY_MINUTES + 1))
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
    // Si el modo manual está activado, no aplicamos la programación
    if (control->manual_mode)
    {
        LOGD("Manual mode active for %s, skipping schedule", control->name);
        return;
    }

    LOGD("Checking pump control based on current time");
    int hh, mm;
    if (!getHourMinute(&hh, &mm))
    {
        LOGW("Could not obtain current time for pump control");
        return;
    }

    // Calcular índice real (sin adelanto) para el apagado
    int current_minutes = hh * 60 + mm;
    const int real_idx = hh * 4 + (mm / 15);

    // Calcular índice con adelanto para el encendido
    int adjusted_minutes = current_minutes + control->advance_minutes;
    if (adjusted_minutes >= 1440) // 1440 = 24 * 60
    {
        adjusted_minutes -= 1440;
    }
    int adjusted_hh = adjusted_minutes / 60;
    int adjusted_mm = adjusted_minutes % 60;
    const int advanced_idx = adjusted_hh * 4 + (adjusted_mm / 15);

    // Verificar si debe estar encendido según programación real (para apagar)
    bool should_be_on_real = (real_idx >= 0 && real_idx < 96 && control->programacion[real_idx] != -1);

    // Verificar si debe encenderse según programación con adelanto
    bool should_turn_on_advanced = (advanced_idx >= 0 && advanced_idx < 96 && control->programacion[advanced_idx] != -1);

    if (control->advance_minutes > 0)
    {
        LOGD("Time check for %s: real=%02d:%02d (idx=%d, prog=%d), advanced=%02d:%02d (idx=%d, prog=%d)",
             control->name, hh, mm, real_idx,
             (real_idx >= 0 && real_idx < 96) ? control->programacion[real_idx] : -2,
             adjusted_hh, adjusted_mm, advanced_idx,
             (advanced_idx >= 0 && advanced_idx < 96) ? control->programacion[advanced_idx] : -2);
    }

    // Lógica de decisión:
    // - Encender si el índice adelantado indica ON
    // - Apagar SOLO si el índice real indica OFF
    if (should_turn_on_advanced)
    {
        LOGI("Activating pump %s based on schedule with advance (index: %d)", control->name, advanced_idx);
        encenderRele(control);
    }
    else if (!should_be_on_real)
    {
        // Solo apagar si la programación real dice OFF
        apagarRele(control);
    }
    // Si should_turn_on_advanced=false pero should_be_on_real=true, mantener estado actual
}

static void encenderRele(Control *control)
{
    // int curr = digitalRead(control->pin);
    // if (curr == HIGH) {
    //     LOGD("Relay %s already ON on pin %d. Skipping.", control->name, control->pin);
    //     return;
    // }

    LOGD("Turning relay %s ON for pin: %d", control->name, control->pin);
    digitalWrite(control->pin, HIGH);
    mqttEnqueuePublish(control->state_topic_str, "ON", true);
    LOGI("Relay %s turned ON, state published", control->name);
}

static void apagarRele(Control *control)
{
    // int curr = digitalRead(control->pin);
    // if (curr == LOW) {
    //     LOGD("Relay %s already OFF on pin %d. Skipping.", control->name, control->pin);
    //     return;
    // }

    LOGD("Turning relay %s OFF for pin: %d", control->name, control->pin);
    digitalWrite(control->pin, LOW);
    mqttEnqueuePublish(control->state_topic_str, "OFF", true);
    LOGI("Relay %s turned OFF, state published", control->name);
}

// Array de enteros de 96 posiciones (cuartos de hora) todo a -1 (OFF)
// int[96] prog = [-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1];