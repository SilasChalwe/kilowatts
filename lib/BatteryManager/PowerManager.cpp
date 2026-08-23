/**
 * @file PowerManager.cpp
 * @brief Central battery monitoring and power-budget management
 *        implementation for the Kilowatts system.
 *
 * PowerManager reads the central battery-bus INA219, maintains
 * battery State of Charge (SoC), applies battery and main-bus
 * electrical limits, and calculates P_available for automatic loads.
 *
 * Positive battery current represents battery discharge.
 * Negative battery current represents battery charging.
 *
 * PowerManager does not select loads and does not control relays.
 * BestFirstSearch receives only P_available.
 *
 * @author Silas
 */

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


/**
 * INA219 default I2C address.
 */
constexpr std::uint8_t INA219_I2C_ADDRESS = 0x40U;


/**
 * INA219 register addresses.
 */
constexpr std::uint8_t REGISTER_CONFIGURATION = 0x00U;
constexpr std::uint8_t REGISTER_SHUNT_VOLTAGE = 0x01U;
constexpr std::uint8_t REGISTER_BUS_VOLTAGE = 0x02U;


/**
 * Configuration:
 *
 * Bus voltage range: 32 V
 * PGA range: ±320 mV
 * ADC settings suitable for normal power monitoring.
 */
constexpr std::uint16_t CONFIGURATION_REGISTER_VALUE = 0x399FU;


/**
 * INA219 measurement scaling.
 */
constexpr float BUS_VOLTAGE_LSB_VOLTS = 0.004F;
constexpr float SHUNT_VOLTAGE_LSB_VOLTS = 0.00001F;


/**
 * Maximum shunt voltage allowed by the selected INA219
 * configuration.
 */
constexpr float MAXIMUM_SHUNT_VOLTAGE_VOLTS = 0.320F;


/**
 * Bus-voltage register overflow flag.
 */
constexpr std::uint16_t BUS_VOLTAGE_OVERFLOW_FLAG = 0x0002U;


#ifdef ESP_PLATFORM

constexpr int I2C_TIMEOUT_MS = 100;

constexpr const char* TAG = "POWER_MANAGER";


/**
 * NVS storage used for battery State of Charge.
 */
constexpr const char* SOC_NVS_NAMESPACE = "kw_battery";
constexpr const char* SOC_NVS_KEY = "soc";


/**
 * @brief Writes a 16-bit value to an INA219 register.
 */
bool writeRegister(
    i2c_master_dev_handle_t device,
    std::uint8_t registerAddress,
    std::uint16_t value)
{
    const std::uint8_t bytes[3]{
        registerAddress,
        static_cast<std::uint8_t>(
            (value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(
            value & 0xFFU)
    };

    return i2c_master_transmit(
               device,
               bytes,
               sizeof(bytes),
               I2C_TIMEOUT_MS) == ESP_OK;
}


/**
 * @brief Reads a 16-bit value from an INA219 register.
 */
bool readRegister(
    i2c_master_dev_handle_t device,
    std::uint8_t registerAddress,
    std::uint16_t& value)
{
    std::uint8_t bytes[2]{};

    const esp_err_t result =
        i2c_master_transmit_receive(
            device,
            &registerAddress,
            1U,
            bytes,
            sizeof(bytes),
            I2C_TIMEOUT_MS);

    if (result != ESP_OK) {
        return false;
    }

    value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8U) |
        static_cast<std::uint16_t>(bytes[1]));

    return true;
}


/**
 * @brief Writes a float value to NVS as raw 32-bit data.
 */
bool writeFloat(
    nvs_handle_t handle,
    const char* key,
    float value)
{
    std::uint32_t bits = 0U;

    static_assert(
        sizeof(bits) == sizeof(value),
        "float must be 32 bits");

    std::memcpy(
        &bits,
        &value,
        sizeof(value));

    return nvs_set_u32(
               handle,
               key,
               bits) == ESP_OK;
}


/**
 * @brief Reads a float value from NVS.
 */
bool readFloat(
    nvs_handle_t handle,
    const char* key,
    float& value)
{
    std::uint32_t bits = 0U;

    if (nvs_get_u32(
            handle,
            key,
            &bits) != ESP_OK) {
        return false;
    }

    static_assert(
        sizeof(bits) == sizeof(value),
        "float must be 32 bits");

    std::memcpy(
        &value,
        &bits,
        sizeof(value));

    return std::isfinite(value);
}

#endif


} // namespace


// -----------------------------------------------------------------------------
// Text conversion
// -----------------------------------------------------------------------------


const char* toText(MeasurementSource source)
{
    switch (source) {

        case MeasurementSource::NONE:
            return "NONE";

        case MeasurementSource::HARDWARE:
            return "HARDWARE";

        case MeasurementSource::SIMULATED:
            return "SIMULATED";
    }

    return "UNKNOWN";
}


const char* toText(StateOfChargeSource source)
{
    switch (source) {

        case StateOfChargeSource::UNKNOWN:
            return "UNKNOWN";

        case StateOfChargeSource::INITIAL:
            return "INITIAL";

        case StateOfChargeSource::PERSISTED:
            return "PERSISTED";

        case StateOfChargeSource::COULOMB_COUNTING:
            return "COULOMB_COUNTING";

        case StateOfChargeSource::VOLTAGE_ESTIMATE:
            return "VOLTAGE_ESTIMATE";

        case StateOfChargeSource::SIMULATED:
            return "SIMULATED";
    }

    return "UNKNOWN";
}


// -----------------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------------


PowerManager::PowerManager()
    : busConfiguration_{0U, 0U, 0U, 0U},
      sensorConfiguration_{0.0F, 0.0F, 1.0F},
      batteryConfiguration_{0.0F, 0.0F, 0.0F, 0.0F},
      mainBusConfiguration_{0.0F},
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
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          false,
          0.0F,
          true},
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
            reinterpret_cast<i2c_master_dev_handle_t>(
                deviceHandle_));

        deviceHandle_ = nullptr;
    }


    if (busHandle_ != nullptr) {

        i2c_del_master_bus(
            reinterpret_cast<i2c_master_bus_handle_t>(
                busHandle_));

        busHandle_ = nullptr;
    }

#endif
}


// -----------------------------------------------------------------------------
// Validation
// -----------------------------------------------------------------------------


bool PowerManager::isFinitePositive(float value)
{
    return std::isfinite(value) &&
           value > 0.0F;
}


bool PowerManager::isValidPercent(float value)
{
    return std::isfinite(value) &&
           value >= 0.0F &&
           value <= 100.0F;
}


bool PowerManager::isValidCalibration(
    const Calibration& calibration)
{
    return
        std::isfinite(
            calibration.voltageOffsetVolts) &&

        std::isfinite(
            calibration.currentOffsetAmps) &&

        std::isfinite(
            calibration.currentScaleFactor) &&

        calibration.currentScaleFactor != 0.0F;
}


// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------


bool PowerManager::initialize(
    const BusConfiguration& busConfiguration,
    const SensorConfiguration& sensorConfiguration,
    const BatteryConfiguration& batteryConfiguration,
    const MainBusConfiguration& mainBusConfiguration)
{
    if (busConfiguration.clockSpeedHz == 0U) {
        return false;
    }


    if (!isFinitePositive(
            sensorConfiguration.shuntResistanceOhms) ||

        !isFinitePositive(
            sensorConfiguration.maximumExpectedCurrentAmps) ||

        !std::isfinite(
            sensorConfiguration.emaAlpha) ||

        sensorConfiguration.emaAlpha <= 0.0F ||

        sensorConfiguration.emaAlpha > 1.0F) {

        return false;
    }


    /**
     * Ensure the expected shunt voltage remains inside the
     * INA219 ±320 mV range.
     */
    const float maximumExpectedShuntVoltage =
        sensorConfiguration.shuntResistanceOhms *
        sensorConfiguration.maximumExpectedCurrentAmps;


    if (maximumExpectedShuntVoltage >
        MAXIMUM_SHUNT_VOLTAGE_VOLTS) {

        return false;
    }


    if (!isFinitePositive(
            batteryConfiguration.nameplateVoltageVolts) ||

        !isFinitePositive(
            batteryConfiguration.capacityAmpHours) ||

        !isValidPercent(
            batteryConfiguration.minimumStateOfChargePercent) ||

        !isFinitePositive(
            batteryConfiguration.maximumDischargeCurrentAmps)) {

        return false;
    }


    if (!isFinitePositive(
            mainBusConfiguration.maximumCurrentAmps)) {

        return false;
    }


    busConfiguration_ = busConfiguration;
    sensorConfiguration_ = sensorConfiguration;
    batteryConfiguration_ = batteryConfiguration;
    mainBusConfiguration_ = mainBusConfiguration;


    measurements_ =
        PowerMeasurements{
            0.0F,
            0.0F,
            0.0F};


    filteredMeasurements_ =
        PowerMeasurements{
            0.0F,
            0.0F,
            0.0F};


    hasFilteredMeasurement_ = false;
    measurementSource_ =
        MeasurementSource::NONE;


    /**
     * In simulation mode physical I2C hardware is not required.
     */
    if (simulationEnabled_) {

        if (simulatedMeasurements_.voltageVolts <= 0.0F) {

            simulatedMeasurements_.voltageVolts =
                batteryConfiguration_.nameplateVoltageVolts;

            simulatedMeasurements_.currentAmps = 0.0F;
            simulatedMeasurements_.powerWatts = 0.0F;
        }

        initialized_ = true;
        return true;
    }


    if (!initializeBus()) {
        return false;
    }


    if (!initializeSensor()) {
        return false;
    }


    initialized_ = true;

    return true;
}


bool PowerManager::isInitialized() const
{
    return initialized_;
}


// -----------------------------------------------------------------------------
// I2C bus
// -----------------------------------------------------------------------------


bool PowerManager::initializeBus()
{
#ifdef ESP_PLATFORM

    if (busHandle_ != nullptr) {
        return true;
    }


    i2c_master_bus_config_t bus{};

    bus.i2c_port =
        static_cast<i2c_port_num_t>(
            busConfiguration_.port);

    bus.sda_io_num =
        static_cast<gpio_num_t>(
            busConfiguration_.sdaPin);

    bus.scl_io_num =
        static_cast<gpio_num_t>(
            busConfiguration_.sclPin);

    bus.clk_source =
        I2C_CLK_SRC_DEFAULT;

    bus.glitch_ignore_cnt = 7U;

    bus.flags.enable_internal_pullup = 1U;


    i2c_master_bus_handle_t handle = nullptr;


    const esp_err_t result =
        i2c_new_master_bus(
            &bus,
            &handle);


    if (result != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to initialize I2C bus: %s",
            esp_err_to_name(result));

        return false;
    }


    busHandle_ = handle;

    return true;

#else

    return false;

#endif
}


// -----------------------------------------------------------------------------
// INA219 initialization
// -----------------------------------------------------------------------------


bool PowerManager::initializeSensor()
{
#ifdef ESP_PLATFORM

    if (busHandle_ == nullptr) {
        return false;
    }


    if (deviceHandle_ == nullptr) {

        i2c_device_config_t deviceConfiguration{};

        deviceConfiguration.dev_addr_length =
            I2C_ADDR_BIT_LEN_7;

        deviceConfiguration.device_address =
            INA219_I2C_ADDRESS;

        deviceConfiguration.scl_speed_hz =
            busConfiguration_.clockSpeedHz;


        i2c_master_dev_handle_t device = nullptr;


        const esp_err_t result =
            i2c_master_bus_add_device(
                reinterpret_cast<
                    i2c_master_bus_handle_t>(
                        busHandle_),
                &deviceConfiguration,
                &device);


        if (result != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to add INA219 device: %s",
                esp_err_to_name(result));

            return false;
        }


        deviceHandle_ = device;
    }


    if (!isHardwareSensorPresent()) {

        ESP_LOGE(
            TAG,
            "INA219 not detected at address 0x%02X",
            INA219_I2C_ADDRESS);

        return false;
    }


    const auto device =
        reinterpret_cast<
            i2c_master_dev_handle_t>(
                deviceHandle_);


    if (!writeRegister(
            device,
            REGISTER_CONFIGURATION,
            CONFIGURATION_REGISTER_VALUE)) {

        ESP_LOGE(
            TAG,
            "Failed to configure INA219.");

        return false;
    }


    ESP_LOGI(
        TAG,
        "INA219 initialized at address 0x%02X",
        INA219_I2C_ADDRESS);


    return true;

#else

    return false;

#endif
}


// -----------------------------------------------------------------------------
// Hardware sensor presence
// -----------------------------------------------------------------------------


bool PowerManager::isHardwareSensorPresent() const
{
#ifdef ESP_PLATFORM

    if (busHandle_ == nullptr) {
        return false;
    }


    return
        i2c_master_probe(
            reinterpret_cast<
                i2c_master_bus_handle_t>(
                    busHandle_),
            INA219_I2C_ADDRESS,
            I2C_TIMEOUT_MS) == ESP_OK;

#else

    return false;

#endif
}


// -----------------------------------------------------------------------------
// Raw hardware measurement
// -----------------------------------------------------------------------------


bool PowerManager::readHardwareMeasurements(
    PowerMeasurements& measurements) const
{
#ifdef ESP_PLATFORM

    if (deviceHandle_ == nullptr) {
        return false;
    }


    if (!isHardwareSensorPresent()) {
        return false;
    }


    const auto device =
        reinterpret_cast<
            i2c_master_dev_handle_t>(
                deviceHandle_);


    std::uint16_t busVoltageRaw = 0U;
    std::uint16_t shuntVoltageRaw = 0U;


    if (!readRegister(
            device,
            REGISTER_BUS_VOLTAGE,
            busVoltageRaw)) {

        return false;
    }


    if (!readRegister(
            device,
            REGISTER_SHUNT_VOLTAGE,
            shuntVoltageRaw)) {

        return false;
    }


    if ((busVoltageRaw &
         BUS_VOLTAGE_OVERFLOW_FLAG) != 0U) {

        ESP_LOGE(
            TAG,
            "INA219 math overflow detected.");

        return false;
    }


    const float busVoltageVolts =
        static_cast<float>(
            busVoltageRaw >> 3U) *
        BUS_VOLTAGE_LSB_VOLTS;


    const auto signedShuntRaw =
        static_cast<std::int16_t>(
            shuntVoltageRaw);


    const float shuntVoltageVolts =
        static_cast<float>(
            signedShuntRaw) *
        SHUNT_VOLTAGE_LSB_VOLTS;


    const float currentAmps =
        shuntVoltageVolts /
        sensorConfiguration_.shuntResistanceOhms;


    const float powerWatts =
        busVoltageVolts *
        currentAmps;


    if (!std::isfinite(busVoltageVolts) ||
        !std::isfinite(currentAmps) ||
        !std::isfinite(powerWatts)) {

        return false;
    }


    measurements =
        PowerMeasurements{
            busVoltageVolts,
            currentAmps,
            powerWatts};


    return true;

#else

    (void)measurements;

    return false;

#endif
}


// -----------------------------------------------------------------------------
// Simulation
// -----------------------------------------------------------------------------


bool PowerManager::enableSimulation(bool enabled)
{
    simulationEnabled_ = enabled;

    hasFilteredMeasurement_ = false;
    measurementSource_ =
        MeasurementSource::NONE;

    return true;
}


bool PowerManager::isSimulationEnabled() const
{
    return simulationEnabled_;
}


bool PowerManager::setSimulatedMeasurements(
    float batteryVoltageVolts,
    float batteryCurrentAmps)
{
    if (!std::isfinite(batteryVoltageVolts) ||
        batteryVoltageVolts <= 0.0F ||

        !std::isfinite(batteryCurrentAmps)) {

        return false;
    }


    simulatedMeasurements_.voltageVolts =
        batteryVoltageVolts;

    simulatedMeasurements_.currentAmps =
        batteryCurrentAmps;

    simulatedMeasurements_.powerWatts =
        batteryVoltageVolts *
        batteryCurrentAmps;


    return true;
}


bool PowerManager::setSimulatedStateOfChargePercent(
    float stateOfChargePercent)
{
    if (!simulationEnabled_ ||
        !initialized_ ||
        !isValidPercent(stateOfChargePercent)) {

        return false;
    }

    stateOfChargePercent_ = stateOfChargePercent;
    stateOfChargeSource_ = StateOfChargeSource::SIMULATED;
    stateOfChargeInitialized_ = true;

    return true;
}


bool PowerManager::readSimulatedMeasurements(
    PowerMeasurements& measurements) const
{
    if (!std::isfinite(
            simulatedMeasurements_.voltageVolts) ||

        simulatedMeasurements_.voltageVolts <= 0.0F ||

        !std::isfinite(
            simulatedMeasurements_.currentAmps) ||

        !std::isfinite(
            simulatedMeasurements_.powerWatts)) {

        return false;
    }


    measurements =
        simulatedMeasurements_;


    return true;
}


// -----------------------------------------------------------------------------
// Calibration
// -----------------------------------------------------------------------------


bool PowerManager::setCalibration(
    const Calibration& calibration)
{
    if (!isValidCalibration(calibration)) {
        return false;
    }


    calibration_ = calibration;

    hasFilteredMeasurement_ = false;

    return true;
}


PowerManager::Calibration
PowerManager::getCalibration() const
{
    return calibration_;
}


PowerMeasurements
PowerManager::applyCalibration(
    const PowerMeasurements& measurements,
    const Calibration& calibration)
{
    PowerMeasurements corrected{};


    corrected.voltageVolts =
        measurements.voltageVolts +
        calibration.voltageOffsetVolts;


    corrected.currentAmps =
        (measurements.currentAmps +
         calibration.currentOffsetAmps) *
        calibration.currentScaleFactor;


    corrected.powerWatts =
        corrected.voltageVolts *
        corrected.currentAmps;


    return corrected;
}


// -----------------------------------------------------------------------------
// EMA filtering
// -----------------------------------------------------------------------------


PowerMeasurements
PowerManager::applyExponentialMovingAverage(
    const PowerMeasurements& previous,
    const PowerMeasurements& current,
    float alpha)
{
    const float a =
        std::max(
            0.0F,
            std::min(alpha, 1.0F));


    const float b =
        1.0F - a;


    PowerMeasurements result{};


    result.voltageVolts =
        (a * current.voltageVolts) +
        (b * previous.voltageVolts);


    result.currentAmps =
        (a * current.currentAmps) +
        (b * previous.currentAmps);


    /**
     * Recalculate power from the filtered voltage/current
     * instead of filtering power independently.
     */
    result.powerWatts =
        result.voltageVolts *
        result.currentAmps;


    return result;
}


// -----------------------------------------------------------------------------
// Measurement update
// -----------------------------------------------------------------------------


bool PowerManager::updateMeasurements()
{
    if (!initialized_) {
        return false;
    }


    PowerMeasurements raw{};


    if (simulationEnabled_) {

        if (!readSimulatedMeasurements(raw)) {
            return false;
        }

        measurementSource_ =
            MeasurementSource::SIMULATED;

    } else {

        if (!readHardwareMeasurements(raw)) {

            measurementSource_ =
                MeasurementSource::NONE;

            return false;
        }

        measurementSource_ =
            MeasurementSource::HARDWARE;
    }


    const PowerMeasurements calibrated =
        applyCalibration(
            raw,
            calibration_);


    if (!std::isfinite(
            calibrated.voltageVolts) ||

        !std::isfinite(
            calibrated.currentAmps) ||

        !std::isfinite(
            calibrated.powerWatts)) {

        return false;
    }


    if (hasFilteredMeasurement_) {

        filteredMeasurements_ =
            applyExponentialMovingAverage(
                filteredMeasurements_,
                calibrated,
                sensorConfiguration_.emaAlpha);

    } else {

        filteredMeasurements_ =
            calibrated;

        hasFilteredMeasurement_ = true;
    }


    measurements_ =
        filteredMeasurements_;


    /**
     * Keep measurement information inside the power-budget
     * structure synchronized.
     */
    powerBudget_.batteryVoltageVolts =
        measurements_.voltageVolts;

    powerBudget_.batteryCurrentAmps =
        measurements_.currentAmps;

    powerBudget_.batteryPowerWatts =
        measurements_.powerWatts;


    return true;
}


PowerMeasurements
PowerManager::getMeasurements() const
{
    return measurements_;
}


MeasurementSource
PowerManager::getMeasurementSource() const
{
    return measurementSource_;
}


// -----------------------------------------------------------------------------
// State of Charge
// -----------------------------------------------------------------------------


float PowerManager::clampStateOfCharge(
    float value)
{
    return std::max(
        0.0F,
        std::min(value, 100.0F));
}


bool PowerManager::initializeStateOfCharge(
    float startingStateOfChargePercent,
    bool restorePersistedState)
{
    if (!initialized_ ||
        !isValidPercent(
            startingStateOfChargePercent)) {

        return false;
    }


    float persistedStateOfCharge = 0.0F;


    if (restorePersistedState &&
        loadPersistedStateOfCharge(
            persistedStateOfCharge)) {

        stateOfChargePercent_ =
            persistedStateOfCharge;

        stateOfChargeSource_ =
            StateOfChargeSource::PERSISTED;

    } else {

        stateOfChargePercent_ =
            startingStateOfChargePercent;

        stateOfChargeSource_ =
            StateOfChargeSource::INITIAL;
    }


    stateOfChargeInitialized_ = true;

    return true;
}


bool PowerManager::updateStateOfCharge(
    float deltaTimeSeconds)
{
    if (!stateOfChargeInitialized_ ||

        !std::isfinite(deltaTimeSeconds) ||

        deltaTimeSeconds <= 0.0F ||

        !std::isfinite(
            measurements_.currentAmps)) {

        return false;
    }


    /**
     * Positive battery current means discharge.
     *
     * Ah_used =
     *     I * timeSeconds / 3600
     *
     * percentUsed =
     *     Ah_used / batteryCapacityAh * 100
     *
     * Negative current therefore increases SoC,
     * representing charging.
     */
    const float stateOfChargeChangePercent =
        (
            measurements_.currentAmps *
            deltaTimeSeconds *
            100.0F
        ) /
        (
            3600.0F *
            batteryConfiguration_.capacityAmpHours
        );


    stateOfChargePercent_ =
        clampStateOfCharge(
            stateOfChargePercent_ -
            stateOfChargeChangePercent);


    stateOfChargeSource_ =
        StateOfChargeSource::COULOMB_COUNTING;


    return true;
}


bool PowerManager::applyVoltageStateOfChargeEstimate(
    float emptyVoltageVolts,
    float fullVoltageVolts,
    float weight)
{
    if (!stateOfChargeInitialized_ ||

        !isFinitePositive(
            emptyVoltageVolts) ||

        !isFinitePositive(
            fullVoltageVolts) ||

        fullVoltageVolts <=
            emptyVoltageVolts ||

        !std::isfinite(weight) ||

        weight < 0.0F ||

        weight > 1.0F ||

        !std::isfinite(
            measurements_.voltageVolts)) {

        return false;
    }


    const float voltageStateOfCharge =
        clampStateOfCharge(
            100.0F *
            (
                measurements_.voltageVolts -
                emptyVoltageVolts
            ) /
            (
                fullVoltageVolts -
                emptyVoltageVolts
            ));


    stateOfChargePercent_ =
        clampStateOfCharge(
            ((1.0F - weight) *
             stateOfChargePercent_) +

            (weight *
             voltageStateOfCharge));


    stateOfChargeSource_ =
        StateOfChargeSource::VOLTAGE_ESTIMATE;


    return true;
}


float PowerManager::getStateOfChargePercent() const
{
    return stateOfChargePercent_;
}


StateOfChargeSource
PowerManager::getStateOfChargeSource() const
{
    return stateOfChargeSource_;
}


bool PowerManager::isStateOfChargeValid() const
{
    return
        stateOfChargeInitialized_ &&

        stateOfChargeSource_ !=
            StateOfChargeSource::UNKNOWN &&

        isValidPercent(
            stateOfChargePercent_);
}


// -----------------------------------------------------------------------------
// SoC persistence
// -----------------------------------------------------------------------------


bool PowerManager::loadPersistedStateOfCharge(
    float& stateOfChargePercent) const
{
#ifdef ESP_PLATFORM

    nvs_handle_t handle = 0;


    if (nvs_open(
            SOC_NVS_NAMESPACE,
            NVS_READONLY,
            &handle) != ESP_OK) {

        return false;
    }


    const bool result =
        readFloat(
            handle,
            SOC_NVS_KEY,
            stateOfChargePercent);


    nvs_close(handle);


    return
        result &&
        isValidPercent(
            stateOfChargePercent);

#else

    (void)stateOfChargePercent;

    return false;

#endif
}


bool PowerManager::persistStateOfChargeValue(
    float stateOfChargePercent) const
{
#ifdef ESP_PLATFORM

    if (!isValidPercent(
            stateOfChargePercent)) {

        return false;
    }


    nvs_handle_t handle = 0;


    if (nvs_open(
            SOC_NVS_NAMESPACE,
            NVS_READWRITE,
            &handle) != ESP_OK) {

        return false;
    }


    const bool wrote =
        writeFloat(
            handle,
            SOC_NVS_KEY,
            stateOfChargePercent);


    const bool committed =
        wrote &&
        nvs_commit(handle) == ESP_OK;


    nvs_close(handle);


    return committed;

#else

    (void)stateOfChargePercent;

    return false;

#endif
}


bool PowerManager::persistStateOfCharge() const
{
    if (!isStateOfChargeValid()) {
        return false;
    }


    return persistStateOfChargeValue(
        stateOfChargePercent_);
}


// -----------------------------------------------------------------------------
// Committed power
// -----------------------------------------------------------------------------


bool PowerManager::setCommittedPowerWatts(
    float committedPowerWatts)
{
    if (!std::isfinite(
            committedPowerWatts) ||

        committedPowerWatts < 0.0F) {

        return false;
    }


    powerBudget_.committedPowerWatts =
        committedPowerWatts;


    return true;
}


bool PowerManager::setRemainingRequiredRuntimeHours(
    float remainingRequiredRuntimeHours)
{
    if (!std::isfinite(remainingRequiredRuntimeHours) ||
        remainingRequiredRuntimeHours < 0.0F) {

        return false;
    }

    remainingRequiredRuntimeHours_ =
        remainingRequiredRuntimeHours;

    return true;
}


float PowerManager::getRemainingRequiredRuntimeHours() const
{
    return remainingRequiredRuntimeHours_;
}


// -----------------------------------------------------------------------------
// Power budget
// -----------------------------------------------------------------------------


bool PowerManager::updatePowerBudget()
{
    if (!initialized_) {
        return false;
    }


    const float V_B =
        measurements_.voltageVolts;


    if (!isFinitePositive(V_B)) {
        return false;
    }


    /**
     * Maximum power permitted by the battery.
     *
     * P_battery_max =
     *     V_B * I_battery_max
     */
    const float P_battery_max =
        V_B *
        batteryConfiguration_
            .maximumDischargeCurrentAmps;


    /**
     * Main distribution limit.
     *
     * P_main_max =
     *     V_B * I_main_max
     */
    const float P_main_max =
        V_B *
        mainBusConfiguration_
            .maximumCurrentAmps;


    if (!std::isfinite(P_battery_max) ||
        !std::isfinite(P_main_max)) {

        return false;
    }


    /**
     * The central system cannot safely allocate more
     * power than the most restrictive electrical limit.
     */
    const float safePowerLimitWatts =
        std::min(
            P_battery_max,
            P_main_max);


    const float P_committed =
        powerBudget_
            .committedPowerWatts;


    /**
     * P_remaining =
     *     P_limit - P_committed
     *
     * Never expose negative available power.
     */
    const float P_remaining =
        std::max(
            0.0F,
            safePowerLimitWatts -
            P_committed);


    float P_available =
        P_remaining;


    /**
     * Runtime sustainability.
     *
     * When the user has configured a required-runtime target, the usable
     * battery energy above the reserve/minimum SoC additionally bounds
     * P_available — the stricter of the immediate electrical limit
     * (P_remaining, above) and this sustainability limit always wins.
     */
    const bool runtimeBudgetActive =
        remainingRequiredRuntimeHours_ > 0.0F;

    float sustainableTotalPowerWatts = 0.0F;
    bool requiredRuntimeAchievable = true;

    if (runtimeBudgetActive) {

        const float usableStateOfChargeFraction =
            std::max(
                0.0F,
                (stateOfChargePercent_ -
                 batteryConfiguration_
                     .minimumStateOfChargePercent) /
                100.0F);

        const float fullEnergyWattHours =
            batteryConfiguration_.capacityAmpHours *
            batteryConfiguration_.nameplateVoltageVolts;

        const float usableEnergyWattHours =
            fullEnergyWattHours *
            usableStateOfChargeFraction;

        sustainableTotalPowerWatts =
            usableEnergyWattHours /
            remainingRequiredRuntimeHours_;

        requiredRuntimeAchievable =
            P_committed <=
            sustainableTotalPowerWatts;

        const float sustainableAvailable =
            std::max(
                0.0F,
                sustainableTotalPowerWatts -
                P_committed);

        P_available =
            std::min(
                P_available,
                sustainableAvailable);
    }


    /**
     * Battery protection.
     *
     * Automatic loads are not allocated additional
     * power once the minimum allowed SoC is reached.
     *
     * Fixed ON loads remain represented by
     * P_committed and can be handled separately by
     * higher-level safety policy if required.
     */
    if (!isStateOfChargeValid() ||

        stateOfChargePercent_ <=
            batteryConfiguration_
                .minimumStateOfChargePercent) {

        P_available = 0.0F;
    }


    powerBudget_.batteryVoltageVolts =
        V_B;


    powerBudget_.batteryCurrentAmps =
        measurements_.currentAmps;


    powerBudget_.batteryPowerWatts =
        measurements_.powerWatts;


    powerBudget_.batteryMaximumPowerWatts =
        P_battery_max;


    powerBudget_.mainMaximumPowerWatts =
        P_main_max;


    powerBudget_.remainingPowerWatts =
        P_remaining;


    powerBudget_.availablePowerWatts =
        P_available;


    powerBudget_.runtimeBudgetActive =
        runtimeBudgetActive;


    powerBudget_.sustainableTotalPowerWatts =
        sustainableTotalPowerWatts;


    powerBudget_.requiredRuntimeAchievable =
        requiredRuntimeAchievable;


    return true;
}


PowerBudget
PowerManager::getPowerBudget() const
{
    return powerBudget_;
}


float PowerManager::getAvailablePowerWatts() const
{
    return powerBudget_
        .availablePowerWatts;
}


float PowerManager::getRemainingPowerWatts() const
{
    return powerBudget_
        .remainingPowerWatts;
}


float PowerManager::getCommittedPowerWatts() const
{
    return powerBudget_
        .committedPowerWatts;
}


float PowerManager::getBatteryMaximumPowerWatts() const
{
    return powerBudget_
        .batteryMaximumPowerWatts;
}


float PowerManager::getMainMaximumPowerWatts() const
{
    return powerBudget_
        .mainMaximumPowerWatts;
}


// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------


void PowerManager::printDiagnosticReport() const
{
#ifdef ESP_PLATFORM

    ESP_LOGI(
        TAG,
        "Measurement source: %s",
        toText(measurementSource_));


    ESP_LOGI(
        TAG,
        "Battery: %.2f V | %.3f A | %.2f W",
        static_cast<double>(
            measurements_.voltageVolts),
        static_cast<double>(
            measurements_.currentAmps),
        static_cast<double>(
            measurements_.powerWatts));


    if (isStateOfChargeValid()) {

        ESP_LOGI(
            TAG,
            "SoC: %.2f%% | Source: %s",
            static_cast<double>(
                stateOfChargePercent_),
            toText(
                stateOfChargeSource_));

    } else {

        ESP_LOGW(
            TAG,
            "Battery SoC is not initialized.");
    }


    ESP_LOGI(
        TAG,
        "P_battery_max: %.2f W",
        static_cast<double>(
            powerBudget_
                .batteryMaximumPowerWatts));


    ESP_LOGI(
        TAG,
        "P_main_max: %.2f W",
        static_cast<double>(
            powerBudget_
                .mainMaximumPowerWatts));


    ESP_LOGI(
        TAG,
        "P_committed: %.2f W",
        static_cast<double>(
            powerBudget_
                .committedPowerWatts));


    ESP_LOGI(
        TAG,
        "P_remaining: %.2f W",
        static_cast<double>(
            powerBudget_
                .remainingPowerWatts));


    ESP_LOGI(
        TAG,
        "P_available: %.2f W",
        static_cast<double>(
            powerBudget_
                .availablePowerWatts));


    if (powerBudget_.runtimeBudgetActive) {

        ESP_LOGI(
            TAG,
            "Required runtime: %.2f h remaining | sustainable: %.2f W | achievable: %s",
            static_cast<double>(
                remainingRequiredRuntimeHours_),
            static_cast<double>(
                powerBudget_
                    .sustainableTotalPowerWatts),
            powerBudget_.requiredRuntimeAchievable
                ? "YES" : "NO");
    }

#endif
}


} // namespace kilowatts