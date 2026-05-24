#pragma once
#include <Arduino.h>

/**
 * Solo declaramos las funciones si el modo de asistencia está habilitado
 * en el archivo platformio.ini mediante -D ENABLE_ASSIST_MONITOR
 */
#ifdef ENABLE_MONITOR_ASSIST

typedef void (*HeartRateCallback)(uint8_t bpm, uint8_t flags);

/**
 * Inicializa el escaneo de la banda de pulso y la lógica de monitorización.
 * Este punto de entrada se debe invocar al final del setup() en main.cpp.
 */
void setupHeartRateSensor();

void bleSetHeartRateCallback(HeartRateCallback callback);

#else

typedef void (*HeartRateCallback)(uint8_t bpm, uint8_t flags);
static inline void setupHeartRateSensor() {}
static inline void bleSetHeartRateCallback(HeartRateCallback callback) {}

#endif