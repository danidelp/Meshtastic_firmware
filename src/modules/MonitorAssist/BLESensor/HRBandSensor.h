#pragma once
#include "configuration.h"
#include <Arduino.h>

#ifdef ENABLE_MONITOR_ASSIST
#include "concurrency/OSThread.h"
#include "Observer.h"

class HRBandSensor : public concurrency::OSThread {
private:

    static constexpr uint32_t HR_MEASUREMENT_PERIOD = 5000;  // 5 seconds
    // Relacionar el tamaño del bufer de muestras instantaneas con el periodo del runOnce del sensor de HR
    // La banda emite cada 0.5s, cada segundo nos llegaran aprox 2 muestras
    static constexpr uint8_t INSTANT_HR_BUFFER_SIZE = HR_MEASUREMENT_PERIOD / 500; 
    static constexpr uint8_t UMBRAL_PULSO_BAJO = 40;  
    static constexpr uint16_t UMBRAL_PULSO_ALTO = 160; 

    // Instant Heart Rate Buffer and samples
    uint8_t instant_hr_buffer[INSTANT_HR_BUFFER_SIZE] = {0};
    uint8_t index_hr_buffer = 0;
    uint8_t total_instant_hr = 0;
    uint8_t instant_hr_avg = 0;
    volatile uint32_t last_instant_hr_time = 0;

    // Accumulated macro heart rate average for the last 20 minutes
    uint32_t accum_hr_value = 0;
    uint16_t total_accum_hr = 0;
    
    uint8_t battery_level = 100;
    bool bleInit = false;
    bool band_connected = false;
    bool lastRiskState = false;

    // FreeRTOS Queue for BPM samples
    QueueHandle_t q_instant_bpms;

protected:
    virtual int32_t runOnce() override;

public:
    Observable<const void*> hrEmergencyObservable;
    Observable<const void*> bandConnectionObservable;

    HRBandSensor();
    
    bool init();               
    void setConnected(bool connected) { band_connected = connected; }
    bool isConnected() const { return band_connected; }
    bool isGoodSignal() const;  

    // Flujo asíncrono seguro para ISR
    void hr_from_callback(uint8_t hr);
    void updateBateryLevel(uint8_t nivel);
    
    uint8_t getInstantHR() const { return instant_hr_avg; }
    uint8_t getAccumHR();
    bool evalHeartRateRisk() const;
    void resetBuffers();
};

extern HRBandSensor* hrBandSensor;

#endif // ENABLE_MONITOR_ASSIST