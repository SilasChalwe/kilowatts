/**
 * @file INA219Monitor.h
 * @brief Reads the single INA219 connected to Central's battery bus.
 */
#ifndef KILOWATTS_INA219_MONITOR_H
#define KILOWATTS_INA219_MONITOR_H

#include "Load.h"

#include <cstdint>

namespace kilowatts {

enum class MeasurementSource : std::uint8_t {
    NONE = 0U,
    SIMULATED = 1U,
    HARDWARE = 2U
};

const char* toText(MeasurementSource source);

class INA219Monitor {
public:
    struct I2CBusConfiguration {
        std::uint8_t serialDataPin;
        std::uint8_t serialClockPin;
        std::uint32_t clockSpeedHz;
        std::uint8_t i2cPortNumber;
    };

    struct SensorConfiguration {
        float shuntResistanceOhms;
        float maximumExpectedCurrentAmps;
        float emaAlpha;
    };

    struct Calibration {
        float voltageOffsetVolts;
        float currentOffsetAmps;
        float currentScaleFactor;
    };

    INA219Monitor();
    ~INA219Monitor();

    INA219Monitor(const INA219Monitor&) = delete;
    INA219Monitor& operator=(const INA219Monitor&) = delete;

    bool initializeBus(const I2CBusConfiguration& configuration);
    bool configureSensor(const SensorConfiguration& configuration);

    bool isConfigured() const;
    bool isSensorPresent() const;

    bool readMeasurements(LoadMeasurements& measurements) const;
    bool readFilteredMeasurements(LoadMeasurements& measurements);

    MeasurementSource getLastMeasurementSource() const;

    bool setCalibration(const Calibration& calibration);
    Calibration getCalibration() const;

    static LoadMeasurements applyExponentialMovingAverage(
        const LoadMeasurements& previous,
        const LoadMeasurements& current,
        float alpha);

    void printDiagnosticReport() const;

private:
    static bool isValidSensorConfiguration(const SensorConfiguration& configuration);
    static bool isValidCalibration(const Calibration& calibration);
    static LoadMeasurements applyCalibration(const LoadMeasurements& raw, const Calibration& calibration);

    bool configureHardware();
    bool readHardwareMeasurements(LoadMeasurements& measurements) const;
    bool loadPersistedCalibration();
    bool persistCalibration() const;

    SensorConfiguration configuration_;
    Calibration calibration_;
    LoadMeasurements filteredMeasurements_;

    bool busInitialized_;
    bool sensorConfigured_;
    bool hasFilteredMeasurement_;

    std::uint32_t busClockSpeedHz_;
    std::uint16_t calibrationRegisterValue_;
    float currentLsbAmps_;

    void* busHandle_;
    void* deviceHandle_;

    mutable MeasurementSource lastMeasurementSource_;
};

} // namespace kilowatts

#endif // KILOWATTS_INA219_MONITOR_H
