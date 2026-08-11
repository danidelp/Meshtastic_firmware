#include "HRBandSensor.h"

#ifdef ENABLE_MONITOR_ASSIST
#include <bluefruit.h>

HRBandSensor* hrBandSensor = nullptr;

HRBandSensor::HRBandSensor() : concurrency::OSThread("HRBandThread") {
    q_instant_bpms = xQueueCreate(20, sizeof(uint8_t));
}

static BLEClientService hrService(UUID16_SVC_HEART_RATE);
static BLEClientCharacteristic hrMeasurement(UUID16_CHR_HEART_RATE_MEASUREMENT);
static BLEClientService batteryService(UUID16_SVC_BATTERY);
static BLEClientCharacteristic batteryLevel(UUID16_CHR_BATTERY_LEVEL);

#define HRBAND_NAME "HW9 48857"

static void hr_notify_callback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    
    if (len < 2) return;
    uint8_t flags = data[0];
    uint8_t bpm = (flags & 0x01) ? (data[2] << 8 | data[1]) : data[1];
    
    // Notify the thread to process the new BPM value received from BLE
    if (hrBandSensor) {
        hrBandSensor->hr_from_callback(bpm);
    }
}

void HRBandSensor::hr_from_callback(uint8_t hr) {
    last_instant_hr_time = millis();

    // Notificar en la cola de bpms que hay un nuevo dato
    if (q_instant_bpms) {
        xQueueSendFromISR(q_instant_bpms, &hr, NULL);
    }
}

int32_t HRBandSensor::runOnce() {
    uint8_t hr;
    bool hayDatosNuevos = false;

    if (!q_instant_bpms) return 500;

    // Get all heartbeats accumulated in the queue from the callback
    while (xQueueReceive(q_instant_bpms, &hr, 0) == pdTRUE) {
        hayDatosNuevos = true;
        
        // Add the HR values to the circular buffer
        instant_hr_buffer[index_hr_buffer] = hr;
        index_hr_buffer = (index_hr_buffer + 1) % INSTANT_HR_BUFFER_SIZE;
        if (total_instant_hr < INSTANT_HR_BUFFER_SIZE) total_instant_hr++;
    }

    if (!hayDatosNuevos) {
        return 500; // Wake up in 500ms if no data
    }

    // Process the instant HR values discarting zero measures
    // The band sends zeroes when first connected and when no pulse is detected 
    uint32_t instantAccumValues = 0;
    uint8_t zeroValues = 0;
    uint8_t validValues = 0;

    // Calculate the average HR from the circular buffer
    for (uint8_t i = 0; i < total_instant_hr; i++) {
        if (instant_hr_buffer[i] == 0) zeroValues++;
        else {
            instantAccumValues += instant_hr_buffer[i];
            validValues++;
        }
    }

    if (zeroValues == total_instant_hr) {
        instant_hr_avg = 0;
    } else if (validValues > 0) {
        instant_hr_avg = (uint8_t)(instantAccumValues / validValues);
    }

    // Accumulate the instant HR values for long term average
    if (instant_hr_avg > 0) {
        accum_hr_value += instant_hr_avg;
        total_accum_hr++;
    }

    bool currentRisk = evalHeartRateRisk();
    if (currentRisk && !lastRiskState) {
        hrEmergencyObservable.notifyObservers((void*)true);
    } else if (!currentRisk && lastRiskState) {
        hrEmergencyObservable.notifyObservers((void*)false);
    }
    lastRiskState = currentRisk;

    return HR_MEASUREMENT_PERIOD;
}

bool HRBandSensor::evalHeartRateRisk() const {

    if (total_instant_hr < INSTANT_HR_BUFFER_SIZE || !isGoodSignal()) return false;
    
    if (instant_hr_avg == 0) {
        // If there was never a valid pulse (total_accum_hr == 0), 
        // the 0 is because the band is calibrating, not a cardiac arrest.
        if (total_accum_hr == 0) return false; 
        return true; // Cardiac arrest
    }
    
    if (instant_hr_avg < UMBRAL_PULSO_BAJO || instant_hr_avg > UMBRAL_PULSO_ALTO) return true;
    return false; 
}

bool HRBandSensor::isGoodSignal() const {
    // Determinar si las muestras son correctas en base a que la pulsera esté conectada 
    // y que la ultima muestra sea de hace menos de 10 segundos.
    // Evita falsas alarmas cuando se pierde la conexión con la pulsera.
    return (band_connected && (millis() - last_instant_hr_time < 10000));
}

void HRBandSensor::resetBuffers() {
    index_hr_buffer = 0;
    total_instant_hr = 0;
    instant_hr_avg = 0;
    last_instant_hr_time = 0;
    
    accum_hr_value = 0;
    total_accum_hr = 0;

    memset(instant_hr_buffer, 0, sizeof(instant_hr_buffer));
    if (q_instant_bpms) {
        xQueueReset(q_instant_bpms);
    }
}

// ============================================================================
// CONFIGURACIÓN DE LOS CALLBACKS BLUETOOTH PARA CONECTARSE A LA BANDA
// ============================================================================

static void scan_callback(ble_gap_evt_adv_report_t* report) {
    
    if (hrBandSensor && hrBandSensor->isConnected()) return;

    if (Bluefruit.Scanner.checkReportForService(report, hrService)) {
        
    uint8_t buffer[32];
    memset(buffer, 0, sizeof(buffer));
    
        // Filtrar por nombre de la banda de HR y solo conectarnos a la nuestra configurada
        if (Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, buffer, sizeof(buffer))) {

        if (strstr((char*)buffer, HRBAND_NAME) != NULL) {
            LOG_INFO("[HRBandSensor] Connecting to band %s...", HRBAND_NAME);
            Bluefruit.Central.connect(report);
            return;
        }
    }
    }
    
    Bluefruit.Scanner.resume();
}

static void battery_notify_callback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    if (len >= 1 && hrBandSensor) {
        hrBandSensor->updateBateryLevel(data[0]);
    }
}

void HRBandSensor::updateBateryLevel(uint8_t nivel) {
    if (battery_level != nivel) {
        LOG_INFO("[HRBandSensor] HW9 battery level: %d%%", nivel);
        battery_level = nivel;
    }
}

static void connect_callback(uint16_t conn_handle) {
    
    // Servicio de frecuencia cardiaca 0x180d y caracteristica de medida 0x2a37
    // Habilitar las notificaciones
    if (hrService.discover(conn_handle) && hrMeasurement.discover()) {
        hrMeasurement.enableNotify();
    }
    // Servicio de bateria 0x180f y caracteristica de nivel de bateria 0x2a19
    // Habilitar las notificaciones
    if (batteryService.discover(conn_handle) && batteryLevel.discover()) {
        batteryLevel.enableNotify();
    }

    if (hrBandSensor) {
        hrBandSensor->setConnected(true);

        // Detener el escaner para liberar la radio y evitar problemas de concurrencia Dual Role
        Bluefruit.Scanner.stop();

        // Forzamos al hilo a dormir 5 segundos tras la conexión para dar tiempo a que la cola se llene de muestras
        // Al encender la banda, esta envía 0s hasta estabilizarse
        hrBandSensor->setIntervalFromNow(5000);
        hrBandSensor->bandConnectionObservable.notifyObservers((void*)true);
    }
}

static void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
    if (hrBandSensor) {
        hrBandSensor->setConnected(false);
        hrBandSensor->resetBuffers(); 
        hrBandSensor->bandConnectionObservable.notifyObservers((void*)false);

        if (hrBandSensor->isShutdown()) {
            return;
        }
    }
    Bluefruit.Scanner.start(0);
}

bool HRBandSensor::init() {

    resetBuffers();

    if (bleInit) {
        return true;
    } else {
        LOG_INFO("Init HRBandSensor...");
        bleInit = true;
        hrService.begin();
        hrMeasurement.setNotifyCallback(hr_notify_callback);
        hrMeasurement.begin();
        batteryService.begin();
        batteryLevel.setNotifyCallback(battery_notify_callback);
        batteryLevel.begin();
        Bluefruit.Central.setConnectCallback(connect_callback);
        Bluefruit.Central.setDisconnectCallback(disconnect_callback);
        Bluefruit.Scanner.setRxCallback(scan_callback);
        Bluefruit.Scanner.restartOnDisconnect(true);

        // Ajustar el intervalo de escaneo en unidades de 0.625 ms. 
        // 8000 * 0.625 = 5000 ms = 5 segundos de intervalo de escaneo
        // 1000 * 0.625 = 625 ms de intervalo de ventana
        Bluefruit.Scanner.setInterval(8000, 1000); 
        Bluefruit.Scanner.useActiveScan(true);
        Bluefruit.Scanner.start(0); 
    }
    
    return true;
}

void HRBandSensor::shutdown() {
    LOG_INFO("[HRBandSensor] Shutting down BLE Central operations...");
    shutdown_requested = true;
    Bluefruit.Scanner.stop();
}

uint8_t HRBandSensor::getAccumHR() {
    uint8_t macro = 0;
    if (total_accum_hr > 0) {
        macro = (uint8_t)(accum_hr_value / total_accum_hr);
    } else {
        macro = instant_hr_avg; // Fallback al último conocido
    }

    accum_hr_value = 0;
    total_accum_hr = 0;
    return macro;
}

#endif // ENABLE_MONITOR_ASSIST