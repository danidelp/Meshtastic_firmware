#include "configuration.h"

#ifdef ENABLE_MONITOR_ASSIST

#include "MonitorAssistManager.h"
#include "MonitorAssistProtocol.h"
#include "MeshService.h"
#include "NodeDB.h"
#include <meshtastic/portnums.pb.h>
#include <meshtastic/mesh.pb.h>

void broadcastAssistData(uint8_t bpm, uint8_t flags) {
    
    // Usamos 0 para no esperar. 
    // Si sigue fallando, prueba con packetPool.allocUniqueZeroed(0)
    auto p = packetPool.allocZeroed(0); 

    if (p == nullptr) return; 

    // GENERA UN ID ÚNICO PARA CADA PAQUETE
    // Esto quita el aviso "Ignore 0 id broadcast" en el receptor
    p->id = random(1, 0xFFFFFFFF);

    // 1. Forzamos valores seguros para evitar errores de decodificación local
    p->to = 0xFFFFFFFF; 
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    
    p->want_ack = false; // Desactiva esto para pruebas, evita retransmisiones ruidosas
    p->hop_limit = 3;    // Valor estándar de saltos
    
    // 2. IMPORTANTE: Definir el puerto correctamente
    p->decoded.portnum = meshtastic_PortNum_PRIVATE_APP;
    
    // 3. Preparar el payload
    MonitorAssistTelemetryPacket payload;
    payload.bpm = bpm;
    payload.assist_flags = flags;
    
    // Inicializamos GPS a 0 si no hay datos para evitar basura
    payload.latitude = 0;
    payload.longitude = 0;

    auto localNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (localNode) {
        payload.latitude = localNode->position.latitude_i;
        payload.longitude = localNode->position.longitude_i;
    }

    // 4. Limpiamos el payload del paquete antes de copiar
    memset(p->decoded.payload.bytes, 0, sizeof(p->decoded.payload.bytes));
    
    // Copiamos los datos
    p->decoded.payload.size = sizeof(MonitorAssistTelemetryPacket);
    memcpy(p->decoded.payload.bytes, &payload, sizeof(MonitorAssistTelemetryPacket));

    // 5. El truco final: Marcamos el paquete como NO encriptado para que 
    // el log local no intente desencriptarlo (eso causa el error "too large")
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;

    if (service) {
        LOG_INFO("Enviando paquete Asistencia... (%d bytes)", p->decoded.payload.size);
        service->sendToMesh(p);
    }
}

#endif // ENABLE_ASSIST_MONITOR