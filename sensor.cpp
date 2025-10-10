#include <Arduino.h>

extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

#include "sensor.hpp"
#include "mqtt_manager.hpp"
#include "log_serial.hpp" // para logging

// ---------- Configuración ----------
#define SENSOR_PERIOD_MINUTES 5   // cada X minutos
#define SENSOR_SAMPLES 8          // nº de muestras a promediar
#define SENSOR_ADC_MAX 4095.0f    // ADC 12 bits (ESP32 Arduino)
#define SENSOR_VREF_VOLTS 3.3f    // tensión máxima (V)
#define SENSOR_PUBLISH_DECIMALS 3 // decimales al publicar
#define SENSOR_TASK_DELAY_MS (SENSOR_PERIOD_MINUTES * 60UL * 1000UL)
// ----------------------------------

// Nota: En ESP32, usa preferentemente pines ADC1 (GPIO32..39) si usas WiFi,
// ya que ADC2 entra en conflicto con el WiFi.

void task_sensor(void *params)
{
    LOGD("Starting sensor task");
    Sensor *sensor = static_cast<Sensor *>(params);

    LOGD("Initializing sensor on ADC pin: %d", sensor->pin_adc);
    // Configura el pin ADC (opcional en ESP32, pero no molesta)
    pinMode(sensor->pin_adc, INPUT);

    // Opcional: ajustar resolución/atenuación si lo necesitas
    // analogReadResolution(12); // por defecto en ESP32 Arduino es 12 bits (0..4095)
    // analogSetPinAttenuation(sensor->pin_adc, ADC_11db); // más rango si la señal es alta

    LOGI("Sensor task initialized - Pin: %d, Full scale: %.2f, Period: %d min", 
         sensor->pin_adc, sensor->full_scale_value, SENSOR_PERIOD_MINUTES);

    for (;;)
    {
        LOGD("Starting sensor reading cycle");
        // 1) Promedio de muestras (para reducir ruido)
        uint32_t acc = 0;
        for (int i = 0; i < SENSOR_SAMPLES; ++i)
        {
            acc += analogRead(sensor->pin_adc);
            vTaskDelay(pdMS_TO_TICKS(1)); // para espaciar
        }
        const float raw_avg = acc / (float)SENSOR_SAMPLES;

        LOGD("Raw ADC average: %.2f (from %d samples)", raw_avg, SENSOR_SAMPLES);

        // 2) Convertir a voltios y a unidades físicas
        const float volts = (raw_avg / SENSOR_ADC_MAX) * SENSOR_VREF_VOLTS;
        float value = (volts / SENSOR_VREF_VOLTS) * sensor->full_scale_value; // == raw/ADC_MAX * full_scale

        LOGI("Sensor reading - Volts: %.3fV, Value: %.3f", volts, value);

        // (opcional) Limitar a rango [0, full_scale]
        if (value < 0.0f)
        {
            LOGW("Sensor value below range (%.3f), clamping to 0.0", value);
            value = 0.0f;
        }
        if (value > sensor->full_scale_value)
        {
            LOGW("Sensor value above range (%.3f > %.3f), clamping to full scale", 
                 value, sensor->full_scale_value);
            value = sensor->full_scale_value;
        }

        // 3) Publicar en MQTT como número en texto con N decimales
        char fmt[8];
        snprintf(fmt, sizeof(fmt), "%%.%df", SENSOR_PUBLISH_DECIMALS);

        char payload[32];
        snprintf(payload, sizeof(payload), fmt, value);

        // Encola; si falla, lo volvemos a intentar en el siguiente ciclo
        bool published = mqttEnqueuePublish(sensor->state_topic_str, payload, sensor->retain);
        
        if (!published) {
            LOGW("Failed to enqueue sensor data for publishing, will retry next cycle");
        } else {
            LOGI("Sensor data published to topic '%s': %s", sensor->state_topic_str, payload);
        }

        // 4) Espera hasta el próximo ciclo
        vTaskDelay(pdMS_TO_TICKS(SENSOR_TASK_DELAY_MS));
    }
}
