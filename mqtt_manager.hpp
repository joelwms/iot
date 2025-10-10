#pragma once
#include <Arduino.h>
#include <PubSubClient.h>

// Arranca la tarea que gestiona loop() y reconexión.
// clientId/lwt son opcionales; ajusta a tu proyecto.
void mqttManagerStart(PubSubClient &client,
                      const char *clientId,
                      const char *lwtTopic = nullptr,
                      const char *lwtMsg = nullptr);

// Encola un publish para que lo ejecute la tarea MQTT (thread-safe).
bool mqttEnqueuePublish(const char *topic, const char *payload,
                        bool retained = false, uint8_t qos = 0, TickType_t timeout = 0);
