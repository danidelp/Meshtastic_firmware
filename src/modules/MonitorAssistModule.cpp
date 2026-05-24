#include "MonitorAssistModule.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include "mesh/NodeDB.h"
#include "mesh/MeshService.h"
#include "mesh/Router.h"

#ifdef ARCH_NRF52
#include "platform/nrf52/BLEHeartRate.h"
#endif

static MonitorAssistModule *g_monitorAssistInstance = nullptr;

static void monitorAssistHeartRateCallback(uint8_t bpm, uint8_t flags)
{
    if (g_monitorAssistInstance) {
        g_monitorAssistInstance->onHeartRate(bpm, flags);
    }
}

// MODIFICACIÓN 1: Inicializamos las variables de tiempo para que el primer envío no espere 5 minutos
MonitorAssistModule::MonitorAssistModule() 
    : ProtobufModule("MonitorAssist", meshtastic_PortNum_PRIVATE_APP, meshtastic_MonitorAssistTelemetry_fields), 
      concurrency::OSThread("MonitorAssist") 
{
    g_monitorAssistInstance = this;
    lastSendTime = millis() - NORMAL_SEND_INTERVAL_MS; // Permite envío inmediato al arrancar
    lastHeartRateTime = 0;
    hrSampleCount = 0;
    hrSampleIndex = 0;
    lastSentAverage = 0;
    lastWasCritical = false;

#ifdef ARCH_NRF52
    bleSetHeartRateCallback(monitorAssistHeartRateCallback);
#endif
}

int32_t MonitorAssistModule::runOnce() 
{
    if (!service || myNodeInfo.my_node_num == 0) {
        return 5000; // Si la malla no está lista, reintentar en 5 segundos
    }

    // MODIFICACIÓN 2: Quitamos el bloqueo destructivo de 'hrSampleCount == 0'. 
    // Si no hay datos frescos por timeout de desconexión BLE, esperamos, si no, evaluamos.
    if (hrSampleCount > 0 && (millis() - lastHeartRateTime > HEART_RATE_STALE_MS)) {
        LOG_WARN("[MonitorAssist] Datos de pulso obsoletos (Sensor desconectado). Limpiando buffer.");
        hrSampleCount = 0;
        hrSampleIndex = 0;
        return 5000;
    }

    if (hrSampleCount > 0) {
        evaluateAndSend();
    }

    return 2000; // Despertar cada 2 segundos es suficiente y consume menos batería que 1s
}

bool MonitorAssistModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_MonitorAssistTelemetry *msg) 
{
    LOG_INFO("¡Paquete médico privado TFM recibido de nodo 0x%x!", mp.from);
    LOG_INFO("Pulso: %d BPM | Flags Estado: 0x%02X | GPS: %d,%d", msg->heart_rate, msg->assist_flags,
             msg->lat, msg->lon);
    
    return true; 
}

void MonitorAssistModule::sendAssistTelemetry(uint8_t bpm, uint8_t flags) 
{
    // 1. Inicializamos tu estructura Protobuf autogenerada
    meshtastic_MonitorAssistTelemetry msg = meshtastic_MonitorAssistTelemetry_init_default;
    msg.heart_rate = bpm;
    msg.assist_flags = flags;
    
    if (nodeDB) {
        auto localNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
        if (localNode) {
            msg.lat = localNode->position.latitude_i;
            msg.lon = localNode->position.longitude_i;
        } else {
            msg.lat = 0; msg.lon = 0;
        }
    } else {
        msg.lat = 0; msg.lon = 0;
    }

    LOG_INFO("Enviando mensaje por la malla LoRa... (GPS: %d, %d)", msg.lat, msg.lon);

    // 2. FONTANERÍA MANUAL INALMÁBRICA (Forma ultrasegura)
    // Solicitamos un paquete limpio al pool de memoria de Meshtastic
    auto p = packetPool.allocZeroed(0); 
    if (p == nullptr) {
        LOG_ERROR("¡Error critico! No hay memoria en el packetPool para enviar el mensaje.");
        return;
    }

    // Rellenamos las cabeceras obligatorias de la red Mesh
    p->id = generatePacketId();
    p->to = 0xFFFFFFFF;            // Dirección de Broadcast (A toda la malla)
    p->want_ack = false;           // No necesitamos confirmación pesada para pruebas
    p->hop_limit = 3;              // Límite de saltos en la red LoRa
    p->channel = 0;                // Forzamos canal primario de datos

    // Forzamos de forma explícita que viaja decodificado en nuestra APP privada
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_PRIVATE_APP; // <-- AQUÍ SE SELLA EL PUERTO 256 FIJO

    // 3. LA MAGIA: Serializamos el objeto Protobuf dentro del buffer de bytes del paquete de red
    // Usamos el codificador nativo Nanopb de Meshtastic para rellenar p->decoded.payload.bytes
    pb_ostream_t stream = pb_ostream_from_buffer(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes));
    
    if (!pb_encode(&stream, meshtastic_MonitorAssistTelemetry_fields, &msg)) {
        LOG_ERROR("¡Error crítico! Fallo en la codificacion Nanopb del mensaje: %s", PB_GET_ERROR(&stream));
        packetPool.release(p);
        return;
    }
    
    // Guardamos el tamaño exacto resultante del binario comprimido de Protobuf
    p->decoded.payload.size = stream.bytes_written;

    // 4. Empujamos el paquete directamente a la cola física de la antena de radio LoRa
    if (service) {
        LOG_INFO("[MonitorAssist] Inyectando paquete en LoRa... (%d bytes)", p->decoded.payload.size);
        service->sendToMesh(p);
    } else {
        LOG_WARN("¡Servicio Mesh no disponible! Paquete descartado.");
        packetPool.release(p);
    }
}

void MonitorAssistModule::onHeartRate(uint8_t bpm, uint8_t flags)
{
    // Almacenamiento en buffer circular estándar
    hrSamples[hrSampleIndex] = bpm;
    hrSampleIndex = (hrSampleIndex + 1) % MAX_HR_SAMPLES;
    
    if (hrSampleCount < MAX_HR_SAMPLES) {
        hrSampleCount++;
    }
    lastHeartRateTime = millis();
    
    // LOG_DEBUG("Muestra de pulso acumulada: %d BPM (muestras: %d)", bpm, hrSampleCount);
}

void MonitorAssistModule::evaluateAndSend()
{
    if (hrSampleCount == 0) return;
    
    uint32_t sum = 0;
    for (uint8_t i = 0; i < hrSampleCount; i++) {
        sum += hrSamples[i];
    }
    uint8_t average = sum / hrSampleCount;
    
    bool is_critical = (average > 130 || average < 45);
    uint32_t interval = is_critical ? CRITICAL_SEND_INTERVAL_MS : NORMAL_SEND_INTERVAL_MS;
    uint32_t now = millis();
    
    bool time_to_send = (now - lastSendTime > interval);
    bool significant_change = (average != lastSentAverage && abs(average - lastSentAverage) >= 10);
    bool state_changed = (is_critical != lastWasCritical);
    
    if (time_to_send || significant_change || state_changed) {
        uint8_t flags = 0;
        if (is_critical) {
            flags |= 0x01; // Alerta de pulso crítico
        }
        
        LOG_INFO("[MonitorAssist] Promedio: %d BPM (muestras: %d) | Crítico: %s | Enviando LoRa", 
                 average, hrSampleCount, is_critical ? "SÍ" : "NO");
        
        sendAssistTelemetry(average, flags);
        
        lastSendTime = now;
        lastSentAverage = average;
        lastWasCritical = is_critical;
        
        // MODIFICACIÓN 3: ¡Ya NO ponemos hrSampleCount = 0! 
        // Dejamos el buffer lleno para que calcule medias móviles reales en base a las últimas 20 muestras continuas.
    }
}