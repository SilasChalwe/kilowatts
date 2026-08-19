/**
 * @file INA219Monitor.cpp
 * @brief Implements management of every INA219 current/power sensor wired
 *        to the current Node.
 *
 * This file is split by ESP_PLATFORM, the macro ESP-IDF defines for every
 * source file it builds:
 *
 * - Sensor registration and lookup (addSensor() and friends) is plain,
 *   hardware-free C++ and is always compiled, so it can be exercised by a
 *   host-native test (see test/INA219Monitor/) with no ESP32 involved.
 * - Real I2C register communication is compiled only under ESP_PLATFORM,
 *   using ESP-IDF's new I2C master driver (driver/i2c_master.h). A host
 *   build's version of every hardware-touching method simply returns
 *   false without fabricating a measurement or a device presence.
 *
 * Register map and conversions are from the Texas Instruments INA219
 * datasheet (SBOS448).
 */

#include "INA219Monitor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace kilowatts {


#ifndef __INTELLISENSE__
static_assert(sizeof(std::uint32_t) == sizeof(float), "float must be 32 bits to round-trip through NVS u32 storage");
#endif


#ifdef ESP_PLATFORM
static const char *TAG = "INA219_MONITOR";
#endif


namespace {

/*
 * The INA219's real, datasheet-defined I2C address space: 16 addresses
 * selectable through the A0/A1 pins.
 */
constexpr std::uint8_t INA219_MINIMUM_I2C_ADDRESS = 0x40U;
constexpr std::uint8_t INA219_MAXIMUM_I2C_ADDRESS = 0x4FU;

/*
 * Maximum shunt voltage measurable with the Configuration register value
 * this module writes (PGA = /8, see CONFIGURATION_REGISTER_VALUE below).
 */
constexpr float MAXIMUM_SHUNT_VOLTAGE_VOLTS = 0.32F;

/* Fixed internal scaling constant from the Calibration register formula. */
constexpr float CALIBRATION_SCALING_CONSTANT = 0.04096F;

/* Power register LSB is always 20x the Current register LSB. */
constexpr float POWER_LSB_SCALE = 20.0F;


bool isFinitePositive(float value)
{
    return std::isfinite(value) && value > 0.0F;
}


#ifdef ESP_PLATFORM

constexpr std::uint8_t REGISTER_CONFIGURATION = 0x00U;
constexpr std::uint8_t REGISTER_SHUNT_VOLTAGE = 0x01U;
constexpr std::uint8_t REGISTER_BUS_VOLTAGE   = 0x02U;
constexpr std::uint8_t REGISTER_POWER         = 0x03U;
constexpr std::uint8_t REGISTER_CURRENT       = 0x04U;
constexpr std::uint8_t REGISTER_CALIBRATION   = 0x05U;

/*
 * Power-on-reset default of the Configuration register (datasheet Table
 * 4), written explicitly rather than relied on implicitly: BRNG=1 (32V bus
 * range), PGA=11 (/8, +-320mV shunt full-scale), BADC=SADC=0011 (12-bit,
 * 532us conversion), MODE=111 (shunt and bus voltage, continuous).
 */
constexpr std::uint16_t CONFIGURATION_REGISTER_VALUE = 0x399FU;

/* Shunt Voltage register LSB, fixed regardless of the PGA setting. */
constexpr float SHUNT_VOLTAGE_LSB_VOLTS = 0.00001F;

/* Bus Voltage register LSB, applied after discarding the 3 status bits. */
constexpr float BUS_VOLTAGE_LSB_VOLTS = 0.004F;

/* Bus Voltage register bit 1: set when the chip's own math overflowed. */
constexpr std::uint16_t BUS_VOLTAGE_OVERFLOW_FLAG = 0x0002U;

constexpr int I2C_TRANSACTION_TIMEOUT_MS = 100;

/*
 * NVS keys are limited to 15 characters, so each key is a one-letter
 * field tag followed by the sensor's two-digit hex I2C address (e.g.
 * "v40" for the voltage offset of the sensor at 0x40).
 */
constexpr const char* NVS_NAMESPACE = "kw_ina219";

void makeCalibrationNvsKey(char key[16], char fieldTag, std::uint8_t i2cAddress)
{
    std::snprintf(key, 16, "%c%02X", fieldTag, static_cast<unsigned int>(i2cAddress));
}

#endif // ESP_PLATFORM

} // namespace


const char* toText(MeasurementSource source)
{
    switch (source) {
        case MeasurementSource::NONE: return "NONE";
        case MeasurementSource::HARDWARE: return "HARDWARE";
        case MeasurementSource::SIMULATED: return "SIMULATED";
    }

    return "UNKNOWN";
}


INA219Monitor::INA219Monitor()
    : sensors_(),
      busHandle_(nullptr),
      busInitialized_(false),
      busClockSpeedHz_(0U)
{
}


INA219Monitor::~INA219Monitor()
{
#ifdef ESP_PLATFORM
    for (RegisteredSensor& sensor : sensors_) {
        if (sensor.deviceHandle != nullptr) {
            i2c_master_bus_rm_device(reinterpret_cast<i2c_master_dev_handle_t>(sensor.deviceHandle));
            sensor.deviceHandle = nullptr;
        }
    }

    if (busHandle_ != nullptr) {
        i2c_del_master_bus(reinterpret_cast<i2c_master_bus_handle_t>(busHandle_));
        busHandle_ = nullptr;
    }
#endif
}


bool INA219Monitor::isI2CAddressRegistered(std::uint8_t i2cAddress) const
{
    return findSensor(i2cAddress) != nullptr;
}


bool INA219Monitor::isRelayPinRegistered(std::uint8_t relayPin) const
{
    return findSensorByRelayPin(relayPin) != nullptr;
}


INA219Monitor::RegisteredSensor* INA219Monitor::findMutableSensor(std::uint8_t i2cAddress)
{
    for (RegisteredSensor& sensor : sensors_) {
        if (sensor.configuration.i2cAddress == i2cAddress) {
            return &sensor;
        }
    }

    return nullptr;
}


const INA219Monitor::RegisteredSensor* INA219Monitor::findSensor(std::uint8_t i2cAddress) const
{
    for (const RegisteredSensor& sensor : sensors_) {
        if (sensor.configuration.i2cAddress == i2cAddress) {
            return &sensor;
        }
    }

    return nullptr;
}


bool INA219Monitor::initializeBus(const I2CBusConfiguration& busConfiguration)
{
#ifdef ESP_PLATFORM
    if (busInitialized_) {
        ESP_LOGW(TAG, "I2C bus already initialised; ignoring repeated initializeBus() call");
        return true;
    }

    i2c_master_bus_config_t masterBusConfig{};
    masterBusConfig.i2c_port = static_cast<i2c_port_num_t>(busConfiguration.i2cPortNumber);
    masterBusConfig.sda_io_num = static_cast<gpio_num_t>(busConfiguration.serialDataPin);
    masterBusConfig.scl_io_num = static_cast<gpio_num_t>(busConfiguration.serialClockPin);
    masterBusConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    masterBusConfig.glitch_ignore_cnt = 7U;
    masterBusConfig.flags.enable_internal_pullup = 1U;

    i2c_master_bus_handle_t newBusHandle = nullptr;
    const esp_err_t result = i2c_new_master_bus(&masterBusConfig, &newBusHandle);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialise I2C bus (port=%u sda=%u scl=%u): %s",
                 static_cast<unsigned int>(busConfiguration.i2cPortNumber),
                 static_cast<unsigned int>(busConfiguration.serialDataPin),
                 static_cast<unsigned int>(busConfiguration.serialClockPin),
                 esp_err_to_name(result));
        return false;
    }

    busHandle_ = newBusHandle;
    busInitialized_ = true;
    busClockSpeedHz_ = busConfiguration.clockSpeedHz;
    return true;
#else
    (void)busConfiguration;
    return false;
#endif
}


bool INA219Monitor::addSensor(const INA219SensorConfiguration& sensorConfiguration)
{
    if (sensorConfiguration.i2cAddress < INA219_MINIMUM_I2C_ADDRESS ||
        sensorConfiguration.i2cAddress > INA219_MAXIMUM_I2C_ADDRESS) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected sensor: I2C address 0x%02X is outside the INA219 address space (0x40-0x4F)",
                 static_cast<unsigned int>(sensorConfiguration.i2cAddress));
#endif
        return false;
    }

    if (isI2CAddressRegistered(sensorConfiguration.i2cAddress)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected sensor: I2C address 0x%02X is already registered",
                 static_cast<unsigned int>(sensorConfiguration.i2cAddress));
#endif
        return false;
    }

    if (isRelayPinRegistered(sensorConfiguration.relayPin)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected sensor 0x%02X: relay pin %u already has a registered sensor",
                 static_cast<unsigned int>(sensorConfiguration.i2cAddress),
                 static_cast<unsigned int>(sensorConfiguration.relayPin));
#endif
        return false;
    }

    if (!isFinitePositive(sensorConfiguration.shuntResistanceOhms) ||
        !isFinitePositive(sensorConfiguration.maximumExpectedCurrentAmps)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected sensor 0x%02X: shuntResistanceOhms and maximumExpectedCurrentAmps must both be positive",
                 static_cast<unsigned int>(sensorConfiguration.i2cAddress));
#endif
        return false;
    }

    const float maximumShuntVoltageVolts =
        sensorConfiguration.maximumExpectedCurrentAmps * sensorConfiguration.shuntResistanceOhms;

    if (maximumShuntVoltageVolts > MAXIMUM_SHUNT_VOLTAGE_VOLTS) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected sensor 0x%02X: maximumExpectedCurrentAmps x shuntResistanceOhms exceeds the measurable shunt-voltage range",
                 static_cast<unsigned int>(sensorConfiguration.i2cAddress));
#endif
        return false;
    }

    if (!std::isfinite(sensorConfiguration.emaAlpha) ||
        sensorConfiguration.emaAlpha <= 0.0F ||
        sensorConfiguration.emaAlpha > 1.0F) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected sensor 0x%02X: emaAlpha=%.3f must satisfy 0 < alpha <= 1",
                 static_cast<unsigned int>(sensorConfiguration.i2cAddress), sensorConfiguration.emaAlpha);
#endif
        return false;
    }

    RegisteredSensor sensor{};
    sensor.configuration = sensorConfiguration;
    sensor.currentLsbAmps = sensorConfiguration.maximumExpectedCurrentAmps / 32768.0F;

    const float calibrationValue =
        CALIBRATION_SCALING_CONSTANT / (sensor.currentLsbAmps * sensorConfiguration.shuntResistanceOhms);

    if (!std::isfinite(calibrationValue) || calibrationValue <= 0.0F || calibrationValue > 65535.0F) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected sensor 0x%02X: computed calibration value does not fit the 16-bit Calibration register",
                 static_cast<unsigned int>(sensorConfiguration.i2cAddress));
#endif
        return false;
    }

    sensor.calibrationRegisterValue = static_cast<std::uint16_t>(calibrationValue);
    sensor.powerLsbWatts = POWER_LSB_SCALE * sensor.currentLsbAmps;
    sensor.deviceHandle = nullptr;
    sensor.calibration = INA219Calibration{0.0F, 0.0F, 1.0F};
    sensor.filteredMeasurements = LoadMeasurements{0.0F, 0.0F, 0.0F};
    sensor.hasFilteredMeasurement = false;
    sensor.hasDevelopmentOverride = false;
    sensor.developmentOverrideRawMeasurements = LoadMeasurements{0.0F, 0.0F, 0.0F};
    sensor.lastMeasurementSource = MeasurementSource::NONE;

    sensors_.push_back(sensor);

    if (!configureSensorHardware(sensors_.back())) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "Sensor 0x%02X registered but did not respond during initial configuration; presence is retried on read",
                 static_cast<unsigned int>(sensorConfiguration.i2cAddress));
#endif
    }

    if (!loadPersistedCalibration(sensors_.back())) {
#ifdef ESP_PLATFORM
        ESP_LOGI(TAG, "Sensor 0x%02X starting with identity calibration (no valid persisted calibration found)",
                 static_cast<unsigned int>(sensorConfiguration.i2cAddress));
#endif
    }

    return true;
}


bool INA219Monitor::removeSensor(std::uint8_t i2cAddress)
{
    for (std::size_t index = 0U; index < sensors_.size(); ++index) {
        RegisteredSensor& sensor = sensors_[index];
        if (sensor.configuration.i2cAddress != i2cAddress) {
            continue;
        }

#ifdef ESP_PLATFORM
        if (sensor.deviceHandle != nullptr) {
            const esp_err_t result = i2c_master_bus_rm_device(
                reinterpret_cast<i2c_master_dev_handle_t>(sensor.deviceHandle));
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Could not remove INA219 0x%02X while rolling back configuration: %s",
                         static_cast<unsigned int>(i2cAddress), esp_err_to_name(result));
            }
            sensor.deviceHandle = nullptr;
        }
#endif

        sensors_.erase(sensors_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    return false;
}


std::size_t INA219Monitor::getNumberOfSensors() const
{
    return sensors_.size();
}


const INA219Monitor::INA219SensorConfiguration* INA219Monitor::getSensor(std::size_t sensorIndex) const
{
    if (sensorIndex >= sensors_.size()) {
        return nullptr;
    }

    return &sensors_[sensorIndex].configuration;
}


const INA219Monitor::INA219SensorConfiguration* INA219Monitor::findSensorByI2CAddress(std::uint8_t i2cAddress) const
{
    const RegisteredSensor* sensor = findSensor(i2cAddress);
    return sensor != nullptr ? &sensor->configuration : nullptr;
}


const INA219Monitor::INA219SensorConfiguration* INA219Monitor::findSensorByRelayPin(std::uint8_t relayPin) const
{
    for (const RegisteredSensor& sensor : sensors_) {
        if (sensor.configuration.relayPin == relayPin) {
            return &sensor.configuration;
        }
    }

    return nullptr;
}


bool INA219Monitor::isSensorPresent(std::uint8_t i2cAddress) const
{
#ifdef ESP_PLATFORM
    const RegisteredSensor* sensor = findSensor(i2cAddress);
    if (sensor == nullptr || !busInitialized_) {
        return false;
    }

    const esp_err_t result = i2c_master_probe(
        reinterpret_cast<i2c_master_bus_handle_t>(busHandle_),
        i2cAddress,
        I2C_TRANSACTION_TIMEOUT_MS);

    return result == ESP_OK;
#else
    (void)i2cAddress;
    return false;
#endif
}


bool INA219Monitor::readMeasurements(std::uint8_t i2cAddress, LoadMeasurements& measurements) const
{
#ifdef ESP_PLATFORM
    const RegisteredSensor* sensor = findSensor(i2cAddress);
    if (sensor == nullptr) {
        ESP_LOGW(TAG, "Cannot read sensor 0x%02X: not registered", static_cast<unsigned int>(i2cAddress));
        return false;
    }

    if (sensor->hasDevelopmentOverride) {
        /*
         * An explicit runtime override (see setDevelopmentOverride()) is
         * the ONLY way a non-hardware reading is ever produced - no
         * sensor has one by default. Every downstream step (calibration
         * below, EMA filtering in readFilteredMeasurements(), battery
         * SoC, the power limit, Best-First Search) is the identical
         * production code path regardless of this branch.
         */
        measurements = applyCalibration(sensor->developmentOverrideRawMeasurements, sensor->calibration);
        sensor->lastMeasurementSource = MeasurementSource::SIMULATED;

        ESP_LOGI(TAG, "SENSOR_SAMPLE source=SIMULATED i2c=0x%02X V=%.3fV I=%.3fA P=%.3fW",
                 static_cast<unsigned int>(i2cAddress),
                 static_cast<double>(measurements.voltageVolts),
                 static_cast<double>(measurements.currentAmps),
                 static_cast<double>(measurements.powerWatts));

        return true;
    }

    if (sensor->deviceHandle == nullptr) {
        ESP_LOGW(TAG, "Cannot read sensor 0x%02X: not configured on the bus",
                 static_cast<unsigned int>(i2cAddress));
        return false;
    }

    std::uint16_t busVoltageRaw = 0U;
    std::uint16_t shuntVoltageRaw = 0U;
    std::uint16_t currentRaw = 0U;
    std::uint16_t powerRaw = 0U;

    if (!readRegister(*sensor, REGISTER_BUS_VOLTAGE, busVoltageRaw)) {
        ESP_LOGW(TAG, "Sensor 0x%02X did not respond while reading the Bus Voltage register",
                 static_cast<unsigned int>(i2cAddress));
        return false;
    }

    if ((busVoltageRaw & BUS_VOLTAGE_OVERFLOW_FLAG) != 0U) {
        ESP_LOGW(TAG, "Sensor 0x%02X reports a math overflow; discarding this reading",
                 static_cast<unsigned int>(i2cAddress));
        return false;
    }

    if (!readRegister(*sensor, REGISTER_SHUNT_VOLTAGE, shuntVoltageRaw)) {
        ESP_LOGW(TAG, "Sensor 0x%02X did not respond while reading the Shunt Voltage register",
                 static_cast<unsigned int>(i2cAddress));
        return false;
    }

    const float shuntVoltageVolts = std::fabs(
        static_cast<float>(static_cast<std::int16_t>(shuntVoltageRaw)) * SHUNT_VOLTAGE_LSB_VOLTS);

    if (shuntVoltageVolts >= MAXIMUM_SHUNT_VOLTAGE_VOLTS) {
        ESP_LOGW(TAG, "Sensor 0x%02X shunt voltage is at its measurable limit; discarding this reading",
                 static_cast<unsigned int>(i2cAddress));
        return false;
    }

    if (!readRegister(*sensor, REGISTER_CURRENT, currentRaw) ||
        !readRegister(*sensor, REGISTER_POWER, powerRaw)) {
        ESP_LOGW(TAG, "Sensor 0x%02X did not respond while reading the Current/Power registers",
                 static_cast<unsigned int>(i2cAddress));
        return false;
    }

    LoadMeasurements rawMeasurements{};
    rawMeasurements.voltageVolts = static_cast<float>(busVoltageRaw >> 3) * BUS_VOLTAGE_LSB_VOLTS;
    rawMeasurements.currentAmps = static_cast<float>(static_cast<std::int16_t>(currentRaw)) * sensor->currentLsbAmps;
    rawMeasurements.powerWatts = static_cast<float>(powerRaw) * sensor->powerLsbWatts;

    measurements = applyCalibration(rawMeasurements, sensor->calibration);
    sensor->lastMeasurementSource = MeasurementSource::HARDWARE;

    ESP_LOGI(TAG, "SENSOR_SAMPLE source=HARDWARE i2c=0x%02X V=%.3fV I=%.3fA P=%.3fW",
             static_cast<unsigned int>(i2cAddress),
             static_cast<double>(measurements.voltageVolts),
             static_cast<double>(measurements.currentAmps),
             static_cast<double>(measurements.powerWatts));

    return true;
#else
    (void)i2cAddress;
    (void)measurements;
    return false;
#endif // ESP_PLATFORM
}


bool INA219Monitor::setDevelopmentOverride(std::uint8_t i2cAddress, const LoadMeasurements& rawMeasurements)
{
    RegisteredSensor* sensor = findMutableSensor(i2cAddress);
    if (sensor == nullptr) {
        return false;
    }

    sensor->hasDevelopmentOverride = true;
    sensor->developmentOverrideRawMeasurements = rawMeasurements;

#ifdef ESP_PLATFORM
    ESP_LOGW(TAG, "DEV_OVERRIDE_SET i2c=0x%02X V=%.3fV I=%.3fA (raw, before calibration/EMA)",
             static_cast<unsigned int>(i2cAddress),
             static_cast<double>(rawMeasurements.voltageVolts),
             static_cast<double>(rawMeasurements.currentAmps));
#endif

    return true;
}


bool INA219Monitor::clearDevelopmentOverride(std::uint8_t i2cAddress)
{
    RegisteredSensor* sensor = findMutableSensor(i2cAddress);
    if (sensor == nullptr) {
        return false;
    }

    sensor->hasDevelopmentOverride = false;

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "DEV_OVERRIDE_CLEARED i2c=0x%02X", static_cast<unsigned int>(i2cAddress));
#endif

    return true;
}


MeasurementSource INA219Monitor::getLastMeasurementSource(std::uint8_t i2cAddress) const
{
    const RegisteredSensor* sensor = findSensor(i2cAddress);
    return sensor != nullptr ? sensor->lastMeasurementSource : MeasurementSource::NONE;
}


bool INA219Monitor::readMeasurementsForRelayPin(std::uint8_t relayPin, LoadMeasurements& measurements) const
{
    const INA219SensorConfiguration* sensorConfiguration = findSensorByRelayPin(relayPin);
    if (sensorConfiguration == nullptr) {
        return false;
    }

    return readMeasurements(sensorConfiguration->i2cAddress, measurements);
}


bool INA219Monitor::readFilteredMeasurements(std::uint8_t i2cAddress, LoadMeasurements& filteredMeasurements)
{
    RegisteredSensor* sensor = findMutableSensor(i2cAddress);
    if (sensor == nullptr) {
        return false;
    }

    LoadMeasurements rawMeasurements{};
    if (!readMeasurements(i2cAddress, rawMeasurements)) {
        return false;
    }

    if (!sensor->hasFilteredMeasurement) {
        /* First successful reading seeds the filter: P_bar(0) = P(0). */
        sensor->filteredMeasurements = rawMeasurements;
        sensor->hasFilteredMeasurement = true;
    } else {
        sensor->filteredMeasurements = applyExponentialMovingAverage(
            sensor->filteredMeasurements, rawMeasurements, sensor->configuration.emaAlpha);
    }

    filteredMeasurements = sensor->filteredMeasurements;
    return true;
}


bool INA219Monitor::readFilteredMeasurementsForRelayPin(std::uint8_t relayPin, LoadMeasurements& filteredMeasurements)
{
    const INA219SensorConfiguration* sensorConfiguration = findSensorByRelayPin(relayPin);
    if (sensorConfiguration == nullptr) {
        return false;
    }

    return readFilteredMeasurements(sensorConfiguration->i2cAddress, filteredMeasurements);
}


LoadMeasurements INA219Monitor::applyExponentialMovingAverage(
    const LoadMeasurements& previousFilteredMeasurements,
    const LoadMeasurements& newRawMeasurements,
    float alpha)
{
    const float clampedAlpha = std::isfinite(alpha) ? std::max(0.0F, std::min(alpha, 1.0F)) : 1.0F;

    /* P_bar(t) = alpha P(t) + (1 - alpha) P_bar(t-1). */
    LoadMeasurements filtered{};
    filtered.voltageVolts =
        (clampedAlpha * newRawMeasurements.voltageVolts) +
        ((1.0F - clampedAlpha) * previousFilteredMeasurements.voltageVolts);
    filtered.currentAmps =
        (clampedAlpha * newRawMeasurements.currentAmps) +
        ((1.0F - clampedAlpha) * previousFilteredMeasurements.currentAmps);
    filtered.powerWatts =
        (clampedAlpha * newRawMeasurements.powerWatts) +
        ((1.0F - clampedAlpha) * previousFilteredMeasurements.powerWatts);

    return filtered;
}


bool INA219Monitor::isValidCalibration(const INA219Calibration& calibration)
{
    return std::isfinite(calibration.voltageOffsetVolts) &&
           std::isfinite(calibration.currentOffsetAmps) &&
           std::isfinite(calibration.currentScaleFactor) &&
           calibration.currentScaleFactor > 0.0F;
}


LoadMeasurements INA219Monitor::applyCalibration(
    const LoadMeasurements& rawMeasurements,
    const INA219Calibration& calibration)
{
    LoadMeasurements corrected{};
    corrected.voltageVolts = rawMeasurements.voltageVolts + calibration.voltageOffsetVolts;
    corrected.currentAmps = (rawMeasurements.currentAmps + calibration.currentOffsetAmps) * calibration.currentScaleFactor;

    /*
     * Power is recomputed from the corrected voltage/current rather than
     * scaling the sensor's own raw Power register, since an offset/scale
     * correction on V or I alone would otherwise leave power silently
     * uncorrected.
     */
    corrected.powerWatts = corrected.voltageVolts * corrected.currentAmps;

    return corrected;
}


bool INA219Monitor::setCalibration(std::uint8_t i2cAddress, const INA219Calibration& calibration)
{
    RegisteredSensor* sensor = findMutableSensor(i2cAddress);
    if (sensor == nullptr) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "Cannot set calibration for 0x%02X: not registered", static_cast<unsigned int>(i2cAddress));
#endif
        return false;
    }

    if (!isValidCalibration(calibration)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected calibration for 0x%02X: voltageOffset=%.4f currentOffset=%.4f currentScale=%.4f",
                 static_cast<unsigned int>(i2cAddress),
                 static_cast<double>(calibration.voltageOffsetVolts),
                 static_cast<double>(calibration.currentOffsetAmps),
                 static_cast<double>(calibration.currentScaleFactor));
#endif
        return false;
    }

    sensor->calibration = calibration;

#ifdef ESP_PLATFORM
    const bool persisted = persistCalibration(*sensor);
    ESP_LOGI(TAG, "Calibration set for 0x%02X: voltageOffset=%.4fV currentOffset=%.4fA currentScale=%.4f persisted=%s",
             static_cast<unsigned int>(i2cAddress),
             static_cast<double>(calibration.voltageOffsetVolts),
             static_cast<double>(calibration.currentOffsetAmps),
             static_cast<double>(calibration.currentScaleFactor),
             persisted ? "Yes" : "No");
#else
    persistCalibration(*sensor);
#endif

    return true;
}


bool INA219Monitor::getCalibration(std::uint8_t i2cAddress, INA219Calibration& calibration) const
{
    const RegisteredSensor* sensor = findSensor(i2cAddress);
    if (sensor == nullptr) {
        return false;
    }

    calibration = sensor->calibration;
    return true;
}


bool INA219Monitor::loadPersistedCalibration(RegisteredSensor& sensor)
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        return false;
    }

    INA219Calibration restored{0.0F, 0.0F, 1.0F};
    char key[16] = {};
    bool everyFieldFound = true;

    auto readFloatField = [&](char fieldTag, float& value) {
        makeCalibrationNvsKey(key, fieldTag, sensor.configuration.i2cAddress);
        std::uint32_t rawBits = 0U;
        const esp_err_t fieldResult = nvs_get_u32(handle, key, &rawBits);
        if (fieldResult != ESP_OK) {
            everyFieldFound = false;
            return;
        }
        std::memcpy(&value, &rawBits, sizeof(value));
    };

    readFloatField('v', restored.voltageOffsetVolts);
    readFloatField('i', restored.currentOffsetAmps);
    readFloatField('s', restored.currentScaleFactor);

    nvs_close(handle);

    if (!everyFieldFound) {
        return false;
    }

    if (!isValidCalibration(restored)) {
        ESP_LOGW(TAG, "Discarding corrupt persisted calibration for 0x%02X",
                 static_cast<unsigned int>(sensor.configuration.i2cAddress));
        return false;
    }

    sensor.calibration = restored;
    ESP_LOGI(TAG, "Restored persisted calibration for 0x%02X: voltageOffset=%.4fV currentOffset=%.4fA currentScale=%.4f",
             static_cast<unsigned int>(sensor.configuration.i2cAddress),
             static_cast<double>(restored.voltageOffsetVolts),
             static_cast<double>(restored.currentOffsetAmps),
             static_cast<double>(restored.currentScaleFactor));
    return true;
#else
    (void)sensor;
    return false;
#endif
}


bool INA219Monitor::persistCalibration(const RegisteredSensor& sensor) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS to persist calibration for 0x%02X: %s",
                 static_cast<unsigned int>(sensor.configuration.i2cAddress), esp_err_to_name(result));
        return false;
    }

    char key[16] = {};
    bool everyFieldWritten = true;

    auto writeFloatField = [&](char fieldTag, float value) {
        makeCalibrationNvsKey(key, fieldTag, sensor.configuration.i2cAddress);
        std::uint32_t rawBits = 0U;
        std::memcpy(&rawBits, &value, sizeof(value));
        if (nvs_set_u32(handle, key, rawBits) != ESP_OK) {
            everyFieldWritten = false;
        }
    };

    writeFloatField('v', sensor.calibration.voltageOffsetVolts);
    writeFloatField('i', sensor.calibration.currentOffsetAmps);
    writeFloatField('s', sensor.calibration.currentScaleFactor);

    if (everyFieldWritten) {
        result = nvs_commit(handle);
        everyFieldWritten = (result == ESP_OK);
    }

    nvs_close(handle);
    return everyFieldWritten;
#else
    (void)sensor;
    return false;
#endif
}


void INA219Monitor::printDiagnosticReport() const
{
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "INA219 DEVICES");
    ESP_LOGI(TAG, "--------------------------------------------------------------");
    ESP_LOGI(TAG, "%-9s %-11s %-12s %-9s %-9s %-9s", "Address", "Relay Pin", "Status", "Voltage", "Current", "Power");

    for (const RegisteredSensor& sensor : sensors_) {
        const unsigned int i2cAddress = static_cast<unsigned int>(sensor.configuration.i2cAddress);
        const unsigned int relayPin = static_cast<unsigned int>(sensor.configuration.relayPin);

        if (!isSensorPresent(sensor.configuration.i2cAddress)) {
            ESP_LOGI(TAG, "0x%02X      %-11u NOT FOUND", i2cAddress, relayPin);
            continue;
        }

        LoadMeasurements measurements{};
        if (readMeasurements(sensor.configuration.i2cAddress, measurements)) {
            ESP_LOGI(TAG, "0x%02X      %-11u FOUND        %6.2f V  %6.2f A  %6.2f W",
                     i2cAddress, relayPin,
                     static_cast<double>(measurements.voltageVolts),
                     static_cast<double>(measurements.currentAmps),
                     static_cast<double>(measurements.powerWatts));
        } else {
            ESP_LOGI(TAG, "0x%02X      %-11u FOUND        READ FAILED", i2cAddress, relayPin);
        }
    }

    ESP_LOGI(TAG, "--------------------------------------------------------------");
#endif
}


bool INA219Monitor::configureSensorHardware(RegisteredSensor& sensor)
{
#ifdef ESP_PLATFORM
    if (!busInitialized_) {
        ESP_LOGE(TAG, "Cannot configure sensor 0x%02X: I2C bus has not been initialised",
                 static_cast<unsigned int>(sensor.configuration.i2cAddress));
        return false;
    }

    i2c_device_config_t deviceConfig{};
    deviceConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    deviceConfig.device_address = sensor.configuration.i2cAddress;
    deviceConfig.scl_speed_hz = busClockSpeedHz_;

    i2c_master_dev_handle_t deviceHandle = nullptr;
    const esp_err_t addResult = i2c_master_bus_add_device(
        reinterpret_cast<i2c_master_bus_handle_t>(busHandle_), &deviceConfig, &deviceHandle);

    if (addResult != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device 0x%02X to the bus: %s",
                 static_cast<unsigned int>(sensor.configuration.i2cAddress), esp_err_to_name(addResult));
        return false;
    }

    sensor.deviceHandle = deviceHandle;

    if (!writeRegister(sensor, REGISTER_CONFIGURATION, CONFIGURATION_REGISTER_VALUE)) {
        ESP_LOGE(TAG, "Sensor 0x%02X did not acknowledge the Configuration register",
                 static_cast<unsigned int>(sensor.configuration.i2cAddress));
        return false;
    }

    if (!writeRegister(sensor, REGISTER_CALIBRATION, sensor.calibrationRegisterValue)) {
        ESP_LOGE(TAG, "Sensor 0x%02X did not acknowledge the Calibration register",
                 static_cast<unsigned int>(sensor.configuration.i2cAddress));
        return false;
    }

    return true;
#else
    (void)sensor;
    return false;
#endif
}


bool INA219Monitor::writeRegister(const RegisteredSensor& sensor, std::uint8_t registerAddress, std::uint16_t value) const
{
#ifdef ESP_PLATFORM
    if (sensor.deviceHandle == nullptr) {
        return false;
    }

    const std::uint8_t writeBuffer[3] = {
        registerAddress,
        static_cast<std::uint8_t>((value >> 8) & 0xFFU),
        static_cast<std::uint8_t>(value & 0xFFU)
    };

    const esp_err_t result = i2c_master_transmit(
        reinterpret_cast<i2c_master_dev_handle_t>(sensor.deviceHandle),
        writeBuffer, sizeof(writeBuffer),
        I2C_TRANSACTION_TIMEOUT_MS);

    return result == ESP_OK;
#else
    (void)sensor;
    (void)registerAddress;
    (void)value;
    return false;
#endif
}


bool INA219Monitor::readRegister(const RegisteredSensor& sensor, std::uint8_t registerAddress, std::uint16_t& value) const
{
#ifdef ESP_PLATFORM
    if (sensor.deviceHandle == nullptr) {
        return false;
    }

    const std::uint8_t writeBuffer[1] = { registerAddress };
    std::uint8_t readBuffer[2] = { 0U, 0U };

    const esp_err_t result = i2c_master_transmit_receive(
        reinterpret_cast<i2c_master_dev_handle_t>(sensor.deviceHandle),
        writeBuffer, sizeof(writeBuffer),
        readBuffer, sizeof(readBuffer),
        I2C_TRANSACTION_TIMEOUT_MS);

    if (result != ESP_OK) {
        return false;
    }

    value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(readBuffer[0]) << 8) | readBuffer[1]);
    return true;
#else
    (void)sensor;
    (void)registerAddress;
    (void)value;
    return false;
#endif
}


} // namespace kilowatts
