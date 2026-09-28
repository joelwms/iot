#pragma once
#include <PubSubClient.h>

// Configuración por instancia del sensor
struct Sensor
{
    const char *name;            // name of the sensor device, e.g., "nivel_balsa"
    int pin_adc;                 // GPIO del ADC (mejor ADC1: 32..39 en ESP32)
    const char *state_topic_str; // Topic donde publicar la medida
    bool retain = false;         // Si quieres publicar retenido (normalmente false)

    // Valor de escala: ¿qué valor "físico" equivale a 3.3 V?
    // Ej.: si 3.3 V = 250 cm de nivel, pon full_scale_value = 250.0f
    float full_scale_value = 1.0f;
};

// Tarea que lee el ADC y publica por MQTT periódicamente
void task_sensor(void *params);
