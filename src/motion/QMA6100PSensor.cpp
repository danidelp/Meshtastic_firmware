#include "QMA6100PSensor.h"
#include <math.h>

#ifdef ENABLE_MONITOR_ASSIST

#endif

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && defined(HAS_QMA6100P)

// Flag when an interrupt has been detected
volatile static bool QMA6100P_IRQ = false;

#ifdef ENABLE_MONITOR_ASSIST
// Variables de estado para enlazar los eventos de hardware
static bool fall_detected = false;
static bool userImmobile = false;
static uint32_t last_impact_time = 0;
static uint32_t first_impact_time = 0;
#endif

// Interrupt service routine
void QMA6100PSetInterrupt()
{
    QMA6100P_IRQ = true;
}

// === CONFIGURACIÓN DE SENSIBILIDAD PARA EL USUARIO FINAL ===
// En modo ±32G (el usado por defecto), cada unidad Hexadecimal equivale a ~0.0625 G.
//
// Tabla de valores típicos (chuleta):
// 0x08 = 0.5 G (Hipersensible, detecta casi cualquier roce)
// 0x10 = 1.0 G (Sensible, detecta a una persona arrastrándose o moviendo los brazos)
// 0x18 = 1.5 G (Medio)
// 0x20 = 2.0 G (Impacto medio-fuerte)
// 0x28 = 2.5 G (Caída notable)
// 0x30 = 3.0 G (Caída muy dura)
// 0x40 = 4.0 G (Golpe extremo)
//
// Ajusta estos umbrales a tu gusto:
#define UMBRAL_CAIDA_GRAVE           0x0C // 0.75G (VALOR DE PRUEBAS) -> Fuerza necesaria para que el sistema considere que te has caído
#define UMBRAL_MOVIMIENTO_POST_CAIDA 0x03 // 0x10 = 1.0G, 0x03 = 0.5G -> Fuerza necesaria para considerar que te estás moviendo por el suelo

// Ajustes de tiempos (en milisegundos):
#define TIMEOUT_INMOVILIDAD_MS  10000 // Si estás quieto 10s tras la caída, salta la emergencia
#define TIMEOUT_RECUPERACION_MS 40000 // Si te pasas 40s seguidos moviéndote, asumimos que estás bien

#ifdef ENABLE_MONITOR_ASSIST
void QMA6100PSensor::resetFallState() {
    LOG_INFO("QMA6100P: Resetting hardware state (Abort/Init).");
    fall_detected = false;
    last_impact_time = 0;
    first_impact_time = 0;
    
    QMA6100PSingleton *sensor = QMA6100PSingleton::GetInstance();
    if (sensor) {
        sensor->writeRegisterByte(0x2E, UMBRAL_CAIDA_GRAVE);
        sensor->writeRegisterByte(0x2C, 0x03);
    }
}
#endif

// ===========================================================

QMA6100PSensor::QMA6100PSensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}

bool QMA6100PSensor::init()
{
    // Initialise the sensor
    sensor = QMA6100PSingleton::GetInstance();
    if (!sensor->init(device))
        return false;

#ifdef ENABLE_MONITOR_ASSIST
    // Enable fall detection
    return sensor->setFallDetection();
#else
    // Enable simple Wake on Motion
    return sensor->setWakeOnMotion();
#endif // ENABLE_MONITOR_ASSIST
}

#ifdef QMA_6100P_INT_PIN

int32_t QMA6100PSensor::runOnce()
{
#ifdef ENABLE_MONITOR_ASSIST
    // SPY DIAGNOSTICO - NO BORRAR HASTA QUE FUNCIONE
    // static uint32_t last_spy_time = 0;
    // if (millis() - last_spy_time > 2000) {
    //     uint8_t reg2F = 0, reg2E = 0, reg30 = 0, reg20 = 0, reg09 = 0, reg2C = 0, reg2D = 0, reg18 = 0, reg1A = 0;
    //     sensor->readRegisterRegion(0x2F, &reg2F, 1);
    //     sensor->readRegisterRegion(0x2E, &reg2E, 1);
    //     sensor->readRegisterRegion(0x30, &reg30, 1);
    //     sensor->readRegisterRegion(0x20, &reg20, 1);
    //     sensor->readRegisterRegion(0x09, &reg09, 1);
    //     sensor->readRegisterRegion(0x2C, &reg2C, 1);
    //     sensor->readRegisterRegion(0x2D, &reg2D, 1);
    //     sensor->readRegisterRegion(0x18, &reg18, 1);
    //     sensor->readRegisterRegion(0x1A, &reg1A, 1);

    //     LOG_INFO("[Accelerometer] [QMA SPY] PIN: %d | 0x09:%02X | 2C:%02X | 2D:%02X | 18:%02X | 1A:%02X | 30:%02X | 2F:%02X",
    //              digitalRead(QMA_6100P_INT_PIN), reg09, reg2C, reg2D, reg18, reg1A, reg30, reg2F);
                 
    //     last_spy_time = millis();
    // }
#endif

    // Evaluamos los timeouts de software
    if (fall_detected) {
        if (millis() - last_impact_time > TIMEOUT_INMOVILIDAD_MS) {

            if (!userImmobile){
                
                // Tiempo sin NINGÚN movimiento -> INMOVILIDAD REAL
                LOG_WARN("QMA6100P: Immobility timeout reached! ALERT FSM...");

                userImmobile = true;

                if (sensor) {
                    sensor->notifyObservers((void*)true); // Notifica caída confirmada
                    // sensor->writeRegisterByte(0x2E, UMBRAL_CAIDA_GRAVE); // Restaurar umbral principal
                    // sensor->writeRegisterByte(0x2C, 0x03);
                }
            }
            
        } else if (millis() - first_impact_time > TIMEOUT_RECUPERACION_MS) {
            // Han pasado X segundos desde la caída. ¿Debemos desarmar?
            // BUGFIX: Si el usuario lleva ya un buen rato quieto (ej. 5 segundos), NO desarmamos,
            // porque podría estar perdiendo el conocimiento justo ahora y el temporizador de 
            // inmovilidad de 15s debe tener prioridad para terminar de contar.
            // Solo desarmamos si el usuario se ha movido "recientemente" (hace menos de 5s).
            if (millis() - last_impact_time < 5000) {
                LOG_DEBUG("QMA6100P: User recovered and active. Disarming and restoring original sensitivity...");
                fall_detected = false;
                userImmobile = false;
                
                if (sensor) {
                    sensor->writeRegisterByte(0x2E, UMBRAL_CAIDA_GRAVE); // Restaurar umbral principal
                    sensor->writeRegisterByte(0x2C, 0x03);
                }
            }
        }
    }

    // Wake on motion using hardware interrupts - this is the most efficient way to check for motion
    if (QMA6100P_IRQ) {
        QMA6100P_IRQ = false;

#ifdef ENABLE_MONITOR_ASSIST
        detectFall();
#else
        // Original functionality for IMU sensor
        wakeScreen();
#endif
    }
    
    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

#else

int32_t QMA6100PSensor::runOnce()
{
    // Wake on motion using polling  - this is not as efficient as using hardware interrupt pin (see above)
    if (!sensor)
        return MOTION_SENSOR_CHECK_INTERVAL_MS;

    uint8_t tempVal;
    if (!sensor->readRegisterRegion(SFE_QMA6100P_INT_ST0, &tempVal, 1)) {
        LOG_DEBUG("QMA6100PS isWakeOnMotion failed to read interrupts");
        return MOTION_SENSOR_CHECK_INTERVAL_MS;
    }

    if ((tempVal & 7) != 0) {
        // Wake up!
        wakeScreen();       
    }
    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

#endif

#ifdef ENABLE_MONITOR_ASSIST

// Member fall detection helper used by the interrupt path.
/**
 * @brief Algoritmo de Detección de Caídas para Personas Mayores (Man-Down)
 * 
 * Este mecanismo combina el motor de interrupciones interno del QMA6100P 
 * con una máquina de estados por software para evitar falsos positivos.
 * 
 * === 1. EL HARDWARE (QMA6100P) ===
 * El chip no evalúa la fuerza G absoluta, sino la "Pendiente" o "Delta" 
 * entre muestras consecutivas: |Accel(t) - Accel(t-1)|. 
 * Gracias a esto, el detector es INMUNE a la gravedad estática y a los 
 * posibles defectos de calibración de fábrica (offsets).
 * 
 * - ANY_MOTION (Impacto): El chip dispara esta interrupción (Bits 1-3) si 
 *   la aceleración cambia bruscamente superando el umbral ANY_MOT_TH 
 *   (ej. 1.75G). Esto ocurre en caídas, tropiezos o al levantarse de golpe.
 * 
 * - NO_MOTION (Quietu): El chip dispara esta interrupción (Bit 0) si 
 *   la aceleración se mantiene por DEBAJO del umbral NO_MOT_TH (ej. 1.0G) 
 *   durante un tiempo prolongado (ej. 15 segundos). Permite respirar o 
 *   moverse suavemente sin resetear el contador.
 * 
 * === 2. EL SOFTWARE (Máquina de Estados) ===
 * Como sentarse a ver la tele también dispara la interrupción de Quietud, 
 * el software actúa como filtro:
 * 
 * 1. IMPACTO: Si el hardware detecta un golpe, ponemos `fall_detected = true`
 *    y empezamos a vigilar (Armado).
 * 2. QUIETUD: Si el hardware detecta inmovilidad prolongada, SOLO lanzamos la
 *    emergencia si previamente hubo un impacto (`fall_detected == true`). 
 *    Si no hubo impacto previo, asumimos que el usuario está durmiendo.
 * 3. RECUPERACIÓN: Si han pasado 30 segundos desde el impacto y el usuario
 *    ha seguido moviéndose (la Quietud nunca saltó), asumimos que fue un
 *    tropiezo sin consecuencias y desarmamos el sistema (`fall_detected = false`).
 */
void QMA6100PSensor::detectFall()
{
    uint8_t int_status = 0;

    if (!sensor) {
        LOG_WARN("QMA6100P: on detectFall sensor instance is null");
        return;
    }

    /*
        El registro 0x09 te devuelve un solo byte (8 bits). Cada bit es como una pequeña bombilla que la IMU enciende (1) o apaga (0) para indicar qué ha pasado. El mapa de ese byte es este:

        Bit 0: Se enciende si ha habido Inmovilidad (NO_MOT). Valor Hex: 0x01.
        Bit 1: Se enciende si el primer impacto fue en el eje X. Valor Hex: 0x02.
        Bit 2: Se enciende si el primer impacto fue en el eje Y. Valor Hex: 0x04.
        Bit 3: Se enciende si el primer impacto fue en el eje Z. Valor Hex: 0x08.
        Bits 4 a 7: Otras cosas (signo del impacto, contador de pasos, etc).
    */
     if (sensor->readRegisterRegion(SFE_QMA6100P_INT_ST0, &int_status, 1)) {
        LOG_INFO("QMA6100P: on detectFall interrupt status is 0x%02X", int_status);
        
        // Leemos caida del registro de la IMU
        // Leemos impacto del registro de la IMU (Bits 0, 1, 2)
        // 0x07 = 0000 0111 -> X, Y, Z. Ignoramos el bit de signo (Bit 3).
        if (int_status & 0x07) {
            if (!fall_detected) {
                // PRIMER IMPACTO GRAVE (Caída)
                LOG_WARN("QMA6100P: Detected fall on sensor...");
                fall_detected = true;
                last_impact_time = millis();
                first_impact_time = millis();
                userImmobile = false;
                
                // Subimos la sensibilidad para el suelo.
                sensor->writeRegisterByte(0x2E, UMBRAL_MOVIMIENTO_POST_CAIDA);
                // Filtro de duración del movimiento a 4 muestras seguidas (0x03).
                sensor->writeRegisterByte(0x2C, 0x03);
            } else {
                // MOVIMIENTO DESPUÉS DE LA CAÍDA
                // La persona se está moviendo (intentando levantarse). Reseteamos el contador de 15s.
                LOG_DEBUG("QMA6100P: User is moving on the floor, resetting 15s immobility timer...");
                last_impact_time = millis();

                // Si la FSM estaba en emergencia (inmóvil), la desarmamos
                if (userImmobile) {
                    LOG_INFO("QMA6100P: Motion detected! Canceling FSM emergency...");
                    userImmobile = false;
                    sensor->notifyObservers((void*)false); // Notifica cancelación de emergencia
                }
            }
        }
    }
}
#endif


// ----------------------------------------------------------------------
// QMA6100PSingleton
// ----------------------------------------------------------------------

// Get a singleton wrapper for an Sparkfun QMA_6100P_I2C
QMA6100PSingleton *QMA6100PSingleton::GetInstance()
{
    if (pinstance == nullptr) {
        pinstance = new QMA6100PSingleton();
    }
    return pinstance;
}

QMA6100PSingleton::QMA6100PSingleton() {}

QMA6100PSingleton::~QMA6100PSingleton() {}

QMA6100PSingleton *QMA6100PSingleton::pinstance{nullptr};

// Initialise the QMA6100P Sensor
bool QMA6100PSingleton::init(ScanI2C::FoundDevice device)
{
    // startup
#ifdef Wire1
    bool status = begin(device.address.address, device.address.port == ScanI2C::I2CPort::WIRE1 ? &Wire1 : &Wire);
#else
    // check chip id
    bool status = begin(device.address.address, &Wire);
#endif
    if (status != true) {
        LOG_WARN("QMA6100P init begin failed");
        return false;
    }
    delay(20);
    // SW reset to make sure the device starts in a known state
    if (softwareReset() != true) {
        LOG_WARN("QMA6100P init reset failed");
        return false;
    }
    delay(20);
    // Set range
    if (!setRange(QMA_6100P_MPU_ACCEL_SCALE)) {
        LOG_WARN("QMA6100P init range failed");
        return false;
    }
    // set active mode
    if (!enableAccel()) {
        LOG_WARN("ERROR QMA6100P active mode set failed");
    }
    // set calibrateoffsets
    if (!calibrateOffsets()) {
        LOG_WARN("ERROR QMA6100P calibration failed");
    }
#ifdef QMA_6100P_INT_PIN

    // Active low & Open Drain
    uint8_t tempVal;
    if (!readRegisterRegion(SFE_QMA6100P_INTPINT_CONF, &tempVal, 1)) {
        LOG_WARN("QMA6100P init failed to read interrupt pin config");
        return false;
    }

    tempVal |= 0b00000010; // Active low & Open Drain

    if (!writeRegisterByte(SFE_QMA6100P_INTPINT_CONF, tempVal)) {
        LOG_WARN("QMA6100P init failed to write interrupt pin config");
        return false;
    }

    // Latch until cleared, all reads clear the latch
    if (!readRegisterRegion(SFE_QMA6100P_INT_CFG, &tempVal, 1)) {
        LOG_WARN("QMA6100P init failed to read interrupt config");
        return false;
    }

    tempVal |= 0b10000001; // Latch until cleared, INT_RD_CLR1

    if (!writeRegisterByte(SFE_QMA6100P_INT_CFG, tempVal)) {
        LOG_WARN("QMA6100P init failed to write interrupt config");
        return false;
    }
    // Set up an interrupt pin with an internal pullup for active low
    pinMode(QMA_6100P_INT_PIN, INPUT_PULLUP);

    // Set up an interrupt service routine
    attachInterrupt(QMA_6100P_INT_PIN, QMA6100PSetInterrupt, FALLING);

#endif
    return true;
}

bool QMA6100PSingleton::setWakeOnMotion()
{
    // Enable 'Any Motion' interrupt
    if (!writeRegisterByte(SFE_QMA6100P_INT_EN2, 0b00000111)) {
        LOG_WARN("QMA6100P :setWakeOnMotion failed to write interrupt enable");
        return false;
    }

    // Set 'Significant Motion' interrupt map to INT1
    uint8_t tempVal;

    if (!readRegisterRegion(SFE_QMA6100P_INT_MAP1, &tempVal, 1)) {
        LOG_WARN("QMA6100P setWakeOnMotion failed to read interrupt map");
        return false;
    }

    sfe_qma6100p_int_map1_bitfield_t int_map1;
    int_map1.all = tempVal;
    int_map1.bits.int1_any_mot = 1; // any motion interrupt to INT1
    tempVal = int_map1.all;

    if (!writeRegisterByte(SFE_QMA6100P_INT_MAP1, tempVal)) {
        LOG_WARN("QMA6100P setWakeOnMotion failed to write interrupt map");
        return false;
    }

    // Clear any current interrupts
    QMA6100P_IRQ = false;
    return true;
}

#ifdef ENABLE_MONITOR_ASSIST

/**
     * @brief Configuración de los Registros del QMA6100P para Detección de Caídas
     * 
     * Esta función configura el motor de hardware interno del acelerómetro. 
     * El QMA6100P trabaja calculando la "Pendiente" o "Delta" de aceleración 
     * entre muestras consecutivas, lo que lo hace inmune a la gravedad estática.
     * Estamos trabajando en una escala de ±32g (1G = 256 LSB).
     * 
     * === REGISTROS DE UMBRALES Y TIEMPOS ===
     * 
     * 1. 0x2E (MOTION_CFG2) - Umbral de Impacto (ANY_MOT_TH):
     *    - Define el delta de aceleración necesario para detectar un golpe.
     *    - En modo ±32g, cada unidad Hexadecimal equivale a 32mG.
     *    - Ej: 0x0E (14) -> 14 * 32 = 448 mG (Ideal para no saltar al sentarse).
     * 
     * 2. 0x2D (MOTION_CFG1) - Umbral de Quietud (NO_MOT_TH):
     *    - Define el movimiento máximo permitido mientras se considera "Quieto".
     *    - En modo ±32g, cada unidad Hexadecimal equivale a 16mG.
     *    - Ej: 0x10 (16) -> 16 * 16 = 256 mG (Permite moverse suavemente en el suelo).
     * 
     * 3. 0x2C (MOTION_CFG0) - Duraciones (NO_MOT_DUR / ANY_MOT_DUR):
     *    - Bits [7:2]: Segundos de inmovilidad necesarios (N+1). 
     *      Ej: 0x38 (Bits a 14) -> 14 + 1 = 15 Segundos.
     *    - Bits [1:0]: Muestras de impacto necesarias (00 = instantáneo).
     * 
     * 4. 0x2F (MOTION_CFG3):
     *    - Escribimos 0x40 para activar el bit ANY_MOT_IN_SEL. Esto hace que
     *      los umbrales se escalen dinámicamente según si el chip está en ±2g o ±32g.
     * 
     * === REGISTROS DE CONTROL DE INTERRUPCIONES ===
     * 
     * 5. 0x18 (INT_EN2) - Habilitación de Motores:
     *    - Bits [7:5]: Activan la quietud (NO_MOT) en los ejes Z, Y, X.
     *    - Bits [2:0]: Activan el impacto (ANY_MOT) en los ejes Z, Y, X.
     *    - Valor 0xE7 (11100111) -> Activa todos los ejes en ambas funciones.
     * 
     * 6. 0x1A (INT_MAP1) - Ruteo al Pin Físico:
     *    - Bit 7: Manda la alerta de Quietud al Pin INT1.
     *    - Bit 0: Manda la alerta de Impacto al Pin INT1.
     *    - Valor 0x81 (10000001) -> Despierta a nuestro ESP32 ante cualquiera de las dos.
     * 
     * === REGISTRO DE LECTURA (En detectFall) ===
     * 
     * 7. 0x09 (INT_ST0) - Estado de la Interrupción (El chivato):
     *    - Bit 0 (0x01): Vale 1 si el chip lleva quieto el tiempo configurado.
     *    - Bits 1 a 3 (0x0E): Valen 1 si el chip ha sentido un impacto en X, Y o Z.
     *    - NOTA: Este registro se limpia (Latch reset) automáticamente al leerlo.
     */
bool QMA6100PSingleton::setFallDetection()
{
LOG_INFO("QMA6100P [MonitorAssist]: Configuring fall detection...");

    // 0. Desbloquear motores y activar Filtros (Registro 0x30)
    // 0x3B = 0011 1011 -> Motores ENCENDIDOS (incluyendo ANY_MOT en Bit 3). Filtros ACTIVADOS (Bit 2 a 0).
    if (!writeRegisterByte(SFE_QMA6100P_REG_30, 0x3B)) {
        LOG_WARN("QMA6100P: Error configuring engine reset");
        return false;
    }

    // 1. Activar interrupciones de Impacto (Any-Motion) (Registro 0x18)
    // 0x07 = 0000 0111 -> Desactivamos el inútil NO_MOT. Solo ANY_MOT.
    if (!writeRegisterByte(SFE_QMA6100P_INT_EN2, 0x07)) {
        LOG_WARN("QMA6100P: Error configuring Fall interrupt enable");
        return false;
    }

    // 2. Mapeo de Interruptores (Registro 0x19 / SFE_QMA6100P_INT_MAP1)
    // Bit 0: ANY_MOT al Pin INT1 (0x01)
    if (!writeRegisterByte(SFE_QMA6100P_INT_MAP1, 0x01)) {
        LOG_WARN("QMA6100P: Error configuring INT1 map");
        return false;
    }

    // 3. Any-Motion Select (Registro 0x2F)
    // 0x00 -> Usa el modo Differential/Slope (Ignora la gravedad y se centra en los picos).
    if (!writeRegisterByte(0x2F, 0x00)) {
        LOG_WARN("QMA6100P: Error configuring Any-Motion Select");
        return false;
    }

    // 4. Umbral de Impacto G (Registro 0x2E)
    // En modo Slope, el multiplicador es de 16 LSB por unidad.
    // Usamos la macro configurable definida al principio del fichero.
    if (!writeRegisterByte(0x2E, UMBRAL_CAIDA_GRAVE)) {
        LOG_WARN("QMA6100P: Error configuring High-G threshold");
        return false;
    }

    // (Limpieza: Apagamos NO_MOT completamente en el registro 0x30 y 0x2D)
    writeRegisterByte(0x30, 0x1B); // Desactivar filtros de quietud rotos
    writeRegisterByte(0x2D, 0x00); // 0 umbral
    
    // 4.1 ANY_MOT_DUR (Registro 0x2C)
    // El filtro anti-falsos-positivos. Los bits [1:0] son ANY_MOT_DUR.
    // Si vale 0, CUALQUIER pico de 1 milisegundo (golpear la mesa con la uña) dispara la alarma.
    // Ponemos 0x03 (4 muestras consecutivas). Como lee a ~100Hz, el golpe debe durar al menos ~40ms 
    // para ser considerado válido. Las caídas humanas son largas, los golpes secos son cortos.
    writeRegisterByte(0x2C, 0x03);

    // 5. Corregir polaridad del pin INT1 (Registro 0x20)
    // Forzamos Push-Pull y Active Low (Bit 1=0, Bit 0=0) = 0x00.
    // Push-Pull asegura que el chip tiene fuerza para bajar el pin a 0 físicamente.
    uint8_t int_conf;
    if (readRegisterRegion(0x20, &int_conf, 1)) {
        int_conf = (int_conf & 0xFC) | 0x00; // Bit 1=0 (Push-Pull), Bit 0=0 (Active Low)
        writeRegisterByte(0x20, int_conf);
    }

    // 9. Limpieza de seguridad (Flush)
    // Leemos el registro de estado una vez para asegurar que el pin físico INT1 
    // se desatasca y vuelve a su estado normal tras el arranque.
    uint8_t dummy;
    readRegisterRegion(SFE_QMA6100P_INT_ST0, &dummy, 1);
    QMA6100P_IRQ = false; // Limpiamos la variable de software por si acaso

    return true;
}
#endif // ENABLE_MONITOR_ASSIST

#endif
