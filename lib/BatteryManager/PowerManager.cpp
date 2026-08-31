#include "PowerManager.h"

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
constexpr std::uint8_t REGISTER_CONFIGURATION = 0x00U;
constexpr std::uint8_t REGISTER_SHUNT_VOLTAGE = 0x01U;
constexpr std::uint8_t REGISTER_BUS_VOLTAGE = 0x02U;
constexpr std::uint16_t CONFIGURATION_REGISTER_VALUE = 0x399FU;
constexpr float BUS_VOLTAGE_LSB_VOLTS = 0.004F;
constexpr float SHUNT_VOLTAGE_LSB_VOLTS = 0.00001F;
constexpr float MAXIMUM_SHUNT_VOLTAGE_VOLTS = 0.320F;
constexpr std::uint16_t BUS_VOLTAGE_OVERFLOW_FLAG = 0x0002U;

#ifdef ESP_PLATFORM
constexpr int I2C_TIMEOUT_MS = 100;
constexpr const char* TAG = "POWER_MANAGER";
constexpr const char* SOC_NVS_NAMESPACE = "kw_battery";
constexpr const char* SOC_NVS_KEY = "soc";

bool writeRegister(
    i2c_master_dev_handle_t device,
    std::uint8_t address,
    std::uint16_t value)
{
    const std::uint8_t bytes[3]{
        address,
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(value & 0xFFU)};

    return i2c_master_transmit(
               device,
               bytes,
               sizeof(bytes),
               I2C_TIMEOUT_MS) == ESP_OK;
}

bool readRegister(
    i2c_master_dev_handle_t device,
    std::uint8_t address,
    std::uint16_t& value)
{
    std::uint8_t bytes[2]{};
    if (i2c_master_transmit_receive(
            device,
            &address,
            1U,
            bytes,
            sizeof(bytes),
            I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }

    value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8U) |
        static_cast<std::uint16_t>(bytes[1]));
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
    if (nvs_get_u32(handle, key, &bits) != ESP_OK) return false;

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
        case MeasurementSource::HARDWARE: return "HARDWARE";
        case MeasurementSource::SIMULATED: return "SIMULATED";
    }
    return "UNKNOWN";
}

const char* toText(StateOfChargeSource source)
{
    switch (source) {
        case StateOfChargeSource::UNKNOWN: return "UNKNOWN";
        case StateOfChargeSource::INITIAL: return "INITIAL";
        case StateOfChargeSource::PERSISTED: return "PERSISTED";
        case StateOfChargeSource::COULOMB_COUNTING: return "COULOMB_COUNTING";
        case StateOfChargeSource::VOLTAGE_ESTIMATE: return "VOLTAGE_ESTIMATE";
        case StateOfChargeSource::SIMULATED: return "SIMULATED";
    }
    return "UNKNOWN";
}

PowerManager::PowerManager()
    : busConfiguration_{0U, 0U, 0U, 0U},
      sensorConfiguration_{0.0F, 0.0F, 1.0F},
      batteryConfiguration_{0.0F, 0.0F, 0.0F},
      calibration_{0.0F, 0.0F, 1.0F},
      measurements_{0.0F, 0.0F, 0.0F},
      filteredMeasurements_{0.0F, 0.0F, 0.0F},
      simulatedMeasurements_{0.0F, 0.0F, 0.0F},
      hasFilteredMeasurement_(false),
      measurementSource_(MeasurementSource::NONE),
      stateOfChargePercent_(0.0F),
      stateOfChargeSource_(StateOfChargeSource::UNKNOWN),
      stateOfChargeInitialized_(false),
      powerBudget_{
          0.0F, 0.0F, 0.0F,
          0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
          false, true},
      remainingRequiredRuntimeHours_(0.0F),
      initialized_(false),
      simulationEnabled_(false),
      busHandle_(nullptr),
      deviceHandle_(nullptr)
{
}

PowerManager::~PowerManager()
{
#ifdef ESP_PLATFORM
    if (deviceHandle_ != nullptr) {
        i2c_master_bus_rm_device(
            reinterpret_cast<i2c_master_dev_handle_t>(deviceHandle_));
        deviceHandle_ = nullptr;
    }
    if (busHandle_ != nullptr) {
        i2c_del_master_bus(
            reinterpret_cast<i2c_master_bus_handle_t>(busHandle_));
        busHandle_ = nullptr;
    }
#endif
}

bool PowerManager::isFinitePositive(float value)
{
    return std::isfinite(value) && value > 0.0F;
}

bool PowerManager::isFiniteNonNegative(float value)
{
    return std::isfinite(value) && value >= 0.0F;
}

bool PowerManager::isValidPercent(float value)
{
    return std::isfinite(value) && value >= 0.0F && value <= 100.0F;
}

bool PowerManager::isValidCalibration(const Calibration& calibration)
{
    return std::isfinite(calibration.voltageOffsetVolts) &&
           std::isfinite(calibration.currentOffsetAmps) &&
           std::isfinite(calibration.currentScaleFactor) &&
           calibration.currentScaleFactor != 0.0F;
}

bool PowerManager::initialize(
    const BusConfiguration& busConfiguration,
    const SensorConfiguration& sensorConfiguration,
    const BatteryConfiguration& batteryConfiguration)
{
    if (busConfiguration.clockSpeedHz == 0U ||
        !isValidPercent(batteryConfiguration.minimumStateOfChargePercent)) {
        return false;
    }

    if (!simulationEnabled_) {
        if (!isFinitePositive(sensorConfiguration.shuntResistanceOhms) ||
            !isFinitePositive(sensorConfiguration.maximumExpectedCurrentAmps) ||
            !std::isfinite(sensorConfiguration.emaAlpha) ||
            sensorConfiguration.emaAlpha <= 0.0F ||
            sensorConfiguration.emaAlpha > 1.0F ||
            !isFinitePositive(batteryConfiguration.nameplateVoltageVolts) ||
            !isFinitePositive(batteryConfiguration.capacityAmpHours)) {
            return false;
        }

        const float maximumExpectedShuntVoltage =
            sensorConfiguration.shuntResistanceOhms *
            sensorConfiguration.maximumExpectedCurrentAmps;
        if (maximumExpectedShuntVoltage > MAXIMUM_SHUNT_VOLTAGE_VOLTS) {
            return false;
        }
    } else {
        const bool batteryMetadataProvided =
            batteryConfiguration.nameplateVoltageVolts != 0.0F ||
            batteryConfiguration.capacityAmpHours != 0.0F;
        if (batteryMetadataProvided &&
            (!isFinitePositive(batteryConfiguration.nameplateVoltageVolts) ||
             !isFinitePositive(batteryConfiguration.capacityAmpHours))) {
            return false;
        }
    }

    busConfiguration_ = busConfiguration;
    sensorConfiguration_ = sensorConfiguration;
    batteryConfiguration_ = batteryConfiguration;
    measurements_ = {0.0F, 0.0F, 0.0F};
    filteredMeasurements_ = {0.0F, 0.0F, 0.0F};
    hasFilteredMeasurement_ = false;
    measurementSource_ = MeasurementSource::NONE;

    if (simulationEnabled_) {
        initialized_ = true;
        return true;
    }

    if (!initializeBus() || !initializeSensor()) return false;

    initialized_ = true;
    return true;
}

bool PowerManager::isInitialized() const
{
    return initialized_;
}

bool PowerManager::initializeBus()
{
#ifdef ESP_PLATFORM
    if (busHandle_ != nullptr) return true;

    i2c_master_bus_config_t bus{};
    bus.i2c_port = static_cast<i2c_port_num_t>(busConfiguration_.port);
    bus.sda_io_num = static_cast<gpio_num_t>(busConfiguration_.sdaPin);
    bus.scl_io_num = static_cast<gpio_num_t>(busConfiguration_.sclPin);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7U;
    bus.flags.enable_internal_pullup = 1U;

    i2c_master_bus_handle_t handle = nullptr;
    const esp_err_t result = i2c_new_master_bus(&bus, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(result));
        return false;
    }

    busHandle_ = handle;
    return true;
#else
    return false;
#endif
}

bool PowerManager::initializeSensor()
{
#ifdef ESP_PLATFORM
    if (busHandle_ == nullptr) return false;

    if (deviceHandle_ == nullptr) {
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = INA219_I2C_ADDRESS;
        config.scl_speed_hz = busConfiguration_.clockSpeedHz;

        i2c_master_dev_handle_t device = nullptr;
        if (i2c_master_bus_add_device(
                reinterpret_cast<i2c_master_bus_handle_t>(busHandle_),
                &config,
                &device) != ESP_OK) {
            return false;
        }
        deviceHandle_ = device;
    }

    if (!isHardwareSensorPresent()) return false;

    return writeRegister(
        reinterpret_cast<i2c_master_dev_handle_t>(deviceHandle_),
        REGISTER_CONFIGURATION,
        CONFIGURATION_REGISTER_VALUE);
#else
    return false;
#endif
}

bool PowerManager::isHardwareSensorPresent() const
{
#ifdef ESP_PLATFORM
    return busHandle_ != nullptr &&
           i2c_master_probe(
               reinterpret_cast<i2c_master_bus_handle_t>(busHandle_),
               INA219_I2C_ADDRESS,
               I2C_TIMEOUT_MS) == ESP_OK;
#else
    return false;
#endif
}

bool PowerManager::readHardwareMeasurements(
    PowerMeasurements& measurements) const
{
#ifdef ESP_PLATFORM
    if (deviceHandle_ == nullptr || !isHardwareSensorPresent()) return false;

    const auto device =
        reinterpret_cast<i2c_master_dev_handle_t>(deviceHandle_);
    std::uint16_t busRaw = 0U;
    std::uint16_t shuntRaw = 0U;

    if (!readRegister(device, REGISTER_BUS_VOLTAGE, busRaw) ||
        !readRegister(device, REGISTER_SHUNT_VOLTAGE, shuntRaw) ||
        (busRaw & BUS_VOLTAGE_OVERFLOW_FLAG) != 0U) {
        return false;
    }

    const float voltage =
        static_cast<float>(busRaw >> 3U) * BUS_VOLTAGE_LSB_VOLTS;
    const float shuntVoltage =
        static_cast<float>(static_cast<std::int16_t>(shuntRaw)) *
        SHUNT_VOLTAGE_LSB_VOLTS;
    const float current = shuntVoltage / sensorConfiguration_.shuntResistanceOhms;
    const float power = voltage * current;

    if (!std::isfinite(voltage) ||
        !std::isfinite(current) ||
        !std::isfinite(power)) {
        return false;
    }

    measurements = {voltage, current, power};
    return true;
#else
    (void)measurements;
    return false;
#endif
}

bool PowerManager::enableSimulation(bool enabled)
{
    simulationEnabled_ = enabled;
    hasFilteredMeasurement_ = false;
    measurementSource_ = MeasurementSource::NONE;
    measurements_ = {0.0F, 0.0F, 0.0F};
    filteredMeasurements_ = {0.0F, 0.0F, 0.0F};

    // Simulation is an input source, not a second system. It is immediately
    // ready to accept V/I values. Returning to INA219 requires hardware
    // initialization through the existing applyBatteryConfiguration path.
    initialized_ = enabled;
    return true;
}

bool PowerManager::isSimulationEnabled() const
{
    return simulationEnabled_;
}

bool PowerManager::setSimulatedMeasurements(float voltage, float current)
{
    if (!simulationEnabled_ ||
        !std::isfinite(voltage) ||
        voltage <= 0.0F ||
        !std::isfinite(current)) {
        return false;
    }

    simulatedMeasurements_ = {voltage, current, voltage * current};
    return std::isfinite(simulatedMeasurements_.powerWatts);
}

bool PowerManager::setSimulatedStateOfChargePercent(float stateOfChargePercent)
{
    if (!simulationEnabled_ || !isValidPercent(stateOfChargePercent)) return false;

    stateOfChargePercent_ = stateOfChargePercent;
    stateOfChargeSource_ = StateOfChargeSource::SIMULATED;
    stateOfChargeInitialized_ = true;
    return true;
}

bool PowerManager::readSimulatedMeasurements(PowerMeasurements& measurements) const
{
    if (!std::isfinite(simulatedMeasurements_.voltageVolts) ||
        simulatedMeasurements_.voltageVolts <= 0.0F ||
        !std::isfinite(simulatedMeasurements_.currentAmps) ||
        !std::isfinite(simulatedMeasurements_.powerWatts)) {
        return false;
    }

    measurements = simulatedMeasurements_;
    return true;
}

bool PowerManager::setCalibration(const Calibration& calibration)
{
    if (!isValidCalibration(calibration)) return false;
    calibration_ = calibration;
    hasFilteredMeasurement_ = false;
    return true;
}

PowerManager::Calibration PowerManager::getCalibration() const
{
    return calibration_;
}

PowerMeasurements PowerManager::applyCalibration(
    const PowerMeasurements& measurements,
    const Calibration& calibration)
{
    PowerMeasurements result{};
    result.voltageVolts = measurements.voltageVolts + calibration.voltageOffsetVolts;
    result.currentAmps =
        (measurements.currentAmps + calibration.currentOffsetAmps) *
        calibration.currentScaleFactor;
    result.powerWatts = result.voltageVolts * result.currentAmps;
    return result;
}

PowerMeasurements PowerManager::applyExponentialMovingAverage(
    const PowerMeasurements& previous,
    const PowerMeasurements& current,
    float alpha)
{
    const float a = std::max(0.0F, std::min(alpha, 1.0F));
    const float b = 1.0F - a;

    PowerMeasurements result{};
    result.voltageVolts = a * current.voltageVolts + b * previous.voltageVolts;
    result.currentAmps = a * current.currentAmps + b * previous.currentAmps;
    result.powerWatts = result.voltageVolts * result.currentAmps;
    return result;
}

bool PowerManager::updateMeasurements()
{
    if (!initialized_) return false;

    PowerMeasurements raw{};
    if (simulationEnabled_) {
        if (!readSimulatedMeasurements(raw)) return false;
        measurementSource_ = MeasurementSource::SIMULATED;
    } else {
        if (!readHardwareMeasurements(raw)) {
            measurementSource_ = MeasurementSource::NONE;
            return false;
        }
        measurementSource_ = MeasurementSource::HARDWARE;
    }

    const PowerMeasurements calibrated = applyCalibration(raw, calibration_);
    if (!std::isfinite(calibrated.voltageVolts) ||
        !std::isfinite(calibrated.currentAmps) ||
        !std::isfinite(calibrated.powerWatts)) {
        return false;
    }

    filteredMeasurements_ = hasFilteredMeasurement_
        ? applyExponentialMovingAverage(
              filteredMeasurements_, calibrated, sensorConfiguration_.emaAlpha)
        : calibrated;

    hasFilteredMeasurement_ = true;
    measurements_ = filteredMeasurements_;
    powerBudget_.batteryVoltageVolts = measurements_.voltageVolts;
    powerBudget_.batteryCurrentAmps = measurements_.currentAmps;
    powerBudget_.P_measured = measurements_.powerWatts;
    return true;
}

PowerMeasurements PowerManager::getMeasurements() const
{
    return measurements_;
}

MeasurementSource PowerManager::getMeasurementSource() const
{
    return measurementSource_;
}

float PowerManager::clampStateOfCharge(float value)
{
    return std::max(0.0F, std::min(value, 100.0F));
}

bool PowerManager::initializeStateOfCharge(
    float starting,
    bool restorePersistedState)
{
    if (!isValidPercent(starting)) return false;

    float persisted = 0.0F;
    if (restorePersistedState && loadPersistedStateOfCharge(persisted)) {
        stateOfChargePercent_ = persisted;
        stateOfChargeSource_ = StateOfChargeSource::PERSISTED;
    } else {
        stateOfChargePercent_ = starting;
        stateOfChargeSource_ = StateOfChargeSource::INITIAL;
    }

    stateOfChargeInitialized_ = true;
    return true;
}

bool PowerManager::updateStateOfCharge(float deltaTimeSeconds)
{
    if (!stateOfChargeInitialized_ ||
        !isFinitePositive(deltaTimeSeconds) ||
        !std::isfinite(measurements_.currentAmps) ||
        !isFinitePositive(batteryConfiguration_.capacityAmpHours)) {
        return false;
    }

    const float change =
        measurements_.currentAmps * deltaTimeSeconds * 100.0F /
        (3600.0F * batteryConfiguration_.capacityAmpHours);

    stateOfChargePercent_ = clampStateOfCharge(stateOfChargePercent_ - change);
    stateOfChargeSource_ = StateOfChargeSource::COULOMB_COUNTING;
    return true;
}

bool PowerManager::applyVoltageStateOfChargeEstimate(
    float emptyV,
    float fullV,
    float weight)
{
    if (!stateOfChargeInitialized_ ||
        !isFinitePositive(emptyV) ||
        !isFinitePositive(fullV) ||
        fullV <= emptyV ||
        !std::isfinite(weight) ||
        weight < 0.0F || weight > 1.0F ||
        !std::isfinite(measurements_.voltageVolts)) {
        return false;
    }

    const float voltageSoc = clampStateOfCharge(
        100.0F * (measurements_.voltageVolts - emptyV) / (fullV - emptyV));

    stateOfChargePercent_ = clampStateOfCharge(
        (1.0F - weight) * stateOfChargePercent_ + weight * voltageSoc);
    stateOfChargeSource_ = StateOfChargeSource::VOLTAGE_ESTIMATE;
    return true;
}

float PowerManager::getStateOfChargePercent() const
{
    return stateOfChargePercent_;
}

StateOfChargeSource PowerManager::getStateOfChargeSource() const
{
    return stateOfChargeSource_;
}

bool PowerManager::isStateOfChargeValid() const
{
    return stateOfChargeInitialized_ &&
           stateOfChargeSource_ != StateOfChargeSource::UNKNOWN &&
           isValidPercent(stateOfChargePercent_);
}

bool PowerManager::loadPersistedStateOfCharge(float& stateOfChargePercent) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(SOC_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    const bool ok = readFloat(handle, SOC_NVS_KEY, stateOfChargePercent);
    nvs_close(handle);
    return ok && isValidPercent(stateOfChargePercent);
#else
    (void)stateOfChargePercent;
    return false;
#endif
}

bool PowerManager::persistStateOfChargeValue(float stateOfChargePercent) const
{
#ifdef ESP_PLATFORM
    if (!isValidPercent(stateOfChargePercent)) return false;

    nvs_handle_t handle = 0;
    if (nvs_open(SOC_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;

    const bool ok =
        writeFloat(handle, SOC_NVS_KEY, stateOfChargePercent) &&
        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
#else
    (void)stateOfChargePercent;
    return false;
#endif
}

bool PowerManager::persistStateOfCharge() const
{
    return isStateOfChargeValid() &&
           persistStateOfChargeValue(stateOfChargePercent_);
}

bool PowerManager::setPowerBudgetWatts(float P_budget)
{
    if (!isFinitePositive(P_budget)) return false;
    powerBudget_.P_budget = P_budget;
    return true;
}

bool PowerManager::setPowerReserveWatts(float P_reserve)
{
    if (!isFiniteNonNegative(P_reserve)) return false;
    powerBudget_.P_reserve = P_reserve;
    return true;
}

bool PowerManager::setFixedPowerWatts(float P_fixed)
{
    if (!isFiniteNonNegative(P_fixed)) return false;
    powerBudget_.P_fixed = P_fixed;
    return true;
}

bool PowerManager::setAutoPowerWatts(float P_auto)
{
    if (!isFiniteNonNegative(P_auto)) return false;
    powerBudget_.P_auto = P_auto;
    return true;
}

bool PowerManager::setRemainingRequiredRuntimeHours(float hours)
{
    if (!isFiniteNonNegative(hours)) return false;
    remainingRequiredRuntimeHours_ = hours;
    return true;
}

float PowerManager::getRemainingRequiredRuntimeHours() const
{
    return remainingRequiredRuntimeHours_;
}

bool PowerManager::updatePowerBudget()
{
    const float P_budget = powerBudget_.P_budget;
    const float P_reserve = powerBudget_.P_reserve;
    const float P_fixed = powerBudget_.P_fixed;
    const float P_auto = powerBudget_.P_auto;
    const float minimumStateOfChargePercent =
        batteryConfiguration_.minimumStateOfChargePercent;

    if (!isFinitePositive(P_budget) ||
        !isFiniteNonNegative(P_reserve) ||
        !isFiniteNonNegative(P_fixed) ||
        !isFiniteNonNegative(P_auto) ||
        P_reserve > P_budget ||
        !isValidPercent(minimumStateOfChargePercent)) {
        return false;
    }

    const bool stateOfChargeRequired =
        minimumStateOfChargePercent > 0.0F ||
        remainingRequiredRuntimeHours_ > 0.0F;
    if (stateOfChargeRequired && !isStateOfChargeValid()) return false;

    const float powerAfterReserveWatts =
        std::max(0.0F, P_budget - P_reserve);
    float planningAllowanceWatts = powerAfterReserveWatts;

    const bool runtimeBudgetActive = remainingRequiredRuntimeHours_ > 0.0F;
    bool requiredRuntimeAchievable = true;

    if (runtimeBudgetActive) {
        if (!isFinitePositive(batteryConfiguration_.capacityAmpHours) ||
            !isFinitePositive(batteryConfiguration_.nameplateVoltageVolts)) {
            return false;
        }

        const float usableSocFraction = std::max(
            0.0F,
            (stateOfChargePercent_ - minimumStateOfChargePercent) / 100.0F);
        const float usableEnergyWh =
            batteryConfiguration_.capacityAmpHours *
            batteryConfiguration_.nameplateVoltageVolts *
            usableSocFraction;
        const float runtimeAllowanceWatts =
            usableEnergyWh / remainingRequiredRuntimeHours_;

        if (!std::isfinite(runtimeAllowanceWatts)) return false;

        planningAllowanceWatts =
            std::min(powerAfterReserveWatts, runtimeAllowanceWatts);
        requiredRuntimeAchievable = P_fixed <= planningAllowanceWatts;
    }

    if (isStateOfChargeValid() &&
        stateOfChargePercent_ <= minimumStateOfChargePercent) {
        planningAllowanceWatts = 0.0F;
        if (runtimeBudgetActive && P_fixed > 0.0F) {
            requiredRuntimeAchievable = false;
        }
    }

    powerBudget_.P_auto_available =
        std::max(0.0F, planningAllowanceWatts - P_fixed);
    powerBudget_.P_remaining =
        std::max(0.0F, P_budget - (P_fixed + P_auto));
    powerBudget_.runtimeBudgetActive = runtimeBudgetActive;
    powerBudget_.requiredRuntimeAchievable = requiredRuntimeAchievable;
    powerBudget_.batteryVoltageVolts = measurements_.voltageVolts;
    powerBudget_.batteryCurrentAmps = measurements_.currentAmps;
    powerBudget_.P_measured = measurements_.powerWatts;
    return true;
}

PowerBudget PowerManager::getPowerBudget() const
{
    return powerBudget_;
}

float PowerManager::getAutoAvailablePowerWatts() const
{
    return powerBudget_.P_auto_available;
}

float PowerManager::getRemainingPowerWatts() const
{
    return powerBudget_.P_remaining;
}

void PowerManager::printDiagnosticReport() const
{
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Measurement source: %s", toText(measurementSource_));
    ESP_LOGI(
        TAG,
        "Measured: %.2f V | %.3f A | %.2f W",
        static_cast<double>(measurements_.voltageVolts),
        static_cast<double>(measurements_.currentAmps),
        static_cast<double>(measurements_.powerWatts));
    ESP_LOGI(
        TAG,
        "P_budget=%.2f P_reserve=%.2f",
        static_cast<double>(powerBudget_.P_budget),
        static_cast<double>(powerBudget_.P_reserve));
    ESP_LOGI(
        TAG,
        "P_fixed=%.2f P_auto_available=%.2f P_auto=%.2f P_remaining=%.2f",
        static_cast<double>(powerBudget_.P_fixed),
        static_cast<double>(powerBudget_.P_auto_available),
        static_cast<double>(powerBudget_.P_auto),
        static_cast<double>(powerBudget_.P_remaining));
    if (powerBudget_.runtimeBudgetActive) {
        ESP_LOGI(
            TAG,
            "Runtime remaining=%.2f h achievable=%s",
            static_cast<double>(remainingRequiredRuntimeHours_),
            powerBudget_.requiredRuntimeAchievable ? "YES" : "NO");
    }
#endif
}

} // namespace kilowatts
