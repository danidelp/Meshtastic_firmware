
## INVOCACION: python script_escucha_768_COM_variable.py -p COMXX

import meshtastic
import meshtastic.serial_interface
from pubsub import pub
import struct
import argparse
import sys
import time
import threading

# CONFIGURACIÓN
PORT_ASSIST_APP = 256 # Private applications should use portnums >= 256

def on_receive(packet, interface):
    """Esta función solo se ejecuta cuando pubsub detecta un paquete entrante"""
    try:
        if packet['decoded']['portnum'] == PORT_ASSIST_APP:
            payload = packet['decoded']['payload']
            
            # DECODIFICACIÓN
            heart_rate, lat_raw, lon_raw, flags = struct.unpack('<BiiB', payload)

            lat = lat_raw / 10000000.0
            lon = lon_raw / 10000000.0

            print(f"\n[{time.strftime('%H:%M:%S')}] 📥 NUEVO PAQUETE DE ASISTENCIA")
            print("-" * 40)
            print(f"👤 De Nodo:      {packet['fromId']}")
            print(f"💓 Pulso:        {heart_rate} BPM")
            print(f"📍 Posición:     {lat:.6f}, {lon:.6f}")
            print(f"🗺️ Mapa:         https://www.google.com/maps?q={lat},{lon}")
            
            estado = "✅ Normal"
            if flags & 0x01: estado = "🚨 ALERTA MÉDICA"
            if flags & 0x02: estado = "⚠️ CAÍDA DETECTADA"
            
            print(f"📢 Estado:        {estado}")
            print(f"📶 Señal (SNR):   {packet['rxSnr']} dB")
            print("-" * 40)

    except KeyError:
        pass
    except Exception as e:
        print(f"❌ Error al decodificar: {e}")

def main():
    parser = argparse.ArgumentParser(description="Receptor de Telemetría Médica (Event-driven)")
    parser.add_argument("-p", "--port", help="Puerto serial (ej: COM5)", required=True)
    args = parser.parse_args()

    stop_event = threading.Event()

    try:
        print(f"🔌 Conectando al nodo en {args.port}...")
        interface = meshtastic.serial_interface.SerialInterface(args.port)

        pub.subscribe(on_receive, "meshtastic.receive")

        print("📡 Escuchando...")
        print("⌨️  Presiona Ctrl+C para salir.")

        # En lugar de wait() infinito, esperamos en trozos de 1 segundo
        # Esto permite que Python detecte el KeyboardInterrupt
        while not stop_event.is_set():
            stop_event.wait(timeout=1.0)

    except KeyboardInterrupt:
        print("\n\n🛑 Interrupción detectada. Cerrando conexión...")
        stop_event.set()
    except Exception as e:
        print(f"❌ Error: {e}")
    finally:
        if 'interface' in locals():
            interface.close()
        print("👋 Script finalizado.")
        sys.exit(0)

if __name__ == "__main__":
    main()