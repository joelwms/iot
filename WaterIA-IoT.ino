#include <WiFi.h>
#include <PubSubClient.h>
#include "syncTime.hpp"
#include "mqttCallback.hpp"
#include "mqtt_manager.hpp"
#include "control.hpp"
#include "sensor.hpp"
#include "log_serial.hpp"

// ================== WiFi & MQTT ==================

// const char *SSID = "14 de VAS";
// const char *WIFI_PASS = "yO%#3246%##";

const char *SSID = "TP-LINK_630D3E";
const char *WIFI_PASS = "28255000";

const char *MQTT_HOST = "35.180.140.231";
const uint16_t MQTT_PORT = 1883;

WiFiClient net;
PubSubClient client(net);

// ================== ARRAYS GLOBALES ==================
#define NUM_CONTROLES 2
#define NUM_SENSORES  1

Control gControles[NUM_CONTROLES];  // se rellenan en initControles()
Sensor  gSensores [NUM_SENSORES];   // se rellenan en initSensores()

// ================== PROTOTIPOS ==================
void connectWiFi();
void mqttSetup();
void initControles();   // asigna explícitamente todos los campos
void initSensores();    // asigna explícitamente todos los campos
void lanzarControles(); // crea una tarea por cada control
void lanzarSensores();  // crea una tarea por cada sensor

// ================== SETUP / LOOP ==================
void setup() {
  Serial.begin(115200);
  logTaskStart(/*queue_len=*/64, /*stack=*/4096, /*prio=*/0, /*core=*/1);

  LOGD("SDK Version: %s", ESP.getSdkVersion());
  LOGI("=== WaterIA IoT System Starting ===");
  LOGI("System initialization beginning...");

  connectWiFi();
  initializeTime();

  mqttSetup();

  initControles();
  initSensores();

  lanzarControles();
  lanzarSensores();

  LOGI("=== WaterIA IoT System Started Successfully ===");
  LOGI("All tasks created, system operational");
}

void loop()
{
  // vacío: todo lo gestionan las tareas FreeRTOS
}

// ================== IMPLEMENTACIÓN ==================
void connectWiFi()
{
  LOGI("Connecting to WiFi network: %s", SSID);
  WiFi.begin(SSID, WIFI_PASS);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if (attempts % 20 == 0) {  // Log every 5 seconds (20 * 250ms)
      LOGW("WiFi connection attempt %d, status: %d", attempts / 4, WiFi.status());
    }
    vTaskDelay(pdMS_TO_TICKS(250));
    attempts++;
  }
  
  LOGI("WiFi connected successfully!");
  LOGI("IP Address: %s", WiFi.localIP().toString().c_str());
  LOGI("Signal strength: %d dBm", WiFi.RSSI());
}

void mqttSetup()
{
  LOGI("Setting up MQTT connection to %s:%d", MQTT_HOST, MQTT_PORT);
  configureMqtt(MQTT_HOST, MQTT_PORT, client);
  mqttManagerStart(client, "wateria-device-01", "wateria/bridge/lwt", "OFFLINE");
  LOGI("MQTT manager started with client ID: wateria-device-01");
}

// ---- Inicialización explícita de controles ----
void initControles() {
  LOGI("Initializing %d control devices", NUM_CONTROLES);
  
  // Control 0: Pozo
  LOGD("Setting up Control 0 (Pozo) on pin %d", 18);
  gControles[0].pin                = 18;
  gControles[0].cmd_topic_str      = "casa/riego/pozo/cmd";
  gControles[0].state_topic_str    = "casa/riego/pozo/state";
  gControles[0].schedule_topic_str = "ESPERANZA/programacion/pozo";
  for (int k = 0; k < 96; ++k) {
    gControles[0].programacion[k] = -1;
  }

  // Control 1: Balsa
  LOGD("Setting up Control 1 (Balsa) on pin %d", 19);
  gControles[1].pin                = 19;
  gControles[1].cmd_topic_str      = "casa/riego/balsa/cmd";
  gControles[1].state_topic_str    = "casa/riego/balsa/state";
  gControles[1].schedule_topic_str = "ESPERANZA/programacion/balsa";
  for (int k = 0; k < 96; ++k) {
    gControles[1].programacion[k] = -1;
  }
  
  LOGI("Control devices initialized successfully");
}

// ---- Inicialización explícita de sensores ----
void initSensores() {
  LOGI("Initializing %d sensor devices", NUM_SENSORES);
  
  // Sensor 0: nivel balsa (modo tensión -> escala a metros)
  LOGD("Setting up Sensor 0 (Nivel Balsa) on ADC pin %d", 34);
  gSensores[0].pin_adc                 = 34;
  gSensores[0].state_topic_str         = "v1//things/esperanza/data/0";
  gSensores[0].retain                  = false;
  gSensores[0].full_scale_value        = 6.0f;     // 3.3 V ≡ 6.0 m
  
  LOGI("Sensor devices initialized successfully");
}

// ---- Lanzadores de tareas ----
void lanzarControles() {
  LOGI("Creating %d control tasks", NUM_CONTROLES);
  for (int i = 0; i < NUM_CONTROLES; ++i) {
    char name[24];
    snprintf(name, sizeof(name), "control_%d", i);
    
    BaseType_t result = xTaskCreate(
      task_control,     // función de tarea
      name,             // nombre
      8192,             // stack
      &gControles[i],   // parámetro
      3 + i,            // prioridad
      nullptr
    );
    
    if (result == pdPASS) {
      LOGI("Control task '%s' created successfully (priority: %d)", name, 3 + i);
    } else {
      LOGE("Failed to create control task '%s'", name);
    }
  }
}

void lanzarSensores() {
  LOGI("Creating %d sensor tasks", NUM_SENSORES);
  for (int i = 0; i < NUM_SENSORES; ++i) {
    char name[24];
    snprintf(name, sizeof(name), "sensor_%d", i);
    
    BaseType_t result = xTaskCreate(
      task_sensor,      // función de tarea
      name,
      8192,
      &gSensores[i],
      2,                // prioridad
      nullptr
    );
    
    if (result == pdPASS) {
      LOGI("Sensor task '%s' created successfully (priority: 2)", name);
    } else {
      LOGE("Failed to create sensor task '%s'", name);
    }
  }
}

// void lanzarControl()
// {
//   Control controlPozo;
//   controlPozo.pin = 10;
//   controlPozo.cmd_topic_str = "casa/riego/pozo/cmd";
//   controlPozo.state_topic_str = "casa/riego/pozo/state";
//   controlPozo.schedule_topic_str = "ESPERANZA/programacion/pozo";
//   for (int i = 0; i < 96; ++i)
//   {
//     controlPozo.programacion[i] = -1; // todo OFF
//   }

//   xTaskCreate(task_control, "control_pozo", 8192, &controlPozo, 4, NULL);

//   Control controlBalsa;
//   controlBalsa.pin = 15;
//   controlBalsa.cmd_topic_str = "casa/riego/balsa/cmd";
//   controlBalsa.state_topic_str = "casa/riego/balsa/state";
//   controlBalsa.schedule_topic_str = "ESPERANZA/programacion/balsa";
//   for (int i = 0; i < 96; ++i)
//   {
//     controlBalsa.programacion[i] = -1; // todo OFF
//   }

//   xTaskCreate(task_control, "control_balsa", 8192, &controlBalsa, 3, NULL);
// }

// void lanzarSensorNivelBalsa()
// {
//   Sensor sensorNivelBalsa;
//   sensorNivelBalsa.pin_adc = 34;
//   sensorNivelBalsa.state_topic_str = "v1//things/esperanza/data/0";
//   sensorNivelBalsa.retain = false;
//   sensorNivelBalsa.full_scale_value = 6.0f; // 0..6 m

//   xTaskCreate(task_sensor, "sensor_nivel_balsa", 8192, &sensorNivelBalsa, 2, NULL);
// }
