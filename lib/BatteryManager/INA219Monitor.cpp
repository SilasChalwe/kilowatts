/**
 * @file INA219Monitor.cpp
 * @brief Single battery-bus INA219 reader.
 */
#include "INA219Monitor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"
#endif

namespace kilowatts {

namespace {

constexpr std::uint8_t INA219_I2C_ADDRESS = 0x40U;
constexpr float MAXIMUM_SHUNT_VOLTAGE_VOLTS = 0.32F;
constexpr float CALIBRATION_SCALING_CONSTANT = 0.04096F;

/*
 * Current bench mode. Change only this file when the real INA219 is ready.
 * false = read the physical INA219 at 0x40.
 */
constexpr bool USE_SIMULATED_READING = true;
constexpr LoadMeasurements SIMULATED_READING{15.0F, 3.0F, 45.0F};

#ifdef ESP_PLATFORM
static const char* TAG = "INA219";

constexpr std::uint8_t REGISTER_CONFIGURATION = 0x00U;
constexpr std::uint8_t REGISTER_BUS_VOLTAGE = 0x02U;
constexpr std::uint8_t REGISTER_CALIBRATION = 0x05U;
constexpr std::uint8_t REGISTER_CURRENT = 0x04U;
constexpr std::uint16_t CONFIGURATION_REGISTER_VALUE = 0x399FU;
constexpr float BUS_VOLTAGE_LSB_VOLTS = 0.004F;
constexpr std::uint16_t BUS_VOLTAGE_OVERFLOW_FLAG = 0x0002U;
constexpr int I2C_TIMEOUT_MS = 100;

constexpr const char* NVS_NAMESPACE = "kw_ina219";
constexpr const char* NVS_KEY_VOLTAGE_OFFSET = "v_offset";
constexpr const char* NVS_KEY_CURRENT_OFFSET = "i_offset";
constexpr const char* NVS_KEY_CURRENT_SCALE = "i_scale";

bool writeRegister(i2c_master_dev_handle_t device, std::uint8_t reg, std::uint16_t value)
{
    const std::uint8_t bytes[3]{
        reg,
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(value & 0xFFU)};
    return i2c_master_transmit(device, bytes, sizeof(bytes), I2C_TIMEOUT_MS) == ESP_OK;
}

bool readRegister(i2c_master_dev_handle_t device, std::uint8_t reg, std::uint16_t& value)
{
    std::uint8_t bytes[2]{};
    if (i2c_master_transmit_receive(device, &reg, 1U, bytes, sizeof(bytes), I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
    return true;
}

bool writeFloat(nvs_handle_t handle, const char* key, float value)
{
    std::uint32_t bits = 0U;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32 bits");
    std::memcpy(&bits, &value, sizeof(value));
    return nvs_set_u32(handle, key, bits) == ESP_OK;
}

bool readFloat(nvs_handle_t handle, const char* key, float& value)
{
    std::uint32_t bits = 0U;
    if (nvs_get_u32(handle, key, &bits) != ESP_OK) {
        return false;
    }
    static_assert(sizeof(bits) == sizeof(value), "float must be 32 bits");
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value);
}
#endif

} // namespace

const char* toText(MeasurementSource source)
{
    switch (source) {
        case MeasurementSource::NONE: return "NONE";
        case MeasurementSource::SIMULATED: return "SIMULATED";
        case MeasurementSource::HARDWARE: return "HARDWARE";
    }
    return "UNKNOWN";
}

INA219Monitor::INA219Monitor()
    : configuration_{0.0F, 0.0F, 1.0F},
      calibration_{0.0F, 0.0F, 1.0F},
      filteredMeasurements_{0.0F, 0.0F, 0.0F},
      busInitialized_(false),
      sensorConfigured_(false),
      hasFilteredMeasurement_(false),
      busClockSpeedHz_(0U),
      calibrationRegisterValue_(0U),
      currentLsbAmps_(0.0F),
      busHandle_(nullptr),
      deviceHandle_(nullptr),
      lastMeasurementSource_(MeasurementSource::NONE)
{
}

INA219Monitor::~INA219Monitor()
{
#ifdef ESP_PLATFORM
    if (deviceHandle_ != nullptr) {
        i2c_master_bus_rm_device(reinterpret_cast<i2c_master_dev_handle_t>(deviceHandle_));
        deviceHandle_ = nullptr;
    }
    if (busHandle_ != nullptr) {
        i2c_del_master_bus(reinterpret_cast<i2c_master_bus_handle_t>(busHandle_));
        busHandle_ = nullptr;
    }
#endif
}

bool INA219Monitor::initializeBus(const I2CBusConfiguration& configuration)
{
#ifdef ESP_PLATFORM
    if (busInitialized_) {
        return true;
    }

    i2c_master_bus_config_t bus{};
    bus.i2c_port = static_cast<i2c_port_num_t>(configuration.i2cPortNumber);
    bus.sda_io_num = static_cast<gpio_num_t>(configuration.serialDataPin);
    bus.scl_io_num = static_cast<gpio_num_t>(configuration.serialClockPin);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7U;
    bus.flags.enable_internal_pullup = 1U;

    i2c_master_bus_handle_t handle = nullptr;
    if (i2c_new_master_bus(&bus, &handle) != ESP_OK) {
        return false;
    }

    busHandle_ = handle;
    busClockSpeedHz_ = configuration.clockSpeedHz;
    busInitialized_ = true;
    return true;
#else
    (void)configuration;
    return false;
#endif
}

bool INA219Monitor::isValidSensorConfiguration(const SensorConfiguration& configuration)
{
    if (!std::isfinite(configuration.shuntResistanceOhms) || configuration.shuntResistanceOhms <= 0.0F ||
        !std::isfinite(configuration.maximumExpectedCurrentAmps) || configuration.maximumExpectedCurrentAmps <= 0.0F ||
        !std::isfinite(configuration.emaAlpha) || configuration.emaAlpha <= 0.0F || configuration.emaAlpha > 1.0F) {
        return false;
    }

    return configuration.shuntResistanceOhms * configuration.maximumExpectedCurrentAmps <=
           MAXIMUM_SHUNT_VOLTAGE_VOLTS;
}

bool INA219Monitor::configureSensor(const SensorConfiguration& configuration)
{
    if (!isValidSensorConfiguration(configuration)) {
        return false;
    }

    const float currentLsb = configuration.maximumExpectedCurrentAmps / 32768.0F;
    const float calibrationValue = CALIBRATION_SCALING_CONSTANT /
        (currentLsb * configuration.shuntResistanceOhms);

    if (!std::isfinite(calibrationValue) || calibrationValue <= 0.0F || calibrationValue > 65535.0F) {
        return false;
    }

    configuration_ = configuration;
    currentLsbAmps_ = currentLsb;
    calibrationRegisterValue_ = static_cast<std::uint16_t>(calibrationValue);
    calibration_ = Calibration{0.0F, 0.0F, 1.0F};
    filteredMeasurements_ = LoadMeasurements{0.0F, 0.0F, 0.0F};
    hasFilteredMeasurement_ = false;
    lastMeasurementSource_ = MeasurementSource::NONE;
    sensorConfigured_ = true;

    loadPersistedCalibration();

    if (USE_SIMULATED_READING) {
        return true;
    }

    return configureHardware();
}

bool INA219Monitor::isConfigured() const
{
    return sensorConfigured_;
}

bool INA219Monitor::configureHardware()
{
#ifdef ESP_PLATFORM
    if (!busInitialized_ || busHandle_ == nullptr || !sensorConfigured_) {
        return false;
    }

    if (deviceHandle_ == nullptr) {
        i2c_device_config_t device{};
        device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device.device_address = INA219_I2C_ADDRESS;
        device.scl_speed_hz = busClockSpeedHz_;

        i2c_master_dev_handle_t handle = nullptr;
        if (i2c_master_bus_add_device(
                reinterpret_cast<i2c_master_bus_handle_t>(busHandle_), &device, &handle) != ESP_OK) {
            return false;
        }
        deviceHandle_ = handle;
    }

    const auto device = reinterpret_cast<i2c_master_dev_handle_t>(deviceHandle_);
    return writeRegister(device, REGISTER_CONFIGURATION, CONFIGURATION_REGISTER_VALUE) &&
           writeRegister(device, REGISTER_CALIBRATION, calibrationRegisterValue_);
#else
    return false;
#endif
}

bool INA219Monitor::isSensorPresent() const
{
    if (!sensorConfigured_) {
        return false;
    }

    if (USE_SIMULATED_READING) {
        return true;
    }

#ifdef ESP_PLATFORM
    if (!busInitialized_ || busHandle_ == nullptr) {
        return false;
    }
    return i2c_master_probe(
               reinterpret_cast<i2c_master_bus_handle_t>(busHandle_),
               INA219_I2C_ADDRESS,
               I2C_TIMEOUT_MS) == ESP_OK;
#else
    return false;
#endif
}

bool INA219Monitor::readHardwareMeasurements(LoadMeasurements& measurements) const
{
#ifdef ESP_PLATFORM
    if (!sensorConfigured_ || deviceHandle_ == nullptr || !isSensorPresent()) {
        return false;
    }

    const auto device = reinterpret_cast<i2c_master_dev_handle_t>(deviceHandle_);
    std::uint16_t busRaw = 0U;
    std::uint16_t currentRaw = 0U;

    if (!readRegister(device, REGISTER_BUS_VOLTAGE, busRaw) ||
        !readRegister(device, REGISTER_CURRENT, currentRaw) ||
        (busRaw & BUS_VOLTAGE_OVERFLOW_FLAG) != 0U) {
        return false;
    }

    const auto signedCurrent = static_cast<std::int16_t>(currentRaw);
    LoadMeasurements raw{};
    raw.voltageVolts = static_cast<float>(busRaw >> 3U) * BUS_VOLTAGE_LSB_VOLTS;
    raw.currentAmps = static_cast<float>(signedCurrent) * currentLsbAmps_;
    raw.powerWatts = raw.voltageVolts * raw.currentAmps;

    if (!std::isfinite(raw.voltageVolts) || !std::isfinite(raw.currentAmps) || !std::isfinite(raw.powerWatts)) {
        return false;
    }

    measurements = raw;
    return true;
#else
    (void)measurements;
    return false;
#endif
}

LoadMeasurements INA219Monitor::applyCalibration(const LoadMeasurements& raw, const Calibration& calibration)
{
    LoadMeasurements corrected{};
    corrected.voltageVolts = raw.voltageVolts + calibration.voltageOffsetVolts;
    corrected.currentAmps = (raw.currentAmps + calibration.currentOffsetAmps) * calibration.currentScaleFactor;
    corrected.powerWatts = corrected.voltageVolts * corrected.currentAmps;
    return corrected;
}

bool INA219Monitor::readMeasurements(LoadMeasurements& measurements) const
{
    if (!sensorConfigured_) {
        return false;
    }

    LoadMeasurements raw{};
    if (USE_SIMULATED_READING) {
        raw = SIMULATED_READING;
        lastMeasurementSource_ = MeasurementSource::SIMULATED;
    } else {
        if (!readHardwareMeasurements(raw)) {
            return false;
        }
        lastMeasurementSource_ = MeasurementSource::HARDWARE;
    }

    const LoadMeasurements corrected = applyCalibration(raw, calibration_);
    if (!std::isfinite(corrected.voltageVolts) || !std::isfinite(corrected.currentAmps) ||
        !std::isfinite(corrected.powerWatts)) {
        return false;
    }

    measurements = corrected;
    return true;
}

LoadMeasurements INA219Monitor::applyExponentialMovingAverage(
    const LoadMeasurements& previous,
    const LoadMeasurements& current,
    float alpha)
{
    const float a = std::max(0.0F, std::min(alpha, 1.0F));
    const float b = 1.0F - a;
    return LoadMeasurements{
        (a * current.voltageVolts) + (b * previous.voltageVolts),
        (a * current.currentAmps) + (b * previous.currentAmps),
        (a * current.powerWatts) + (b * previous.powerWatts)};
}

bool INA219Monitor::readFilteredMeasurements(LoadMeasurements& measurements)
{
    LoadMeasurements current{};
    if (!readMeasurements(current)) {
        return false;
    }

    filteredMeasurements_ = hasFilteredMeasurement_
        ? applyExponentialMovingAverage(filteredMeasurements_, current, configuration_.emaAlpha)
        : current;
    hasFilteredMeasurement_ = true;
    measurements = filteredMeasurements_;
    return true;
}

MeasurementSource INA219Monitor::getLastMeasurementSource() const
{
    return lastMeasurementSource_;
}

bool INA219Monitor::isValidCalibration(const Calibration& calibration)
{
    return std::isfinite(calibration.voltageOffsetVolts) &&
           std::isfinite(calibration.currentOffsetAmps) &&
           std::isfinite(calibration.currentScaleFactor) &&
           calibration.currentScaleFactor > 0.0F;
}

bool INA219Monitor::setCalibration(const Calibration& calibration)
{
    if (!sensorConfigured_ || !isValidCalibration(calibration)) {
        return false;
    }
    calibration_ = calibration;
    return persistCalibration();
}

INA219Monitor::Calibration INA219Monitor::getCalibration() const
{
    return calibration_;
}

bool INA219Monitor::loadPersistedCalibration()
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    Calibration restored{};
    const bool ok = readFloat(handle, NVS_KEY_VOLTAGE_OFFSET, restored.voltageOffsetVolts) &&
                    readFloat(handle, NVS_KEY_CURRENT_OFFSET, restored.currentOffsetAmps) &&
                    readFloat(handle, NVS_KEY_CURRENT_SCALE, restored.currentScaleFactor) &&
                    isValidCalibration(restored);
    nvs_close(handle);

    if (ok) {
        calibration_ = restored;
    }
    return ok;
#else
    return false;
#endif
}

bool INA219Monitor::persistCalibration() const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    const bool wrote = writeFloat(handle, NVS_KEY_VOLTAGE_OFFSET, calibration_.voltageOffsetVolts) &&
                       writeFloat(handle, NVS_KEY_CURRENT_OFFSET, calibration_.currentOffsetAmps) &&
                       writeFloat(handle, NVS_KEY_CURRENT_SCALE, calibration_.currentScaleFactor);
    const bool committed = wrote && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return committed;
#else
    return true;
#endif
}

void INA219Monitor::printDiagnosticReport() const
{
#ifdef ESP_PLATFORM
    LoadMeasurements measurements{};
    const bool ok = readMeasurements(measurements);
    ESP_LOGI(TAG, "Sensor: %s source=%s", ok ? "READY" : "NOT READY", toText(lastMeasurementSource_));
    if (ok) {
        ESP_LOGI(TAG, "Battery: %.2f V  %.2f A  %.2f W",
                 static_cast<double>(measurements.voltageVolts),
                 static_cast<double>(measurements.currentAmps),
                 static_cast<double>(measurements.powerWatts));
    }
#endif
}

} // namespace kilowatts
