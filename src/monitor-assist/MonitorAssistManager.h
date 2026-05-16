#ifndef ASSIST_MANAGER_H
#define ASSIST_MANAGER_H

#include <stdint.h>

// Definimos la función que llamaremos desde el código de Bluetooth
void broadcastAssistData(uint8_t bpm, uint8_t flags);

#endif