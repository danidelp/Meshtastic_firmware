#include "configuration.h"

// Envolvemos todo el archivo
#ifdef ENABLE_MONITOR_ASSIST

#include "BLEHeartRate.h"
#include <bluefruit.h>

static HeartRateCallback heartRateCallback = nullptr;

// Variables de estado
static bool is_connected = false;

// Instancia del servicio de Heart Rate
BLEClientService hrService(UUID16_SVC_HEART_RATE);
BLEClientCharacteristic hrMeasurement(UUID16_CHR_HEART_RATE_MEASUREMENT);

// --- SOURCE: BLE HEART RATE EVENTS ---

void reportHeartRate(uint8_t bpm, uint8_t flags)
{
    if (heartRateCallback) {
        heartRateCallback(bpm, flags);
    } else {
        LOG_WARN("No hay callback de HeartRate registrado");
    }
}

// Permite a otros módulos (p.ej. MonitorAssistModule) registrar un callback
void bleSetHeartRateCallback(HeartRateCallback callback) {
    heartRateCallback = callback;
}

// --- CALLBACKS BLE ---

void hr_notify_callback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    uint8_t flags = data[0];
    uint8_t bpm = (flags & 0x01) ? (data[2] << 8 | data[1]) : data[1];
    
    // LOG_INFO(">>> [HRM] Notificacion BLE: %d BPM, flags=0x%02X", bpm, flags);
    reportHeartRate(bpm, flags);
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