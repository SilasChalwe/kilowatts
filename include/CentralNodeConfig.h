/**
 * @file CentralNodeConfig.h
 * @brief Hardware/policy configuration for the Central Node — no
 *        installation-specific local Loads/Branches.
 *
 * Compile-time constants and factory functions only, read once at startup
 * by src/central/main.cpp. Secrets (Wi-Fi password, MQTT credentials)
 * live in KilowattsSecrets.h, never here.
 *
 * Deliberately defines no local Load/Branch and no battery sensor address:
 * Central boots with zero local Loads and zero configured sensors until
 * commissioned, like a Smart Node. Simulated sensor input is a runtime-only
 * override (DevelopmentSession.h, INA219Monitor::setDevelopmentOverride()),
 * never a compile-time default applied automatically at boot.
 */

#ifndef KILOWATTS_CENTRAL_NODE_CONFIG_H
#define KILOWATTS_CENTRAL_NODE_CONFIG_H

#include "BestFirstSearch.h"
#include "INA219Monitor.h"
#include "PowerManager.h"

#include <array>
#include <cstdint>

namespace kilowatts {
namespace CentralNodeConfig {


constexpr const char* CENTRAL_NODE_NAME = "Central";

/** DHCP hostname reported to the household/installation Access Point (cosmetic, see WiFiManager::Credentials::hostname). */
constexpr const char* WIFI_STATION_HOSTNAME = "kilowatts-central";


constexpr std::array<std::uint8_t, 8U> VERIFIED_RELAY_GPIO_PINS{4U, 13U, 14U, 16U, 17U, 18U, 19U, 23U};


/* I2C bus - ESP32 standard SDA/SCL pins. Only consulted in production mode. */
constexpr std::uint8_t I2C_SERIAL_DATA_PIN = 21U;
constexpr std::uint8_t I2C_SERIAL_CLOCK_PIN = 22U;
constexpr std::uint32_t I2C_CLOCK_SPEED_HZ = 400000U;
constexpr std::uint8_t I2C_PORT_NUMBER = 0U;

inline INA219Monitor::I2CBusConfiguration i2cBusConfiguration()
{
    return INA219Monitor::I2CBusConfiguration{I2C_SERIAL_DATA_PIN, I2C_SERIAL_CLOCK_PIN, I2C_CLOCK_SPEED_HZ, I2C_PORT_NUMBER};
}


/*
 * Central's battery-bus INA219 is deliberately not configured here: an
 * uncommissioned Central must never automatically pick up an
 * installation-specific sensor address. Battery sensor identity (I2C
 * address, shunt resistance, expected current) becomes commissioning
 * configuration held by CentralConfigurationStore and is applied only
 * after an installer command verifies the real INA219 responds. Until
 * then BatteryStateOfCharge is never initialize()d, sensorAcquisitionTask()
 * has nothing to read, and every battery/SoC field reports
 * NOT_CONFIGURED/UNKNOWN rather than a fabricated reading (see
 * SystemStateJson's batterySensorConfigured/stateOfChargeValid fields).
 */


/*
 * Battery/electrical policy defaults, used only on a never-commissioned
 * device. The installer supplies the real nameplate voltage/capacity for
 * the battery bank via the CONFIGURE_BATTERY_SENSOR MQTT command (see
 * CentralConfigurationStore::BatterySensorConfiguration), after which that
 * value is authoritative (configuredOrDefaultNominalVoltageVolts() in
 * src/central/main.cpp).
 */
constexpr float NOMINAL_BATTERY_VOLTAGE_VOLTS = 12.0F;
constexpr float BATTERY_CAPACITY_AMP_HOURS = 100.0F;
constexpr float DEFAULT_STATE_OF_CHARGE_PERCENT = 80.0F;   // used only before any SoC has ever been persisted
constexpr float MINIMUM_STATE_OF_CHARGE_PERCENT = 20.0F;
constexpr float WARNING_STATE_OF_CHARGE_PERCENT = 40.0F;
constexpr float TARGET_RUNTIME_HOURS = 4.0F;
constexpr float MAXIMUM_BATTERY_DISCHARGE_CURRENT_AMPS = 40.0F;
constexpr float MAXIMUM_MAIN_CURRENT_AMPS = 30.0F;
constexpr float SAFETY_FACTOR = 0.9F;

inline SafePowerLimitCalculator::Inputs safePowerLimitInputs(float stateOfChargePercent, float batteryBusVoltageVolts)
{
    SafePowerLimitCalculator::Inputs inputs{};
    inputs.stateOfChargePercent = stateOfChargePercent;
    inputs.minimumStateOfChargePercent = MINIMUM_STATE_OF_CHARGE_PERCENT;
    inputs.nominalBatteryVoltageVolts = NOMINAL_BATTERY_VOLTAGE_VOLTS;
    inputs.batteryCapacityAmpHours = BATTERY_CAPACITY_AMP_HOURS;
    inputs.targetRuntimeHours = TARGET_RUNTIME_HOURS;
    inputs.batteryBusVoltageVolts = batteryBusVoltageVolts;
    inputs.maximumBatteryDischargeCurrentAmps = MAXIMUM_BATTERY_DISCHARGE_CURRENT_AMPS;
    inputs.maximumMainCurrentAmps = MAXIMUM_MAIN_CURRENT_AMPS;
    inputs.safetyFactor = SAFETY_FACTOR;
    return inputs;
}


/*
 * Equal unit weighting across all terms by default so no single factor
 * dominates the ranking. Maximum allowed priority (10) comfortably
 * exceeds every configured Load's priority (max 9, Central Status
 * Indicator).
 */
inline BestFirstSearch::Weights bestFirstSearchWeights()
{
    BestFirstSearch::Weights weights{};
    weights.runningPowerWeight = 1.0F;
    weights.startupPowerWeight = 1.0F;
    weights.batteryStressWeight = 1.0F;
    weights.priorityWeight = 1.0F;
    weights.scheduleWeight = 1.0F;
    weights.maximumAllowedPriority = 10U;
    return weights;
}


constexpr std::uint32_t OPTIMIZATION_PERIOD_MILLISECONDS = 5000U;
constexpr std::uint32_t SENSOR_ACQUISITION_PERIOD_MILLISECONDS = 1000U;
constexpr std::uint32_t WATCHDOG_PERIOD_MILLISECONDS = 60000U;
constexpr std::uint32_t RELAY_COMMAND_ACK_TIMEOUT_MILLISECONDS = 3000U;

/*
 * The one Node-report staleness timeout used everywhere a Smart Node's
 * online/offline status matters — TopologyTree's "online" field and the
 * Optimisation Task's candidate-exclusion check both use this constant,
 * so there is only one definition of "offline" in this project. Smart
 * Nodes report roughly every 2 seconds (NODE_REPORT_PERIOD_MS in
 * src/smart/main.cpp); 5x that cadence tolerates a couple of missed
 * reports while staying well inside one Optimisation Task period.
 */
constexpr std::uint32_t NODE_REPORT_TIMEOUT_MILLISECONDS = 10000U;

/*
 * How long a previously-valid battery telemetry reading stays usable (for
 * Fixed-load critical-protection planning only — see src/central/main.cpp)
 * after the INA219 stops returning successful readings, before it is
 * treated as invalid rather than merely stale. 5x the 1s Sensor
 * Acquisition Task period tolerates a couple of transient read failures.
 */
constexpr std::uint32_t BATTERY_TELEMETRY_STALE_TIMEOUT_MILLISECONDS = 5000U;


/*
 * MQTT broker connectivity/credentials live in KilowattsSecrets.h, never
 * here. MQTT_DEVICE_ID identifies this installation inside the shared
 * "kilowatts/v1" namespace (see MqttManager).
 *
 * The build may override the namespace with a quoted PlatformIO build
 * flag, for example:
 *   -DKILOWATTS_MQTT_TOPIC_NAMESPACE=\"kilowatts/v1/home-42\"
 *
 * Keeping a safe default preserves existing Central builds, while a real
 * deployment can give every installation a distinct topic root that is
 * matched by the Flutter portal and enforced by broker ACLs.
 */
#ifndef KILOWATTS_MQTT_TOPIC_NAMESPACE
#define KILOWATTS_MQTT_TOPIC_NAMESPACE "kilowatts/v1"
#endif
constexpr const char* MQTT_TOPIC_NAMESPACE = KILOWATTS_MQTT_TOPIC_NAMESPACE;
constexpr const char* MQTT_DEVICE_ID = "central-01";
/* Version 2 distinguishes installer-rated load estimates from live battery telemetry. */
constexpr std::uint32_t MQTT_SCHEMA_VERSION = 2U;


} // namespace CentralNodeConfig
} // namespace kilowatts

#endif // KILOWATTS_CENTRAL_NODE_CONFIG_H
