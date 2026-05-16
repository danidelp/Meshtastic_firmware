#include "configuration.h"

// Envolvemos todo el archivo
#ifdef ENABLE_MONITOR_ASSIST

#include "BLEHeartRate.h"
#include <bluefruit.h>
#include "MonitorAssistManager.h" // Importante para enviar a la red Mesh

// Variables de estado
static bool is_connected = false;

// Variables para la media móvil
#define HR_WINDOW_SIZE 5
static uint8_t hr_history[HR_WINDOW_SIZE];
static uint8_t hr_index = 0;
static uint32_t last_log_time = 0;

// Variables de control de envío LoRa
static uint32_t last_mesh_send = 0;
static uint8_t last_sent_average = 0;

// Instancia del servicio de Heart Rate
BLEClientService hrService(UUID16_SVC_HEART_RATE);
BLEClientCharacteristic hrMeasurement(UUID16_CHR_HEART_RATE_MEASUREMENT);

// --- LÓGICA DE CONTROL DE ENVÍO ---

void checkAndSendLoRa(uint8_t current_avg) {
    uint32_t now = millis();
    
    // Definición de umbrales según diseño de TFM
    // bool is_critical = (current_avg > 160 || current_avg < 40);
    bool is_critical = true;
    uint32_t interval = is_critical ? 10000 : 300000; // 20000 = 20s (Crítico) o 5min (Normal)

    // Enviamos si toca por tiempo O si hay un cambio brusco (+-20 BPM)
    if (now - last_mesh_send > interval || abs(current_avg - last_sent_average) > 20) {
        
        uint8_t flags = 0;
        if (is_critical) flags |= 0x01; // Bit 0: Alerta de pulso
        
        // Llamada a tu nuevo sistema de monitorización y asistencia
        broadcastAssistData(current_avg, flags);
        
        last_mesh_send = now;
        last_sent_average = current_avg;
        LOG_INFO(">>> [ASSIST] Telemetría enviada: %d BPM (Flags: %d)", current_avg, flags);
    }
}

// --- CALLBACKS BLE ---

void hr_notify_callback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    uint8_t bpm = (data[0] & 0x01) ? (data[2] << 8 | data[1]) : data[1];
    
    // Actualizar historial
    hr_history[hr_index] = bpm;
    hr_index = (hr_index + 1) % HR_WINDOW_SIZE;

    uint32_t now = millis();
    if (now - last_log_time >= 3000) { 
        int sum = 0;
        for(int i=0; i<HR_WINDOW_SIZE; i++) sum += hr_history[i];
        uint8_t average = sum / HR_WINDOW_SIZE;

        LOG_INFO(">>> [HRM] Instantáneo: %d | Media: %d BPM", bpm, average);
        
        // Verificar si toca enviar a la red LoRa
        checkAndSendLoRa(average);
        
        last_log_time = now;
    }
}

void scan_callback(ble_gap_evt_adv_report_t* report) {
    if (is_connected) return;

    if (Bluefruit.Scanner.checkReportForService(report, hrService)) {
        LOG_INFO("-> Pulsómetro encontrado, conectando...");
        Bluefruit.Central.connect(report);
    } else {
        Bluefruit.Scanner.resume();
    }
}

void connect_callback(uint16_t conn_handle) {
    is_connected = true;
    LOG_INFO("-> ¡Conectado al dispositivo!");
    
    if (hrService.discover(conn_handle)) {
        if (hrMeasurement.discover()) {
            if (hrMeasurement.enableNotify()) {
                LOG_INFO("-> Notificaciones de pulso activadas.");
            }
        }
    }
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
    is_connected = false; 
    LOG_INFO("HRM: Desconectado. Razón: 0x%02X", reason);
    memset(hr_history, 0, sizeof(hr_history));
    Bluefruit.Scanner.start(0);
}

// --- PUNTO DE ENTRADA ---

void setupHeartRateSensor() {
    LOG_INFO("Iniciando Modo Asistencia (BLE Dual Role)");

    hrService.begin();
    hrMeasurement.setNotifyCallback(hr_notify_callback);
    hrMeasurement.begin();

    Bluefruit.Central.setConnectCallback(connect_callback);
    Bluefruit.Central.setDisconnectCallback(disconnect_callback);
    
    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Scanner.restartOnDisconnect(true);
    Bluefruit.Scanner.setInterval(160, 80); 
    Bluefruit.Scanner.useActiveScan(true);
    Bluefruit.Scanner.start(0); 
}

#endif // ENABLE_ASSIST_MONITOR