#include "mqtt_manager.hpp"
#include "mqttCallback.hpp" // usa mqttOnReconnect() y el callback router
#include "log_serial.hpp"   // para logging
#include <WiFi.h>           // si no usas WiFi, elimina esta dependencia

// --- Cola de salida para publish (evita usar publish desde otras tareas) ---
struct MqttTxMsg
{
    char topic[128];
    char payload[256];
    bool retained;
    uint8_t qos;
};
static QueueHandle_t gMqttTxQ = nullptr;

// --- Contexto ---
static PubSubClient *gClient = nullptr;
static const char *gClientId = "client";
static const char *gLwtTopic = nullptr;
static const char *gLwtMsg = nullptr;

static void mqttManagerTask(void *);

// API
void mqttManagerStart(PubSubClient &client,
                      const char *clientId,
                      const char *lwtTopic,
                      const char *lwtMsg)
{
    LOGD("Starting MQTT manager with clientId: %s", clientId ? clientId : "client");
    gClient = &client;
    gClientId = clientId ? clientId : "client";
    gLwtTopic = lwtTopic;
    gLwtMsg = lwtMsg;

    if (!gMqttTxQ)
    {
        gMqttTxQ = xQueueCreate(16, sizeof(MqttTxMsg)); // ajusta tamaño
        if (!gMqttTxQ)
        {
            LOGE("Failed to create MQTT TX queue");
            return;
        }
    }

    // Asegura que ya configuraste el callback router en otro sitio:
    // configureMqtt(client);  // si aún no lo has llamado

    // xTaskCreatePinnedToCore(mqttManagerTask, "mqtt_mgr", 4096, nullptr, 2, nullptr, 1);
    xTaskCreate(mqttManagerTask, "mqtt_mgr", 4096, nullptr, 3, nullptr);
    LOGI("MQTT manager started");
    LOGI("-----------MQTT_MAX_PACKET_SIZE: %d", MQTT_MAX_PACKET_SIZE);
}

bool mqttEnqueuePublish(const char *topic, const char *payload,
                        bool retained, uint8_t qos, TickType_t timeout)
{
    LOGD("Enqueueing publish to topic: %s, payload: %s", topic ? topic : "NULL", payload ? payload : "NULL");
    if (!gMqttTxQ || !topic || !payload)
    {
        LOGE("Failed to enqueue publish: %s",
             !gMqttTxQ ? "Queue not initialized" : !topic ? "Topic is NULL"
                                                          : "Payload is NULL");
        return false;
    }
    MqttTxMsg msg{};
    strlcpy(msg.topic, topic, sizeof(msg.topic));
    strlcpy(msg.payload, payload, sizeof(msg.payload));
    msg.retained = retained;
    msg.qos = qos;
    bool result = xQueueSend(gMqttTxQ, &msg, timeout) == pdTRUE;
    if (result)
    {
        LOGI("Message successfully enqueued for topic: %s", topic);
    }
    else
    {
        LOGE("Failed to enqueue message for topic: %s (queue full or timeout)", topic);
    }
    return result;
}

// --- Internos ---
static bool wifiReady()
{
    LOGD("Checking WiFi status");
    // Si usas otra conectividad, adapta esta función.
    return (WiFi.status() == WL_CONNECTED);
}

static bool mqttConnect()
{
    LOGD("Attempting MQTT connection with clientId: %s", gClientId);
    if (!gClient)
    {
        LOGE("MQTT client is NULL, cannot connect");
        return false;
    }

    // Con LWT si está configurado
    bool ok;
    if (gLwtTopic && gLwtMsg)
    {
        ok = gClient->connect(gClientId, gLwtTopic, /*qos*/ 0, /*retain*/ true, gLwtMsg);
    }
    else
    {
        ok = gClient->connect(gClientId);
    }
    if (ok)
    {
        LOGI("MQTT connection successful");
        // Re-suscribe TODO lo registrado con addRoute()
        mqttOnReconnect();
        // Mensaje de "birth" opcional
        if (gLwtTopic)
        {
            gClient->publish(gLwtTopic, "ONLINE", true);
            LOGI("Published ONLINE status to LWT topic: %s", gLwtTopic);
        }
    }
    return ok;
}

static void mqttManagerTask(void *)
{
    uint32_t backoffMs = 500; // backoff exponencial 0.5s -> 30s
    const uint32_t backoffMax = 30000;

    for (;;)
    {
        // 1) Conexión/reconexión
        if (!gClient->connected())
        {
            LOGW("MQTT client disconnected, attempting reconnection...");
            if (wifiReady())
            {
                if (mqttConnect())
                {
                    LOGI("MQTT connection established, resetting backoff");
                    backoffMs = 500; // reset backoff
                }
                else
                {
                    LOGW("MQTT connection failed, retrying in %d ms", backoffMs);
                    vTaskDelay(pdMS_TO_TICKS(backoffMs));
                    backoffMs = min(backoffMs * 2, backoffMax);
                    continue; // reintenta
                }
            }
            else
            {
                LOGW("WiFi not ready, waiting for connection...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        // 2) loop MQTT (recibe y dispara callbacks)
        gClient->loop();

        // 3) Drenar un publish de la cola (no bloqueante)
        if (gMqttTxQ && uxQueueMessagesWaiting(gMqttTxQ) > 0)
        {
            MqttTxMsg msg;
            if (xQueueReceive(gMqttTxQ, &msg, 0) == pdTRUE)
            {
                // PubSubClient no soporta QoS>0 en Arduino clásico; si tu fork sí, úsalo.
                gClient->publish(msg.topic, msg.payload, msg.retained);
                LOGI("Published message to topic: %s", msg.topic);
            }
        }

        // 4) Pequeño yield para no monopolizar CPU
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
