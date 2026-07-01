#pragma once
#ifndef _QMA_6100P_SENSOR_H_
#define _QMA_6100P_SENSOR_H_

#include "MotionSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && defined(HAS_QMA6100P)

#include <QMA6100P.h>
#ifdef ENABLE_MONITOR_ASSIST
#include "Observer.h"
#endif

// Set the default accelerometer scale - gpm2, gpm4, gpm8, gpm16
#ifndef QMA_6100P_MPU_ACCEL_SCALE
#define QMA_6100P_MPU_ACCEL_SCALE SFE_QMA6100P_RANGE32G
#endif

// The I2C address of the Accelerometer (if found) from main.cpp
extern ScanI2C::DeviceAddress accelerometer_found;

// Singleton wrapper for the Sparkfun QMA_6100P_I2C class
class QMA6100PSingleton : public QMA6100P
#ifdef ENABLE_MONITOR_ASSIST
    , public Observable<const void *>
#endif
{
  private:
    static QMA6100PSingleton *pinstance;

  protected:
    QMA6100PSingleton();
    ~QMA6100PSingleton();

  public:
    // Create a singleton instance (not thread safe)
    static QMA6100PSingleton *GetInstance();

    // Singletons should not be cloneable.
    QMA6100PSingleton(QMA6100PSingleton &other) = delete;

    // Singletons should not be assignable.
    void operator=(const QMA6100PSingleton &) = delete;

    // Initialise the motion sensor singleton for normal operation
    bool init(ScanI2C::FoundDevice device);

    // Enable Wake on Motion interrupts (sensor must be initialised first)
    bool setWakeOnMotion();

#ifdef ENABLE_MONITOR_ASSIST
    // Enable Fall Detection interrupts for our Monitor Assist functionality
    bool setFallDetection();
#endif
};

class QMA6100PSensor : public MotionSensor
{
  private:
    QMA6100PSingleton *sensor = nullptr;
    // Detect a fall event (called from runOnce when an interrupt arrives)
    void detectFall();

  public:
#ifdef ENABLE_MONITOR_ASSIST
    // Resetea el estado de la FSM de caída desde fuera (ej: al pulsar el botón físico)
    static void resetFallState();
#endif

    explicit QMA6100PSensor(ScanI2C::FoundDevice foundDevice);

    // Initialise the motion sensor
    virtual bool init() override;

    // Called each time our sensor gets a chance to run
    virtual int32_t runOnce() override;
};

#endif

#endif