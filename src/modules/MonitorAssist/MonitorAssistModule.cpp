#include "MonitorAssistModule.h"
#include "DebugConfiguration.h"

#ifdef ENABLE_MONITOR_ASSIST

#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/portnums.pb.h"

#include <Fsm.h>

#include "BLESensor/HRBandSensor.h"

#include "motion/QMA6100PSensor.h"

// Definición de Eventos
#define EVENT_HRBAND_READY 1
#define EVENT_HRBAND_DISCONNECT 2
#define EVENT_HR_CRITICAL 3
#define EVENT_HR_NORMAL 4
#define EVENT_FALL_DETECTED 5
#define EVENT_MOTION_NORMAL 6
#define EVENT_BUTTON_STOP_EMERGENCY 7

#define TIMEOUT_EMERGENCIA_MS 7200000 // 120 minutos en milisegundos

// Puntero global al módulo MonitorAssistModule
MonitorAssistModule* g_monitorAssistInstance = nullptr;

// Funciones on_enter, on_state y on_exit para cada estado

void MonitorAssistModule::initEnter() {
  LOG_INFO("FSM [initEnter] Initializing MonitorAssist Module...");
  // Inicializacion del sensor de pulso
  if (hrBandSensor) {
      hrBandSensor->init();
  }
  
  // Limpiamos cualquier estado "fantasma" que haya podido quedar en la IMU
  // antes de arrancar todo el sistema
  QMA6100PSensor::resetFallState();
}
void MonitorAssistModule::initState(){

  static bool first_time_log = true;
  if(first_time_log){
      LOG_INFO("FSM [initState] Waiting for HR Band Sensor...");
      first_time_log = false;
  }

  // Esperar a que llegue el evento de banda de HR conectada, 
  // es la unica transicion valida para ir a NormalState.
  // El evento se lanza desde el callback del observer onHRBandConnection()
}
void MonitorAssistModule::initExit(){
  LOG_INFO("FSM [initExit] Exiting INIT state");
}

void MonitorAssistModule::normalEnter(){
  LOG_INFO("FSM [normalEnter] Entering NORMAL state");
  
  // Siempre que pasamos a estado NORMAL (ya sea desde INIT o volviendo de EMERGENCY), 
  // aseguramos que el hardware del acelerómetro se reinicie y olvide caídas viejas.
  QMA6100PSensor::resetFallState();

  if (g_monitorAssistInstance) {
      g_monitorAssistInstance->alertaCaida = false;
      g_monitorAssistInstance->alertaPulso = false;

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
      if (now - g_monitorAssistInstance->lastSendTime >= INTERVALO_NORMAL_MS && hrBandSensor) {
          uint8_t macroBpm = hrBandSensor->getAccumHR();
          uint8_t flags = 0;
          if (!g_monitorAssistInstance->hrBandConnected) flags |= FLAG_HR_DISCONNECTED;
          LOG_INFO("FSM [normalState] Sending Assist Telemetry. Heart Rate: %d BPM", macroBpm);
          g_monitorAssistInstance->sendAssistTelemetry(macroBpm, flags);
          g_monitorAssistInstance->lastSendTime = now;
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
      g_monitorAssistInstance->lastSendTime = millis() - MonitorAssistModule::INTERVALO_ALERTA_INMEDIATA_MS;
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
      QMA6100PSensor::resetFallState();
      g_monitorAssistInstance->buttonAbortEmergency = false;
      return;
  }


  // Periodic sending logic in emergency
  uint32_t tiempoEnEmergencia = now - g_monitorAssistInstance->emergenciaStartTimer;
  uint32_t intervaloActual = (tiempoEnEmergencia < MonitorAssistModule::INTERVALO_ALERTA_SECUNDARIA_MS) 
                              ? MonitorAssistModule::INTERVALO_ALERTA_INMEDIATA_MS 
                              : MonitorAssistModule::INTERVALO_ALERTA_SOSTENIDA_MS;

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
  lastSendTime = millis() - INTERVALO_NORMAL_MS;
  alertaCaida = false;
  alertaPulso = false;

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
  LOG_INFO("[MonitorAssist] Observer de conexión de banda configurado.");

  // Suscribirnos como observadores del sensor de movimiento para que nos notifique ante las caidas
  QMA6100PSingleton *imu = QMA6100PSingleton::GetInstance();
  if (imu) {
    imuFallObserver = new CallbackObserver<MonitorAssistModule, const void *>(
        this, &MonitorAssistModule::onFall);
    imuFallObserver->observe(imu);
    LOG_INFO("[MonitorAssist] Observer de caidas configurado.");
  }

  // Suscribirnos como observadores del InputBroker para gestionar los eventos del boton
  if (inputBroker) {
      buttonObserver = new CallbackObserver<MonitorAssistModule, const InputEvent *>(
          this, &MonitorAssistModule::onButtonPressed);
      buttonObserver->observe(inputBroker);
      LOG_INFO("[MonitorAssist] Observer de botones configurado.");
  }

}

int32_t MonitorAssistModule::runOnce() {

  // El entorno de meshtastic aun no está configurado
  if (!service || myNodeInfo.my_node_num == 0) {
    return 5000;
  }

  // Region no configurada aun. Esperar a que esté configurada consultando cada 10 segundos
  // Sin esto, no inicializamos el modulo de asistencia.
  // Dependiendo del modelo del nodo, se deberá configurar la región desde la app movil
  if (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
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
      #ifdef PIN_BUZZER
      if (led_estado) {
          tone(PIN_BUZZER, 800, 400); // Tono agudo
      } else {
          tone(PIN_BUZZER, 600, 400); // Tono grave
      }
      #endif
      
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
      LOG_INFO("[MonitorAssist] HR Band connected via BLE");
      monitorAssistFSM.trigger(EVENT_HRBAND_READY);
  } else {
      hrBandConnected = false;
      LOG_INFO("[MonitorAssist] HR Band disconnected from BLE");
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

  if (nodeDB) {
    auto localNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (localNode) {
      msg.lat = localNode->position.latitude_i;
      msg.lon = localNode->position.longitude_i;
    } else {
      msg.lat = 0;
      msg.lon = 0;
    }
  } else {
    msg.lat = 0;
    msg.lon = 0;
  }

  auto p = packetPool.allocZeroed(0);
  if (p == nullptr) {
    LOG_ERROR("PacketPool vacío. No se puede enviar telemetría.");
    return;
  }

  p->id = generatePacketId();
  p->to = 0xFFFFFFFF; // Broadcast
  p->want_ack = false;
  p->hop_limit = 3;
  p->channel = 0;

  p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  p->decoded.portnum = meshtastic_PortNum_PRIVATE_APP; // Puerto privado fijo

  pb_ostream_t stream = pb_ostream_from_buffer(
      p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes));
  if (!pb_encode(&stream, meshtastic_MonitorAssistTelemetry_fields, &msg)) {
    LOG_ERROR("Error Nanopb: %s", PB_GET_ERROR(&stream));
    packetPool.release(p);
    return;
  }

  p->decoded.payload.size = stream.bytes_written;

  if (service) {
    service->sendToMesh(p);
  } else {
    packetPool.release(p);
  }
}

bool MonitorAssistModule::handleReceivedProtobuf(
    const meshtastic_MeshPacket &mp, meshtastic_MonitorAssistTelemetry *msg) {
  LOG_INFO("Paquete médico recibido de nodo 0x%x. Pulso: %d", mp.from,
           msg->heart_rate);
  return true;
}

#endif // ENABLE_MONITOR_ASSIST