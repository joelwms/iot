#include "mqttCallback.hpp"
#include "log_serial.hpp" // para logging
#include <cstring>

Route *routesArray = nullptr;
size_t numRoutes = 0;

static PubSubClient *gClient = nullptr;

void configureMqtt(const char *server, uint16_t port, PubSubClient &client)
{
    LOGD("Configuring MQTT with server: %s, port: %d", server ? server : "NULL", port);
    gClient = &client;
    client.setServer(server, port);
    client.setCallback(mqttCallback);

    LOGI("MQTT configured successfully with server: %s:%d", server ? server : "NULL", port);
    mqttOnReconnect(); // suscribe todo si ya está conectado (poco probable)
    LOGI("-----------MQTT_MAX_PACKET_SIZE: %d", MQTT_MAX_PACKET_SIZE);
}

static bool topicExists(const char *topic)
{
    LOGD("Checking if topic exists: %s", topic ? topic : "NULL");
    for (size_t i = 0; i < numRoutes; ++i)
    {
        if (std::strcmp(routesArray[i].topic, topic) == 0)
            return true;
    }
    return false;
}

bool addRoute(const Route &r)
{
    LOGD("Adding route for topic: %s, QoS: %d", r.topic ? r.topic : "NULL", r.qos);
    if (!r.topic || !r.handler)
    {
        LOGE("Cannot add route: %s", !r.topic ? "Topic is NULL" : "Handler is NULL");
        return false;
    }
    if (topicExists(r.topic))
    {
        LOGI("Route for topic '%s' already exists (idempotent)", r.topic);
        return true; // ya estaba; idempotente
    }

    Route *newRoutes = new Route[numRoutes + 1];
    for (size_t i = 0; i < numRoutes; ++i)
        newRoutes[i] = routesArray[i];
    newRoutes[numRoutes] = r;
    delete[] routesArray;
    routesArray = newRoutes;
    ++numRoutes;

    LOGI("Route added successfully for topic: %s (total routes: %zu)", r.topic, numRoutes);

    // Suscribirse inmediatamente si tenemos cliente
    if (gClient)
    {
        // Si ya está conectado suscribimos ahora; si no, lo haremos en mqttOnReconnect()
        if (gClient->connected())
        {
            bool ok = gClient->subscribe(r.topic, r.qos);
            if (!ok)
            {
                LOGE("Failed to subscribe to topic: %s", r.topic);
            }
            else
            {
                LOGI("Successfully subscribed to topic: %s", r.topic);
            }
            return ok;
        }
    }
    LOGW("Route added but not subscribed (client not connected): %s", r.topic);
    return true; // ruta añadida; se suscribirá en la próxima reconexión
}

size_t addRoutes(const Route *routes, size_t count)
{
    LOGD("Adding %zu routes", count);
    size_t added = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (addRoute(routes[i]))
            ++added; // cada addRoute intenta suscribir si procede
    }
    LOGI("Added %zu out of %zu routes successfully", added, count);
    return added;
}

bool removeRoute(const char *topic)
{
    LOGD("Removing route for topic: %s", topic ? topic : "NULL");
    if (!topic || numRoutes == 0)
    {
        LOGE("Cannot remove route: %s", !topic ? "Topic is NULL" : "No routes available");
        return false;
    }
    size_t idx = numRoutes;
    for (size_t i = 0; i < numRoutes; ++i)
    {
        if (std::strcmp(routesArray[i].topic, topic) == 0)
        {
            idx = i;
            break;
        }
    }
    if (idx == numRoutes)
    {
        LOGE("Route not found for removal: %s", topic);
        return false; // no encontrada
    }

    if (gClient && gClient->connected())
    {
        gClient->unsubscribe(topic);
        LOGI("Unsubscribed from topic: %s", topic);
    }

    Route *newRoutes = (numRoutes > 1) ? new Route[numRoutes - 1] : nullptr;
    for (size_t i = 0, j = 0; i < numRoutes; ++i)
    {
        if (i == idx)
            continue;
        newRoutes[j++] = routesArray[i];
    }
    delete[] routesArray;
    routesArray = newRoutes;
    --numRoutes;
    LOGI("Route removed successfully for topic: %s (remaining routes: %zu)", topic, numRoutes);
    return true;
}

// Llamar tras reconectar MQTT para re-suscribir todo lo registrado
void mqttOnReconnect()
{
    LOGD("Reconnecting MQTT - resubscribing to %zu routes", numRoutes);
    if (!gClient || !gClient->connected())
    {
        LOGW("Cannot resubscribe: %s", !gClient ? "Client is NULL" : "Client not connected");
        return;
    }
    for (size_t i = 0; i < numRoutes; ++i)
    {
        bool ok = gClient->subscribe(routesArray[i].topic, routesArray[i].qos);
        if (!ok)
        {
            LOGE("Failed to resubscribe to topic: %s", routesArray[i].topic);
        }
        else
        {
            LOGI("Resubscribed successfully to topic: %s", routesArray[i].topic);
        }
    }
    // LOGI("Resubscribed to all %zu routes successfully", numRoutes);
}

// --- Router ---
void mqttCallback(char *topic, uint8_t *payload, unsigned int len)
{
    LOGD("MQTT message received - topic: %s, length: %u", topic ? topic : "NULL", len);
    for (size_t i = 0; i < numRoutes; ++i)
    {
        if (std::strcmp(topic, routesArray[i].topic) == 0)
        {
            LOGI("Message routed to handler for topic: %s", topic);
            routesArray[i].handler(routesArray[i].ctx, topic, payload, len);
            return;
        }
    }
    LOGW("Unhandled topic received: %s", topic);
}
