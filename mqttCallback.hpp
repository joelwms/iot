#pragma once
// #define MQTT_MAX_PACKET_SIZE 512
#include <Arduino.h>
#include <PubSubClient.h>

typedef void (*MqttHandler)(void *ctx, const char *topic, const uint8_t *payload, unsigned int len);

struct Route
{
    const char *topic;
    MqttHandler handler;
    uint8_t qos = 0;
    void *ctx = nullptr; // <-- contexto por ruta (p.ej., Control*)
};

extern Route *routesArray;
extern size_t numRoutes;

void configureMqtt(const char *server, uint16_t port, PubSubClient &client); // guarda puntero + setCallback
bool addRoute(const Route &r);                                               // añade + subscribe inmediato
size_t addRoutes(const Route *routes, size_t count);                         // añade en bloque (subscribiendo)
bool removeRoute(const char *topic);                                         // opcional
void mqttCallback(char *topic, uint8_t *payload, unsigned int len);
void mqttOnReconnect(); // re-suscribe todo tras reconexión
