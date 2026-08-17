/**
 * @file INA219Monitor.h
 * @brief Declares management of every INA219 current/power sensor wired to
 *        the current Node.
 *
 * A Node can have several Loads, and individual Loads can each have their
 * own INA219 current/power sensor sharing one I2C bus. INA219Monitor's one
 * responsibility is to manage the INA219 devices physically connected to
 * THIS Node and to obtain voltage, current and power measurements for the
 * Loads associated with those devices.
 *
 * Global Load identity remains Node MAC address + relay pin (see Load.h).
 * INA219Monitor never stores a Node MAC address: the Node/Load layer that
 * owns this monitor already knows which Node it is running on. Locally, one
 * INA219 is identified only by its I2C address, and is associated with the
 * relay pin of the Load it measures. That relay pin is enough for the
 * caller to find the matching Load through Node::getLoadByRelayPin().
 *
 * INA219Monitor does not perform ESP-NOW communication, does not know
 * remote Node topology, does not run Best-First Search, does not filter
 * Loads, does not calculate system Available Power, does not perform MQTT,
 * and does not control relays. It measures. Nothing else.
 *
 * Development vs. production sensor source (Chapter 4, "Development mode
 * may provide deterministic test measurements at the hardware-source
 * boundary"): a per-sensor runtime override (setDevelopmentOverride() /
 * clearDevelopmentOverride()), never a compile-time switch, changes only
 * where readMeasurements() gets its raw voltage/current/power from - every
 * other method on this class, and everything downstream of it (EMA
 * filtering, calibration, Load::setMeasurements(), battery State of
 * Charge, the power budget, Best-First Search) is exactly the same
 * production code path whether or not an override is active. No sensor
 * has an override by default: a freshly registered sensor always attempts
 * real I2C communication first, and only ever reports a substituted
 * reading after something explicitly armed one - see DevelopmentSession.h in lib/DevelopmentManager
 * for the session/policy layer that decides when that happens.
 * getLastMeasurementSource() exposes which one actually produced a
 * sensor's most recent successful reading (HARDWARE/SIMULATED), or NONE
 * before any successful reading has ever occurred. This distinction is
 * orthogonal to the host-vs-ESP32 build split below: an override only
 * ever substitutes a reading while running as real firmware on the ESP32
 * target (ESP_PLATFORM defined) - a plain host build still always returns
 * false from every hardware-touching method regardless of any override,
 * since there is no firmware runtime there at all to substitute one for.
 *
 * This header has no ESP-IDF dependency so it can be included, and its
 * sensor-registration logic and EMA math exercised, from a plain host
 * build (see test/INA219Monitor/). The real I2C register communication,
 * the development-value substitution, and calibration NVS persistence are
 * compiled only when building for the ESP32 target (see
 * INA219Monitor.cpp).
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#ifndef KILOWATTS_INA219_MONITOR_H
#define KILOWATTS_INA219_MONITOR_H

#include "Load.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


/**
 * Which real source actually produced a sensor's most recent successful
 * reading. NONE means no successful reading has occurred yet (never read,
 * or every attempt so far has failed) - it is never used to mean "zero
 * measured value," which is a legitimate HARDWARE/SIMULATED reading in
 * its own right.
 */
enum class MeasurementSource : std::uint8_t {
    NONE = 0U,
    HARDWARE = 1U,
    SIMULATED = 2U
};

const char* toText(MeasurementSource source);


/**
 * Manages every INA219 device on one Node's single shared I2C bus, and
 * reads the voltage, current and power of the Load each device is wired
 * to measure.
 *
 * Usage:
 *
 *     INA219Monitor powerMonitor;
 *     powerMonitor.initializeBus(busConfiguration);
 *     powerMonitor.addSensor(sensorConfigurationA);
 *     powerMonitor.addSensor(sensorConfigurationB);
 *     ...
 *     LoadMeasurements measurements{};
 *     if (powerMonitor.readMeasurementsForRelayPin(16U, measurements)) {
 *         Load *load = node.getLoadByRelayPin(16U);
 *         if (load != nullptr) load->setMeasurements(measurements);
 *     }
 *
 * One INA219Monitor manages every sensor on the Node, rather than one
 * monitor object per sensor.
 */
class INA219Monitor {

public:

    /**
     * Configuration for the ONE I2C bus this Node's INA219 devices share.
     *
     * initializeBus() is called once per Node, before any addSensor()
     * call, regardless of how many INA219 devices are on that bus.
     */
    struct I2CBusConfiguration {
        std::uint8_t serialDataPin;
        std::uint8_t serialClockPin;
        std::uint32_t clockSpeedHz;
        std::uint8_t i2cPortNumber;
    };


    /**
     * Identifies one physically connected INA219 device and the Load it
     * measures, plus the two physical values its calibration depends on.
     *
     * i2cAddress is the local identity of the device on this Node's I2C
     * bus (the INA219's real, datasheet-defined address space is 0x40 to
     * 0x4F). relayPin is the relay pin of the Load this device measures,
     * inside this same Node — never a Node MAC address, since ownership is
     * already known by the surrounding Node/Load architecture.
     *
     * shuntResistanceOhms and maximumExpectedCurrentAmps are physical
     * properties of the sensor board actually wired to this relay pin.
     * They are required, not defaulted: correct calibration depends on
     * them, and this module never assumes an undocumented board.
     *
     * emaAlpha is the smoothing factor a (Equations 4.2-4.4), applied by
     * readFilteredMeasurements() below; it must satisfy 0 < alpha <= 1.
     */
    struct INA219SensorConfiguration {
        std::uint8_t i2cAddress;
        std::uint8_t relayPin;
        float shuntResistanceOhms;
        float maximumExpectedCurrentAmps;
        float emaAlpha;
    };


    /**
     * Calibration offsets/scaling factors for one INA219 device (System
     * Requirement 3: "calibration persistence via Non-Volatile Storage to
     * securely retain sensor calibration offsets and scaling factors").
     * Applied to the raw voltage/current reading, before EMA filtering,
     * inside readMeasurements(): correctedVoltage = raw.voltage +
     * voltageOffsetVolts; correctedCurrent = (raw.current +
     * currentOffsetAmps) * currentScaleFactor; power is then recomputed
     * from the corrected voltage/current rather than trusting the
     * sensor's own uncorrected Power register.
     *
     * The identity calibration (no correction) is {0.0F, 0.0F, 1.0F}.
     */
    struct INA219Calibration {
        float voltageOffsetVolts;
        float currentOffsetAmps;
        float currentScaleFactor;
    };


    INA219Monitor();

    ~INA219Monitor();

    /*
     * Copying would duplicate the underlying I2C bus/device handles this
     * object owns, so INA219Monitor is not copyable.
     */
    INA219Monitor(const INA219Monitor&) = delete;
    INA219Monitor& operator=(const INA219Monitor&) = delete;


    /**
     * Initialises the one shared I2C bus used by every INA219 device on
     * this Node. Must be called once, before the first addSensor() call.
     *
     * Returns false when the bus could not be initialised.
     */
    bool initializeBus(const I2CBusConfiguration& busConfiguration);


    /**
     * Registers one INA219 device on the already-initialised I2C bus.
     *
     * Rejected (returns false) when:
     * - sensorConfiguration.i2cAddress is outside the INA219's valid
     *   address space (0x40 to 0x4F),
     * - another registered sensor already uses the same i2cAddress,
     * - another registered sensor already uses the same relayPin,
     * - shuntResistanceOhms or maximumExpectedCurrentAmps is not a
     *   positive, finite value, or
     * - the requested shuntResistanceOhms/maximumExpectedCurrentAmps
     *   combination would exceed the sensor's measurable shunt-voltage
     *   range.
     *
     * A sensor that fails to respond on the I2C bus at registration time
     * is still accepted as long as its configuration is valid: the
     * physical device may simply not be connected yet. Its presence is
     * re-checked by isSensorPresent() and by every read attempt.
     */
    bool addSensor(const INA219SensorConfiguration& sensorConfiguration);


    /**
     * Removes a sensor that was provisionally registered during a failed
     * hardware-configuration transaction. Calibration NVS data is kept so
     * a later corrected configuration at the same physical address does
     * not silently lose the installer-calibrated values.
     */
    bool removeSensor(std::uint8_t i2cAddress);


    /** Returns how many INA219 devices are currently registered. */
    std::size_t getNumberOfSensors() const;


    /** Returns nullptr when sensorIndex does not exist. */
    const INA219SensorConfiguration* getSensor(std::size_t sensorIndex) const;


    /** Returns nullptr when no registered sensor uses this I2C address. */
    const INA219SensorConfiguration* findSensorByI2CAddress(std::uint8_t i2cAddress) const;


    /** Returns nullptr when no registered sensor measures this relay pin. */
    const INA219SensorConfiguration* findSensorByRelayPin(std::uint8_t relayPin) const;


    /**
     * Attempts real I2C communication with the registered sensor at
     * i2cAddress and returns whether it responded.
     *
     * Returns false when i2cAddress is not registered.
     */
    bool isSensorPresent(std::uint8_t i2cAddress) const;


    /**
     * Reads real bus voltage, current and power from the registered INA219
     * at i2cAddress.
     *
     * Returns false, and leaves measurements unchanged, when i2cAddress is
     * not registered, the device does not respond, or the reading is not
     * valid (for example the sensor's own math-overflow flag is set).
     * Never fabricates a measurement.
     */
    bool readMeasurements(std::uint8_t i2cAddress, LoadMeasurements& measurements) const;


    /**
     * Finds the sensor registered for relayPin, then behaves exactly like
     * readMeasurements() for that sensor's I2C address.
     *
     * Returns false when no sensor is registered for relayPin.
     */
    bool readMeasurementsForRelayPin(std::uint8_t relayPin, LoadMeasurements& measurements) const;


    /**
     * Arms a runtime development override for the registered sensor at
     * i2cAddress: every subsequent readMeasurements()/
     * readFilteredMeasurements() call for this sensor returns
     * rawMeasurements (still passed through calibration and EMA filtering
     * exactly like a real reading) instead of performing a real I2C
     * transaction, and reports MeasurementSource::SIMULATED. This is the
     * only way a non-hardware reading can ever be produced - there is no
     * compile-time equivalent. The caller (see DevelopmentSession.h in lib/DevelopmentManager) is
     * responsible for only ever calling this while an explicit Development
     * Session is active.
     *
     * Returns false when i2cAddress is not registered.
     */
    bool setDevelopmentOverride(std::uint8_t i2cAddress, const LoadMeasurements& rawMeasurements);


    /**
     * Removes a sensor's development override, if it has one: subsequent
     * reads resume attempting real I2C communication.
     *
     * Returns false when i2cAddress is not registered.
     */
    bool clearDevelopmentOverride(std::uint8_t i2cAddress);


    /**
     * Returns which source actually produced i2cAddress's most recent
     * successful reading (MeasurementSource::NONE before any successful
     * reading has occurred this boot, or when i2cAddress is not
     * registered).
     */
    MeasurementSource getLastMeasurementSource(std::uint8_t i2cAddress) const;


    /**
     * Reads a raw measurement for i2cAddress (exactly as readMeasurements()
     * does, including the development/production and calibration
     * behaviour described above) and folds it into that sensor's running
     * Exponential Moving Average (Equations 4.2-4.4):
     *
     *     P_bar(t) = alpha P(t) + (1 - alpha) P_bar(t-1)
     *
     * applied component-wise to voltage, current and power, using the
     * sensor's configured emaAlpha. The very first successful reading for
     * a sensor seeds the filter (P_bar(0) = P(0)) since no previous
     * filtered value yet exists.
     *
     * This is the filtered snapshot the rest of the system (battery State
     * of Charge, the power budget, Best-First Search) must consume, not
     * the instantaneous readMeasurements() value, per the Measurement and
     * Filtering Model (Section 4.6.1.1).
     *
     * Returns false, and leaves filteredMeasurements/the running filter
     * state unchanged, when the underlying readMeasurements() call fails
     * (unregistered sensor, absent hardware, or an invalid reading).
     */
    bool readFilteredMeasurements(std::uint8_t i2cAddress, LoadMeasurements& filteredMeasurements);

    /**
     * Finds the sensor registered for relayPin, then behaves exactly like
     * readFilteredMeasurements() for that sensor's I2C address.
     */
    bool readFilteredMeasurementsForRelayPin(std::uint8_t relayPin, LoadMeasurements& filteredMeasurements);

    /**
     * Pure Exponential Moving Average math (Equations 4.2-4.4), applied
     * independently to voltage, current and power. Always compiled and
     * hardware-free so it is directly host-testable; readFilteredMeasurements()
     * is simply this function wired to a real/development raw reading and
     * this sensor's own persisted filter state.
     *
     * alpha is clamped to (0, 1] defensively; a sensor's own emaAlpha is
     * already validated at addSensor() time.
     */
    static LoadMeasurements applyExponentialMovingAverage(
        const LoadMeasurements& previousFilteredMeasurements,
        const LoadMeasurements& newRawMeasurements,
        float alpha
    );


    /**
     * Adopts calibration as the active offsets/scaling factors for the
     * registered sensor at i2cAddress, applied to every subsequent
     * readMeasurements() call, and persists it to NVS (ESP32 target only)
     * so it survives a reboot or power interruption (System Requirement
     * 3). Calibration storage is scoped to a namespace private to this
     * module.
     *
     * Rejected (returns false, no persistence attempted) when i2cAddress
     * is not registered, or calibration contains a non-finite value, or
     * currentScaleFactor is not strictly positive — a corrupt or
     * nonsensical calibration is never silently accepted.
     */
    bool setCalibration(std::uint8_t i2cAddress, const INA219Calibration& calibration);

    /**
     * Returns the active calibration for i2cAddress (the identity
     * calibration {0, 0, 1} when none has ever been set/restored).
     *
     * Returns false when i2cAddress is not registered.
     */
    bool getCalibration(std::uint8_t i2cAddress, INA219Calibration& calibration) const;


    /**
     * Prints one diagnostic table row per registered sensor: its I2C
     * address, the relay pin it measures, whether it was FOUND or NOT
     * FOUND on the bus, and — only for a FOUND device — its real voltage,
     * current and power. A NOT FOUND row never carries a measurement.
     */
    void printDiagnosticReport() const;


private:

    /**
     * One registered INA219 device: the configuration supplied to
     * addSensor(), plus the calibration constants derived from it and the
     * opaque I2C device handle used to talk to it.
     *
     * deviceHandle is stored as void* so this header never needs to
     * include an ESP-IDF header. It is reinterpret_cast to the real
     * i2c_master_dev_handle_t only inside INA219Monitor.cpp, and is
     * nullptr whenever no ESP-IDF device handle has been created (for
     * example on a host build, or before initializeBus() succeeds).
     */
    struct RegisteredSensor {
        INA219SensorConfiguration configuration;
        std::uint16_t calibrationRegisterValue;
        float currentLsbAmps;
        float powerLsbWatts;
        void *deviceHandle;

        /** Calibration offsets/scaling factors (identity until set/restored). */
        INA219Calibration calibration;

        /** Running EMA state consumed/updated by readFilteredMeasurements(). */
        LoadMeasurements filteredMeasurements;
        bool hasFilteredMeasurement;

        /** Set only by setDevelopmentOverride()/cleared by clearDevelopmentOverride() - see their own documentation. */
        bool hasDevelopmentOverride;
        LoadMeasurements developmentOverrideRawMeasurements;

        /**
         * Bookkeeping only, updated by readMeasurements() even though that
         * method is otherwise logically const from the caller's point of
         * view (it still performs no other observable state change).
         */
        mutable MeasurementSource lastMeasurementSource;
    };


    bool isI2CAddressRegistered(std::uint8_t i2cAddress) const;

    bool isRelayPinRegistered(std::uint8_t relayPin) const;

    RegisteredSensor* findMutableSensor(std::uint8_t i2cAddress);

    const RegisteredSensor* findSensor(std::uint8_t i2cAddress) const;

    static bool isValidCalibration(const INA219Calibration& calibration);

    /**
     * Applies calibration's offsets/scale to a raw voltage/current
     * reading and recomputes power from the corrected values. Always
     * compiled and hardware-free so calibration math is host-testable
     * independent of any real or development reading.
     */
    static LoadMeasurements applyCalibration(
        const LoadMeasurements& rawMeasurements,
        const INA219Calibration& calibration
    );

    /**
     * Restores persisted calibration for i2cAddress from NVS into
     * sensor.calibration, validating it before acceptance. Implemented
     * only under ESP_PLATFORM; leaves the identity calibration in place
     * (and returns false) when nothing has ever been persisted, NVS is
     * unavailable, or the restored value fails validation.
     */
    bool loadPersistedCalibration(RegisteredSensor& sensor);

    /**
     * Persists sensor's current calibration to NVS. Implemented only
     * under ESP_PLATFORM; the host build's implementation always returns
     * false without touching any storage.
     */
    bool persistCalibration(const RegisteredSensor& sensor) const;


    /**
     * Performs the real I2C device-add and register-configuration steps
     * for one already-validated sensor. Implemented only under
     * ESP_PLATFORM; the host build's implementation always returns false
     * without touching any hardware.
     */
    bool configureSensorHardware(RegisteredSensor& sensor);


    /**
     * Writes one 16-bit INA219 register. Implemented only under
     * ESP_PLATFORM; the host build's implementation always returns false.
     */
    bool writeRegister(const RegisteredSensor& sensor, std::uint8_t registerAddress, std::uint16_t value) const;


    /**
     * Reads one 16-bit INA219 register. Implemented only under
     * ESP_PLATFORM; the host build's implementation always returns false.
     */
    bool readRegister(const RegisteredSensor& sensor, std::uint8_t registerAddress, std::uint16_t& value) const;


    /**
     * Every INA219 device registered on this Node's bus, in the order
     * addSensor() was called.
     */
    std::vector<RegisteredSensor> sensors_;


    /**
     * The one shared I2C bus handle, stored as void* for the same reason
     * as RegisteredSensor::deviceHandle. nullptr until initializeBus()
     * succeeds.
     */
    void *busHandle_;


    /** True once initializeBus() has succeeded. */
    bool busInitialized_;


    /**
     * The clock speed initializeBus() was configured with, remembered so
     * each addSensor() call can configure its device at the same speed.
     */
    std::uint32_t busClockSpeedHz_;
};


} // namespace kilowatts

#endif // KILOWATTS_INA219_MONITOR_H
