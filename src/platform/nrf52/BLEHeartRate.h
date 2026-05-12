#pragma once
#include <Arduino.h>

/**
 * Inicializa el escaneo de la banda de pulso y la UART de depuración.
 * Se llama una sola vez desde el setup del Bluetooth.
 */
void setupHeartRateSensor();

/**
 * Tarea de actualización
 */
void loopHeartRateSensor();