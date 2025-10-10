#pragma once
#include <PubSubClient.h>

// Cada instancia de Control controla su propio relé y su propia programación
struct Control
{
    int pin;
    const char *cmd_topic_str;      // p.ej. "pozo/1/cmd"
    const char *state_topic_str;    // p.ej. "pozo/1/state"
    const char *schedule_topic_str; // p.ej. "pozo/1/schedule" (JSON con 96 ints)

    // Programación actual: -1 = OFF, 1 = ON (o el convenio que utilices)
    int8_t programacion[96];

    // Programación pendiente (recibida por la tarde). Se aplica a las 00:00.
    int8_t pendingProgramacion[96];
    bool pendingValid = false;

    // Guardas para aplicar una sola vez a medianoche
    // Opción A (si tienes día juliano o fecha): guarda el "día aplicado".
    // Opción B (sin fecha): usa un latch que se desactiva en 23:00+ y activa en 00:00.
    bool appliedTodayLatch = false;
};

void task_control(void *params);
