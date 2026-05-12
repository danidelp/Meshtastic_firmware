#include "BLEHeartRate.h"
#include <bluefruit.h>
#include "configuration.h"

// Variables de estado
static bool is_connected = false;

// Variables para la media móvil (Ventana de 5 muestras)
#define HR_WINDOW_SIZE 5
static uint8_t hr_history[HR_WINDOW_SIZE];
static uint8_t hr_index = 0;
static uint32_t last_log_time = 0;

// Instancia del servicio de Heart Rate (Standard Bluetooth SIG)
BLEClientService hrService(UUID16_SVC_HEART_RATE);
BLEClientCharacteristic hrMeasurement(UUID16_CHR_HEART_RATE_MEASUREMENT);

// --- CALLBACKS ---

void hr_notify_callback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    // El estándar Bluetooth HR: data[1] suele ser el valor si el formato es 8-bit
    uint8_t bpm = (data[0] & 0x01) ? (data[2] << 8 | data[1]) : data[1];
    
    // 1. Guardar en el buffer para la media
    hr_history[hr_index] = bpm;
    hr_index = (hr_index + 1) % HR_WINDOW_SIZE;

    // 2. Procesar la media cada 3-5 segundos para no saturar trazas
    uint32_t now = millis();
    if (now - last_log_time >= 3000) { // Cada 3 segundos
        int sum = 0;
        for(int i=0; i<HR_WINDOW_SIZE; i++) sum += hr_history[i];
        uint8_t average = sum / HR_WINDOW_SIZE;

        LOG_INFO(">>> [HRM] Instantáneo: %d | Media (5 muestras): %d BPM", bpm, average);
        last_log_time = now;
    }
}

void scan_callback(ble_gap_evt_adv_report_t* report) {

    // Si ya estamos conectados, no intentamos conectar a otro
    if (is_connected) return;

    if (Bluefruit.Scanner.checkReportForService(report, hrService)) {
        LOG_INFO("-> Pulsómetro encontrado, conectando...");
        Bluefruit.Central.connect(report);
    } else {
        Bluefruit.Scanner.resume();
    }
}

void connect_callback(uint16_t conn_handle) {

    is_connected = true; // <--- Importante marcarlo aquí
    LOG_INFO("-> ¡Conectado al dispositivo!");
    
    if (hrService.discover(conn_handle)) {
        if (hrMeasurement.discover()) {
            // CAMBIO AQUÍ: La función es enableNotify(), no enableCharacteristicNotify()
            if (hrMeasurement.enableNotify()) {
                LOG_INFO("-> Notificaciones de pulso activadas.");
            } else {
                LOG_INFO("-> ERROR: No se pudieron activar notificaciones.");
            }
        } else {
            LOG_INFO("-> ERROR: No se encontró la característica de pulso.");
        }
    } else {
        LOG_INFO("-> ERROR: No se encontró el servicio de Heart Rate.");
    }
}


void disconnect_callback(uint16_t conn_handle, uint8_t reason) {

    is_connected = false; 
    LOG_INFO("HRM: Desconectado. Razón: 0x%02X", reason);

    // Reiniciar buffer de media para que no arrastre valores viejos
    memset(hr_history, 0, sizeof(hr_history));

    // El Scanner se reiniciará solo porque pusimos restartOnDisconnect(true) en setup
}

uint32_t last_mesh_send = 0;
uint8_t last_sent_average = 0;

void checkAndSendLoRa(uint8_t current_avg) {
    uint32_t now = millis();
    bool is_critical = (current_avg > 160 || current_avg < 40);
    uint32_t interval = is_critical ? 20000 : 300000; // 20s o 5min

    // Enviamos si ha pasado el tiempo O si hay un cambio brusco (ej. +-20 BPM)
    if (now - last_mesh_send > interval || abs(current_avg - last_sent_average) > 20) {
        
        // Aquí llamaríamos a la función de Meshtastic para enviar el paquete
        // sendMeshText(String("BPM: ") + String(current_avg)); 
        
        last_mesh_send = now;
        last_sent_average = current_avg;
        LOG_INFO(">>> ENVIANDO A RED MESH: % d BPM", current_avg);
    }
}

// --- PUNTO DE ENTRADA ---

void setupHeartRateSensor() {

    LOG_INFO("Iniciando BLE modo dual con banda HR");

    // Configuración específica de Bluefruit para modo Central
    hrService.begin();
    hrMeasurement.setNotifyCallback(hr_notify_callback);
    hrMeasurement.begin();

    Bluefruit.Central.setConnectCallback(connect_callback);
    Bluefruit.Central.setDisconnectCallback(disconnect_callback);
    
    // Iniciamos el escaneo
    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Scanner.restartOnDisconnect(true); // <--- Auto-reinicio al perder conexión

    // Ajuste de tiempos: Escaneo más relajado para no interferir con LoRa
    Bluefruit.Scanner.setInterval(160, 80); 
    Bluefruit.Scanner.useActiveScan(true);

    // Iniciar escaneo con un pequeño delay para dejar que el sistema asiente
    Bluefruit.Scanner.start(0); 

}

/*
2. Definición de Umbrales de Riesgo

Para tu TFM, puedes definir estos umbrales (preferiblemente configurables):
Estado      Rango BPM       Frecuencia LoRa     Prioridad Meshtastic
Normal      50 - 110        Cada 5 min          Low
Esfuerzo    110 - 150       Cada 1 min          Medium
RIESGO      < 40 o > 160    Instantáneo         High / Critical

3. ¿Emitir siempre o solo en riesgo?
Mi recomendación: EMITIR SIEMPRE, pero variando la frecuencia.

Si solo emites en riesgo, y el nodo se apaga o se sale de cobertura, 
el receptor no sabrá si el usuario está bien o si el sistema ha fallado (el "silencio" es ambiguo). 

Ver un dato actualizado, aunque sea de hace 3 minutos, da tranquilidad de que el sistema de rescate está operativo.

*/