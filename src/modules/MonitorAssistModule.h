#ifndef MONITOR_ASSIST_MODULE_H
#define MONITOR_ASSIST_MODULE_H

#include "mesh/ProtobufModule.h" 
#include "concurrency/OSThread.h" // <--- Añadimos el gestor de hilos nativo
#include <Arduino.h>
#include "mesh/generated/meshtastic/monitor_assist.pb.h" 


// Heredamos de ProtobufModule y privadamente de OSThread siguiendo las convenciones
class MonitorAssistModule : public ProtobufModule<meshtastic_MonitorAssistTelemetry>, private concurrency::OSThread 
{
private:
    static constexpr uint32_t NORMAL_SEND_INTERVAL_MS = 60000; // 5 min
    static constexpr uint32_t CRITICAL_SEND_INTERVAL_MS = 10000; // 10 s
    static constexpr uint32_t SAMPLE_WINDOW_MS = 10000; // Ventana de muestra para promedio
    static constexpr uint32_t HEART_RATE_STALE_MS = 15000; // Si no recibimos datos, no enviamos
    static constexpr uint8_t MAX_HR_SAMPLES = 20;

    uint8_t hrSamples[MAX_HR_SAMPLES] = {0};
    uint8_t hrSampleCount = 0;
    uint8_t hrSampleIndex = 0;
    uint32_t lastHeartRateTime = 0;
    uint32_t lastSendTime = 0;
    uint8_t lastSentAverage = 0;
    bool lastWasCritical = false;

    void sendAssistTelemetry(uint8_t bpm, uint8_t flags);
    void evaluateAndSend();

protected:
    // Callback nativo cuando llega un paquete a nuestro puerto
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_MonitorAssistTelemetry *msg) override;
    
    // El lazo periódico del hilo
    virtual int32_t runOnce() override; 

public:
    MonitorAssistModule();
    // Expuesto públicamente para que callbacks externos (p.ej. BLE) puedan notificar
    void onHeartRate(uint8_t bpm, uint8_t flags);
};

#endif