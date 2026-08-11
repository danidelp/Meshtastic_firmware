#include "MonitorAssistModule.h"
#include "DebugConfiguration.h"
#include "main.h"

#ifdef ENABLE_MONITOR_ASSIST

#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/portnums.pb.h"

#include <Fsm.h>

#include "BLESensor/HRBandSensor.h"

#ifdef HAS_QMA6100P
#include "motion/QMA6100PSensor.h"
#endif
#include "gps/RTC.h"
#include "buzz/buzz.h"


// Definición de Eventos
#define EVENT_HRBAND_READY 1
#define EVENT_HRBAND_DISCONNECT 2
#define EVENT_HR_CRITICAL 3
#define EVENT_HR_NORMAL 4
#define EVENT_FALL_DETECTED 5
#define EVENT_MOTION_NORMAL 6
#define EVENT_BUTTON_STOP_EMERGENCY 7

#define TIMEOUT_EMERGENCIA_MS 7200000 // 120 minutos en milisegundos

#define ALERT_CHANNEL_NAME "AssistChann"

// Puntero global al módulo MonitorAssistModule
MonitorAssistModule* g_monitorAssistInstance = nullptr;

// Funciones on_enter, on_state y on_exit para cada estado
void MonitorAssistModule::initEnter() {
  
#ifdef MONITOR_ASSIST_SENDER
  // Inicializacion del sensor de pulso
  if (hrBandSensor) {
    hrBandSensor->init();
  }
#endif
  
#ifdef HAS_QMA6100P
  QMA6100PSensor::resetFallState();
#endif
}
void MonitorAssistModule::initState(){

  // Esperar a que llegue el evento de banda de HR conectada, 
  // es la unica transicion valida para ir a NormalState.
  // El evento se lanza desde el callback del observer onHRBandConnection()

  if(!g_monitorAssistInstance->hrBandConnected){
    LOG_INFO("FSM [initState] Waiting for HR Band Sensor...");
  }

}
void MonitorAssistModule::initExit(){
  LOG_INFO("FSM [initExit] Exiting INIT state");
}

void MonitorAssistModule::normalEnter(){
  LOG_INFO("FSM [normalEnter] Entering NORMAL state");
  
#ifdef HAS_QMA6100P
  // Siempre que pasamos a estado NORMAL (ya sea desde INIT o volviendo de EMERGENCY), 
  // aseguramos que el hardware del acelerómetro se reinicie y olvide caídas viejas.
  QMA6100PSensor::resetFallState();
#endif

  if (g_monitorAssistInstance) {
      g_monitorAssistInstance->alertaCaida = false;
      g_monitorAssistInstance->alertaPulso = false;

      // Forzar el envio de un paquete nada mas entrar en estado normal
      g_monitorAssistInstance->lastSendTime = millis() - INTERVALO_ENVIO_NORMAL_MS;

      // Si volvemos a NORMAL desde EMERGENCY y la pulsera se había desconectado,
      // el evento de desconexión se ignoró para no abortar la emergencia, saltamos a INIT.
      if (!g_monitorAssistInstance->hrBandConnected) {
          LOG_WARN("FSM [normalEnter] HRBand disconnected. Back to INIT STATE...");
          monitorAssistFSM.trigger(EVENT_HRBAND_DISCONNECT);
      }
  }
}
void MonitorAssistModule::normalState(){

  static bool first_time_log = true;
  if(first_time_log){
    LOG_INFO("FSM [normalState] in Normal state...");
    first_time_log = false;
  } 
  
  // Envio en modo normal cada 30 minutos
  if (g_monitorAssistInstance) {
      uint32_t now = millis();
      if (now - g_monitorAssistInstance->lastSendTime >= INTERVALO_ENVIO_NORMAL_MS && hrBandSensor) {
          uint8_t macroBpm = hrBandSensor->getAccumHR();
          
          // Evitar mandar el pulso al comienzo del estado normal si aun no se ha estabilizado (0 BPM)
          // a menos que no estemos conectados a la banda (en cuyo caso sí notificamos la falta de conexion con 0)
          if (macroBpm > 0 || !g_monitorAssistInstance->hrBandConnected) {
              uint8_t flags = 0;
              if (!g_monitorAssistInstance->hrBandConnected) flags |= FLAG_HR_DISCONNECTED;
              LOG_INFO("FSM [normalState] Sending Assist Telemetry. Heart Rate: %d BPM", macroBpm);
              g_monitorAssistInstance->sendAssistTelemetry(macroBpm, flags);
              g_monitorAssistInstance->lastSendTime = now;
          }
      }
  }
}
void MonitorAssistModule::normalExit()
{
  LOG_INFO("FSM [normalExit] Exiting NORMAL state");
}

void MonitorAssistModule::emergencyEnter()
{
  LOG_INFO("FSM [emergencyEnter] Entering EMERGENCY state");

  if (g_monitorAssistInstance) {
      g_monitorAssistInstance->emergenciaStartTimer = millis();
      // Forzar envío inmediato
      g_monitorAssistInstance->lastSendTime = millis() - MonitorAssistModule::INTERVALO_ENVIO_ALERTA_INMEDIATA_MS;
  }
}

void MonitorAssistModule::emergencyState()
{
  static bool first_time_log = true;
  if(first_time_log){
    LOG_INFO("FSM [emergencyState] in Emergency state...");
    first_time_log = false;
  } 

  if (!g_monitorAssistInstance) return;

  uint32_t now = millis();

  // Comprobar cancelación manual al pulsar el botón
  if (g_monitorAssistInstance->buttonAbortEmergency) {
    LOG_INFO("FSM [emergencyState] Button pressed. Canceling emergency.");
    monitorAssistFSM.trigger(EVENT_BUTTON_STOP_EMERGENCY);

#ifdef HAS_QMA6100P
    QMA6100PSensor::resetFallState();
#endif
    g_monitorAssistInstance->buttonAbortEmergency = false;
    return;
  }


  // Periodic sending logic in emergency
  uint32_t tiempoEnEmergencia = now - g_monitorAssistInstance->emergenciaStartTimer;
  uint32_t intervaloActual = (tiempoEnEmergencia < MonitorAssistModule::INTERVALO_ENVIO_ALERTA_SECUNDARIA_MS) 
                              ? MonitorAssistModule::INTERVALO_ENVIO_ALERTA_INMEDIATA_MS 
                              : MonitorAssistModule::INTERVALO_ENVIO_ALERTA_SOSTENIDA_MS;

  if (now - g_monitorAssistInstance->lastSendTime >= intervaloActual) {
      uint8_t flags = 0;
      if (g_monitorAssistInstance->alertaCaida) flags |= FLAG_FALL_DETECTED;
      if (g_monitorAssistInstance->alertaPulso) flags |= FLAG_HR_RISK;
      if (!g_monitorAssistInstance->hrBandConnected) flags |= FLAG_HR_DISCONNECTED;

      uint8_t bpm = hrBandSensor ? hrBandSensor->getInstantHR() : 0;
      
      LOG_INFO("FSM [stateEmergency] Sending Emergency Assist Telemetry. BPM: %d, Flags: %s%s%s", bpm, 
        g_monitorAssistInstance->alertaCaida ? "[FALL]" : "",
        g_monitorAssistInstance->alertaPulso ? "[HR_RISK]" : "",
        !g_monitorAssistInstance->hrBandConnected ? "[HR_DISC]" : "");

      g_monitorAssistInstance->sendAssistTelemetry(bpm, flags);
      
      g_monitorAssistInstance->lastSendTime = now;
  }
}

void MonitorAssistModule::emergencyExit()
{
  LOG_INFO("FSM [emergencyExit] Exiting EMERGENCY state");

  // Apagar el LED. Meshtastic seguira controlando el led como hasta ahora en su propio hilo.
  digitalWrite(PIN_LED1, !LED_STATE_ON); 

  // Parar el sonido del zumbador
  #ifdef PIN_BUZZER
  noTone(PIN_BUZZER);
  #endif

  if (g_monitorAssistInstance) {
      g_monitorAssistInstance->alertaCaida = false;
      g_monitorAssistInstance->alertaPulso = false;
      g_monitorAssistInstance->lastSendTime = millis(); // Resetear temporizador normal
  }
}

// Definición de los Estados (on_enter, on_state, on_exit, name)
State stateInit(MonitorAssistModule::initEnter, MonitorAssistModule::initState, MonitorAssistModule::initExit, "INIT");
State stateNormal(MonitorAssistModule::normalEnter, MonitorAssistModule::normalState, MonitorAssistModule::normalExit, "NORMAL");
State stateEmergency(MonitorAssistModule::emergencyEnter, MonitorAssistModule::emergencyState, MonitorAssistModule::emergencyExit, "EMERGENCY");

// Instancia de la Máquina de Estados
Fsm monitorAssistFSM(&stateInit);

MonitorAssistModule::MonitorAssistModule()
    : ProtobufModule("MonitorAssist", meshtastic_PortNum_PRIVATE_APP,
                     meshtastic_MonitorAssistTelemetry_fields),
      concurrency::OSThread("MonitorAssist") {

  g_monitorAssistInstance = this;

  // Añadir transiciones de estado permitidas en base a los eventos recibidos
  monitorAssistFSM.add_transition(&stateInit, &stateNormal, EVENT_HRBAND_READY, NULL, "HR band connected");
  monitorAssistFSM.add_transition(&stateNormal, &stateInit, EVENT_HRBAND_DISCONNECT, NULL, "HR band disconnected.");
  monitorAssistFSM.add_transition(&stateNormal, &stateEmergency, EVENT_HR_CRITICAL, NULL, "HR emergency detected");
  monitorAssistFSM.add_transition(&stateNormal, &stateEmergency, EVENT_FALL_DETECTED, NULL, "Fall emergency detected");
  monitorAssistFSM.add_transition(&stateEmergency, &stateNormal, EVENT_HR_NORMAL, NULL, "Emergency ended. HR back to normal.");
  monitorAssistFSM.add_transition(&stateEmergency, &stateNormal, EVENT_MOTION_NORMAL, NULL, "Emergency ended. Motion detected.");
  monitorAssistFSM.add_transition(&stateEmergency, &stateNormal, EVENT_BUTTON_STOP_EMERGENCY, NULL, "Emergency ended. Button pressed.");
  monitorAssistFSM.add_timed_transition(&stateEmergency, &stateNormal, TIMEOUT_EMERGENCIA_MS, NULL, "Emergency ended. Timeout expired.");
  

  // Forzamos que el primer envío ocurra rápido al arrancar en modo normal
  lastSendTime = millis() - INTERVALO_ENVIO_NORMAL_MS;
  alertaCaida = false;
  alertaPulso = false;

#ifdef MONITOR_ASSIST_SENDER
  // Inicializacion del sensor de banda
  if (!hrBandSensor) {
    hrBandSensor = new HRBandSensor();
  }
  hrEmergencyObserver = new CallbackObserver<MonitorAssistModule, const void *>(
      this, &MonitorAssistModule::onHeartRateEmergency);
  hrEmergencyObserver->observe(&hrBandSensor->hrEmergencyObservable);
  LOG_INFO("[MonitorAssist] Observer de pulso cardiaco configurado.");

  hrBandConnectionObserver = new CallbackObserver<MonitorAssistModule, const void *>(
      this, &MonitorAssistModule::onHRBandConnection);
  hrBandConnectionObserver->observe(&hrBandSensor->bandConnectionObservable);
  LOG_INFO("[MonitorAssist] Observer de conexion de banda configurado.");
#endif

#ifdef HAS_QMA6100P
  // Suscribirnos como observadores del sensor de movimiento para que nos notifique ante las caidas
  QMA6100PSingleton *imu = QMA6100PSingleton::GetInstance();
  if (imu) {
    imuFallObserver = new CallbackObserver<MonitorAssistModule, const void *>(
        this, &MonitorAssistModule::onFall);
    imuFallObserver->observe(imu);
    LOG_INFO("[MonitorAssist] Observer de caidas configurado.");
  }
#endif

  // Suscribirnos como observadores del InputBroker para gestionar los eventos del boton
  if (inputBroker) {
      buttonObserver = new CallbackObserver<MonitorAssistModule, const InputEvent *>(
          this, &MonitorAssistModule::onButtonPressed);
      buttonObserver->observe(inputBroker);
      LOG_INFO("[MonitorAssist] Observer de botones configurado.");
  }

}

int32_t MonitorAssistModule::runOnce() {

  // El entorno de meshtastic aun no está configurado, esperamos 5 segundos
  if (!service || myNodeInfo.my_node_num == 0) {
    return 5000;
  }

#ifdef ARCH_NRF52
  // Asegurarnos de que PowerFSM ha encendido el hardware Bluetooth antes de continuar, evaluar en 2 segundos
  if (nrf52Bluetooth == nullptr) {
    return 2000;
  }
#endif

  // Region no configurada aun. Esperar a que esté configurada consultando cada 10 segundos
  // Sin esto, no inicializamos el modulo de asistencia.
  // Dependiendo del modelo del nodo, se deberá configurar la región desde la app movil
  if (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
    return 10000; 
  }

  // No continuar si hay un reinicio del sistema pendiente
  // Esto evita problemas con disableBluetooth() ejecutandose desde el AdminModule
  // Calcular el tiempo hasta el reinicio e irnos a dormir
  if (rebootAtMsec != 0) {
    uint32_t now = millis();
    if (rebootAtMsec > now) {
      return rebootAtMsec - now; // Dormir exactamente hasta el momento del reinicio
    }
    return 10000;
  }

  monitorAssistFSM.run_machine();

  if (monitorAssistFSM.getState() == &stateEmergency) {
      // Si estamos en el estado de emergencia, hacemos parpadear el LED cada 0.4 segundos
      // Controlamos la frecuencia del parpadeo con el return 500ms del runOnce
      static bool led_estado = false;
      led_estado = !led_estado;
      digitalWrite(PIN_LED1, led_estado ? LED_STATE_ON : !LED_STATE_ON);

      // En estado de alarma, hacer sonar el zumbador como si fuera una sirena 
      if (led_estado) {
          playBeep();
      } else {
          playBoop(); 
      }
      
      return 400;
  } else {
      // Modo Normal: No tocamos el LED, dejamos que Meshtastic lo controle
      return 2000;
  }
}

int MonitorAssistModule::onFall(const void *arg) {
  
  bool fall = (bool)arg;

  if (fall) {
      LOG_INFO("[MonitorAssist] Caida detectada en la IMU. El usuario no se mueve.");
      if (g_monitorAssistInstance) {
          g_monitorAssistInstance->alertaCaida = true;
          monitorAssistFSM.trigger(EVENT_FALL_DETECTED);
      }
  } else {
      LOG_INFO("[MonitorAssist] Usuario recuperado de la caida.");
      if (g_monitorAssistInstance) {
          monitorAssistFSM.trigger(EVENT_MOTION_NORMAL);
      }
  }
  return 0;
}

int MonitorAssistModule::onHeartRateEmergency(const void *arg) {
  
  bool heartRateEmergency = (bool)arg;
  
  if (heartRateEmergency) {
      LOG_INFO("[MonitorAssist] Detectado pulso critico");
      if (g_monitorAssistInstance) {
          g_monitorAssistInstance->alertaPulso = true;
          monitorAssistFSM.trigger(EVENT_HR_CRITICAL);
      }
  } else {
      LOG_INFO("[MonitorAssist] Pulso estable recuperado");
      if (g_monitorAssistInstance) {
          monitorAssistFSM.trigger(EVENT_HR_NORMAL);
      }
  }
  return 0;
}

int MonitorAssistModule::onHRBandConnection(const void *arg) {
  bool connected = (bool)arg;
  if (connected) {
      hrBandConnected = true;
      LOG_INFO("[MonitorAssist] HR Band connected...");
      monitorAssistFSM.trigger(EVENT_HRBAND_READY);
  } else {
      hrBandConnected = false;
      LOG_INFO("[MonitorAssist] HR Band disconnected...");
      monitorAssistFSM.trigger(EVENT_HRBAND_DISCONNECT);
  }
  return 0;
}

int MonitorAssistModule::onButtonPressed(const InputEvent *event) {

    if (event->inputEvent == INPUT_BROKER_USER_PRESS) {
        if (monitorAssistFSM.getState() == &stateEmergency) {
            LOG_INFO("[MonitorAssist] Detectada pulsacion del boton. Abortando emergencia...");
            buttonAbortEmergency = true;
        }
    }
    return 0;
}

void MonitorAssistModule::sendAssistTelemetry(uint8_t bpm, uint8_t flags) {

  meshtastic_MonitorAssistTelemetry msg =
      meshtastic_MonitorAssistTelemetry_init_default;

  msg.heart_rate = bpm;
  msg.assist_flags = flags;
  msg.timestamp = getValidTime(RTCQuality::RTCQualityDevice);

  msg.lat = localPosition.latitude_i;
  msg.lon = localPosition.longitude_i;

  meshtastic_MeshPacket *p = router->allocForSending();
  if (p == nullptr) {
    LOG_ERROR("No se puede enviar el mensaje de asistencia.");
    return;
  }

  p->want_ack = false;
  p->channel = channels.getByName(ALERT_CHANNEL_NAME).index;
  p->decoded.portnum = meshtastic_PortNum_PRIVATE_APP; 
 
  p->decoded.payload.size = pb_encode_to_bytes(
      p->decoded.payload.bytes, 
      sizeof(p->decoded.payload.bytes), 
      &meshtastic_MonitorAssistTelemetry_msg, 
      &msg);
  if (p->decoded.payload.size == 0) {
    LOG_ERROR("Error Nanopb codificando mensaje de asistencia");
    packetPool.release(p);
    return;
  }

  if (service) {
    service->sendToMesh(p, RX_SRC_LOCAL, true);
  } else {
    packetPool.release(p);
  }
}

bool MonitorAssistModule::handleReceivedProtobuf(
    const meshtastic_MeshPacket &mp, meshtastic_MonitorAssistTelemetry *msg) {
  
  LOG_INFO("Received Assist message from Node: 0x%x", mp.from);
  LOG_INFO("   - Heart Rate: %d BPM", msg->heart_rate);
  
  bool esCaida = (msg->assist_flags & FLAG_FALL_DETECTED);
  bool esRiesgoPulso = (msg->assist_flags & FLAG_HR_RISK);

  if (esCaida) {
      LOG_INFO("   - Reason: Fall detected.");
      playComboTune();
      delay(300);
      playComboTune();
  }
  
  if (esRiesgoPulso) {
      LOG_INFO("   - Reason: Heart Rate Risk.");
      for (int i = 0; i < 4; i++) {
          playLongBeep();
          delay(150);
      }
  }

  return true;
}

#endif // ENABLE_MONITOR_ASSIST