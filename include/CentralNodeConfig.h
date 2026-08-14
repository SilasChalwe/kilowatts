/**
 * @file CentralNodeConfig.h
 * @brief Genuine hardware/policy configuration for the Kilowatts Central
 *        Node — no installation-specific local Loads/Branches.
 *
 * Compile-time configuration only (constants and factory functions), read
 * once at startup by src/central/main.cpp — mirrors SmartNodeConfig.h's
 * role for the Smart Node side. Fields marked "PENDING HARDWARE
 * VERIFICATION" are placeholder values that let the firmware compile and
 * run without every physical value re-confirmed against the actual
 * installation yet. Secrets (Wi-Fi password, MQTT credentials) are never
 * placed here — see KilowattsSecrets.h.
 *
 * This header intentionally does NOT define any local Load/Branch, and
 * does NOT define a battery sensor address — Central boots with zero
 * local Loads and zero configured sensors until commissioned, exactly
 * like a Smart Node (see SmartNodeConfig.h's own file comment). Simulated
 * sensor input is a runtime-only concern now (see lib/DevelopmentSession,
 * lib/INA219Monitor::setDevelopmentOverride()), never a compile-time
 * value substituted automatically at boot.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#ifndef KILOWATTS_CENTRAL_NODE_CONFIG_H
#define KILOWATTS_CENTRAL_NODE_CONFIG_H

#include "BestFirstSearch.h"
#include "INA219Monitor.h"
#include "PowerBudgetCalculator.h"

#include <cstdint>

namespace kilowatts {
namespace CentralNodeConfig {


constexpr const char* CENTRAL_NODE_NAME = "Central";

/** DHCP hostname reported to the household/installation Access Point (cosmetic, see WiFiManager::Credentials::hostname). */
constexpr const char* WIFI_STATION_HOSTNAME = "kilowatts-central";


/*
 * -----------------------------------------------------------------------
 * I2C bus (PENDING HARDWARE VERIFICATION)
 * -----------------------------------------------------------------------
 * Only consulted in production mode.
 */
constexpr std::uint8_t I2C_SERIAL_DATA_PIN = 21U;
constexpr std::uint8_t I2C_SERIAL_CLOCK_PIN = 22U;
constexpr std::uint32_t I2C_CLOCK_SPEED_HZ = 400000U;
constexpr std::uint8_t I2C_PORT_NUMBER = 0U;

inline INA219Monitor::I2CBusConfiguration i2cBusConfiguration()
{
    return INA219Monitor::I2CBusConfiguration{
        I2C_SERIAL_DATA_PIN, I2C_SERIAL_CLOCK_PIN, I2C_CLOCK_SPEED_HZ, I2C_PORT_NUMBER
    };
}


/*
 * -----------------------------------------------------------------------
 * Central's battery-bus INA219 (Section 4.5.1: monitors the whole battery
 * bus and is the final design's only INA219) is deliberately NOT
 * configured here. Section "Remove Automatic Development Battery Input":
 * a clean/uncommissioned Central must never automatically register an
 * installation-specific battery sensor address — battery sensor identity
 * (I2C address, shunt resistance, expected current) becomes real
 * commissioning configuration held by CentralConfigurationStore and applied
 * only after an installer command verifies that the real INA219 responds.
 * Until that succeeds, Central genuinely has no battery sensor:
 * BatteryStateOfCharge is never initialize()d, sensorAcquisitionTask() has
 * nothing to read, and every battery/SoC field the system publishes honestly
 * reports NOT_CONFIGURED/UNKNOWN rather than a fabricated reading (see
 * SystemStateJson's batterySensorConfigured/stateOfChargeValid fields).
 * -----------------------------------------------------------------------
 */


/*
 * -----------------------------------------------------------------------
 * Battery/electrical policy (Sections 4.6.2.2-4.6.2.5). Real installation
 * values (battery chemistry/capacity, safe current limits) are PENDING
 * HARDWARE VERIFICATION; the numbers below are conservative placeholders
 * sized for a small 12V lead-acid/LiFePO4 demonstration bank.
 * -----------------------------------------------------------------------
 */
constexpr float NOMINAL_BATTERY_VOLTAGE_VOLTS = 12.0F;
constexpr float BATTERY_CAPACITY_AMP_HOURS = 100.0F;
constexpr float DEFAULT_STATE_OF_CHARGE_PERCENT = 80.0F;   // used only before any SoC has ever been persisted
constexpr float MINIMUM_STATE_OF_CHARGE_PERCENT = 20.0F;   // SoC_min
constexpr float WARNING_STATE_OF_CHARGE_PERCENT = 40.0F;   // SoC_warn
constexpr float TARGET_RUNTIME_HOURS = 4.0F;                // T_target
constexpr float MAXIMUM_BATTERY_DISCHARGE_CURRENT_AMPS = 40.0F; // I_B,max
constexpr float MAXIMUM_MAIN_CURRENT_AMPS = 30.0F;          // I_main,max
constexpr float SAFETY_FACTOR = 0.9F;                        // rho

inline PowerBudgetCalculator::Inputs powerBudgetInputs(float stateOfChargePercent, float batteryBusVoltageVolts)
{
    PowerBudgetCalculator::Inputs inputs{};
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
 * -----------------------------------------------------------------------
 * Best-First Search policy weights (Section 4.6.3, Equations 4.30/4.32).
 * Equal unit weighting across w_P/w_S/w_B/w_Q by default so no single
 * term dominates the demonstration ranking; w_T slightly favours letting
 * an already-due schedule compete on its own priority. W_max=10 comfortably
 * exceeds every configured Load priority (max 9, Central Status Indicator).
 * -----------------------------------------------------------------------
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


/*
 * -----------------------------------------------------------------------
 * Optimisation cadence (Section 4.6.1: "Optimisation Task ... nominal
 * period: 5 seconds").
 * -----------------------------------------------------------------------
 */
constexpr std::uint32_t OPTIMIZATION_PERIOD_MILLISECONDS = 5000U;
constexpr std::uint32_t SENSOR_ACQUISITION_PERIOD_MILLISECONDS = 1000U;
constexpr std::uint32_t WATCHDOG_PERIOD_MILLISECONDS = 60000U;
constexpr std::uint32_t RELAY_COMMAND_ACK_TIMEOUT_MILLISECONDS = 3000U;

/*
 * The ONE Node-report staleness timeout used everywhere a Smart Node's
 * online/offline status matters (Section "Exclude Stale/Offline Smart
 * Nodes From Planning") — TopologyTree's "online" field and the
 * Optimisation Task's candidate-exclusion check both use exactly this
 * constant, so there is never more than one definition of "how long
 * before a Node is considered offline" in this project. Smart Nodes send
 * a NODE_REPORT roughly every 2 seconds (see NODE_REPORT_PERIOD_MS in
 * src/smart/main.cpp); 5x that cadence tolerates a couple of missed/lost
 * reports before declaring a Node offline, while still being well inside
 * one Optimisation Task period.
 */
constexpr std::uint32_t NODE_REPORT_TIMEOUT_MILLISECONDS = 10000U;

/*
 * How long a previously-valid battery telemetry reading may still be
 * treated as usable (for Fixed-load critical-protection planning only —
 * see src/central/main.cpp) after the INA219 stops returning successful
 * readings, before it is treated as genuinely invalid rather than
 * merely stale (Section "Fix Sensor Failure Behavior": "must never be
 * treated indefinitely as fresh"). 5x the 1s Sensor Acquisition Task
 * period tolerates a couple of transient read failures.
 */
constexpr std::uint32_t BATTERY_TELEMETRY_STALE_TIMEOUT_MILLISECONDS = 5000U;


/*
 * -----------------------------------------------------------------------
 * MQTT (broker connectivity/credentials live in KilowattsSecrets.h, never
 * here). MQTT_DEVICE_ID identifies this installation inside the shared
 * "kilowatts/v1" namespace (see MqttManager).
 * -----------------------------------------------------------------------
 */
/*
 * The build may override this with a quoted PlatformIO build flag, for
 * example:
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
