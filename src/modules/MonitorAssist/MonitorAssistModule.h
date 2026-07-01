#ifndef MONITOR_ASSIST_MODULE_H
#define MONITOR_ASSIST_MODULE_H

#include "mesh/ProtobufModule.h" 
#include "concurrency/OSThread.h"
#include <Arduino.h>
#include "mesh/generated/meshtastic/monitor_assist.pb.h" 
#include "Observer.h"
#include "input/InputBroker.h"

#include <Fsm.h>
#include <cstdint>
extern Fsm monitorAssistFSM;
extern State stateInit, stateNormal, stateEmergency;

// Macros de conversiones entre minutos y segundos a milisegundos
#define MINS_TO_MILIS(x) ((x) * 60000)
#define SECS_TO_MILIS(x) ((x) * 1000)

class MonitorAssistModule : public ProtobufModule<meshtastic_MonitorAssistTelemetry>, private concurrency::OSThread 
{
public:

private:
    // --- NUEVAS CONSTANTES DE TIEMPO DE TU ESTRATEGIA ---
    static constexpr uint32_t INTERVALO_NORMAL_MS = MINS_TO_MILIS(30);            // Envio regular cada 30 minutos 
    static constexpr uint32_t INTERVALO_ALERTA_INMEDIATA_MS = SECS_TO_MILIS(30);  // Alerta inmediata cada 30 segundos
    static constexpr uint32_t INTERVALO_ALERTA_SECUNDARIA_MS = MINS_TO_MILIS(2);  // Alerta secundaria cada 2 minutos 
    static constexpr uint32_t INTERVALO_ALERTA_SOSTENIDA_MS = MINS_TO_MILIS(10);  // Alerta sostenida cada 10 minutos 

    static constexpr uint8_t FLAG_FALL_DETECTED = 0x01;
    static constexpr uint8_t FLAG_HR_RISK = 0x02;
    static constexpr uint8_t FLAG_HR_DISCONNECTED = 0x04;

    uint32_t emergenciaStartTimer = 0; // Cronómetro de la emergencia activa
    
    bool alertaPulso = false;
    bool alertaCaida = false;
    
    uint32_t lastSendTime = 0;

    // Métodos de evaluación y lógica interna
    void sendAssistTelemetry(uint8_t bpm, uint8_t flags);

    // Observer helper for IMU fall notifications
    CallbackObserver<MonitorAssistModule, const void *> *imuFallObserver = nullptr;
    int onFall(const void *arg);

    // Observer helper for HR pulse emergencies
    CallbackObserver<MonitorAssistModule, const void *> *hrEmergencyObserver = nullptr;
    int onHeartRateEmergency(const void *arg);

    // Observer helper for HR band connected
    CallbackObserver<MonitorAssistModule, const void *> *hrBandConnectionObserver = nullptr;
    int onHRBandConnection(const void *arg);
    bool hrBandConnected = false;

    // Observer helper for button press events
    CallbackObserver<MonitorAssistModule, const InputEvent *> *buttonObserver = nullptr;
    int onButtonPressed(const InputEvent *event);
    bool buttonAbortEmergency = false;

protected:
    // Callback nativo cuando llega un paquete a nuestro puerto privado
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_MonitorAssistTelemetry *msg) override;
    
    // El lazo periódico del hilo (despierta cada 2 segundos)
    virtual int32_t runOnce() override; 

public:
    MonitorAssistModule();

    static void initEnter();
    static void initState();
    static void initExit();
    
    static void normalEnter();
    static void normalState();
    static void normalExit();
    
    static void emergencyEnter();
    static void emergencyState();
    static void emergencyExit();
};

extern MonitorAssistModule* g_monitorAssistInstance;

#endif // MONITOR_ASSIST_MODULE_H