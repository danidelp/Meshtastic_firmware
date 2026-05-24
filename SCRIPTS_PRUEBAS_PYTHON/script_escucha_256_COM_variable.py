import meshtastic
import meshtastic.serial_interface
from meshtastic import portnums_pb2
from pubsub import pub
import argparse
import sys
import time
import threading
import monitor_assist_pb2 

PORT_ASSIST_APP_NUM = 256 

def on_receive(packet, interface):
    try:
        packet_id = packet.get('id')
        from_id = packet.get('fromId')
        print(f"📡 [Tráfico serie] Detectado paquete ID: {packet_id} desde {from_id}")
        
        if 'decoded' in packet:
            decoded = packet['decoded']
            port_raw = decoded.get('portnum')
            
            # Resolver el puerto dinámicamente (sea int, string o enum)
            port_num = None
            if isinstance(port_raw, int):
                port_num = port_raw
            elif isinstance(port_raw, str):
                if port_raw == "PRIVATE_APP":
                    port_num = 256
                else:
                    port_num = portnums_pb2.PortNum.Value(port_raw)
            else:
                port_num = int(port_raw)

            if port_num == PORT_ASSIST_APP_NUM:
                payload = None
                if 'payload' in decoded:
                    payload = decoded['payload']
                elif 'text' in decoded:
                    payload = decoded['text'].encode('utf-8')

                if payload is None:
                    return

                # 🎓 DECODIFICACIÓN USANDO EL PAQUETE RECONOCIDO
                msg = monitor_assist_pb2.MonitorAssistTelemetry()
                msg.ParseFromString(payload)

                # Convertimos las coordenadas escaladas (entero) a decimales
                lat = msg.lat / 10000000.0
                lon = msg.lon / 10000000.0

                print(f"\n[{time.strftime('%H:%M:%S')}] 📥 ¡BINGO! PAQUETE DECODIFICADO")
                print("-" * 50)
                print(f"👤 Remitente:    {from_id}")
                print(f"💓 Pulso:        {msg.heart_rate} BPM")
                print(f"📍 Ubicación:    {lat:.6f}, {lon:.6f}")
                print(f"🗺️ Google Maps:  https://www.google.com/maps?q={lat},{lon}")
                
                estado = "✅ NORMAL"
                if msg.assist_flags & 0x01: 
                    estado = "🚨 ALERTA - PULSO CRÍTICO"
                print(f"📢 Estado:        {estado}")
                print("-" * 50)
                
    except Exception as e:
        print(f"❌ Error en decodificación: {e}")

def main():
    parser = argparse.ArgumentParser(description="Receptor de Telemetría Médica Protobuf")
    parser.add_argument("-p", "--port", help="Puerto serial (ej: COM9)", required=True)
    args = parser.parse_args()

    stop_event = threading.Event()
    try:
        print(f"🔌 Conectando al nodo T1000-E en {args.port}...")
        interface = meshtastic.serial_interface.SerialInterface(args.port)
        pub.subscribe(on_receive, "meshtastic.receive")
        print("📡 Escuchando puerto 256 (PRIVATE_APP)... Ctrl+C para salir.")
        
        while not stop_event.is_set():
            stop_event.wait(timeout=1.0)
    except KeyboardInterrupt:
        print("\n🛑 Saliendo del receptor...")
        sys.exit(0)

if __name__ == "__main__":
    main()