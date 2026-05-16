#ifndef ASSIST_PROTOCOL_H
#define ASSIST_PROTOCOL_H

#include <stdint.h>

#define PORT_ASSIST_APP 768

// Estructura empaquetada para evitar rellenos de memoria (padding)
typedef struct __attribute__((packed)) {
    uint8_t bpm;            // Pulso
    int32_t latitude;       // Latitud (grados * 1e7)
    int32_t longitude;      // Longitud (grados * 1e7)
    uint8_t assist_flags; // Bit 0: Pulso, Bit 1: IMU, Bit 2: SOS
} MonitorAssistTelemetryPacket;

#endif