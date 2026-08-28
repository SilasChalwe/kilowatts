/**
 * @file SystemCommandModel.h
 * @brief Canonical request/result types shared by every transport that can
 *        command the Central runtime (serial Console, MQTT — and any future
 *        transport).
 *
 * Console and MQTT are two transports to the SAME Kilowatts command
 * interface, not two separate control systems. Each transport still does
 * its own parsing (text argv vs JSON), but the parsed request, the handler
 * that acts on it, and the result it produces are shared: exactly the types
 * defined here. Neither transport may define its own competing request or
 * result struct for an operation already modeled here.
 *
 * This header has no ESP-IDF/MQTT-client dependency (unlike MqttManager.h,
 * which pulls in "mqtt_client.h") so the Console library can depend on it
 * without pulling network-client code into a UI-only translation unit.
 */
#ifndef KILOWATTS_SYSTEM_COMMAND_MODEL_H
#define KILOWATTS_SYSTEM_COMMAND_MODEL_H

#include "Load.h"

#include <cstdint>

namespace kilowatts {

/**
 * Shared outcome of any command handled through this model. Console
 * renders it as human-readable text; MQTT serializes it into a JSON
 * acknowledgement — both describe the SAME accepted/completed/reason
 * result for the SAME underlying operation.
 */
struct CommandResult {
    bool accepted;
    bool completed;
    char reason[128];
};

/**
 * Changes priority/mode/schedule on a Load that already exists (Central-
 * local or Smart Node). Unlike CONFIGURE_LOAD below, this never touches
 * relay hardware/registration — it only updates the stored configuration
 * LoadConfigurationStore already owns and re-applies it to the live Load.
 */
struct LoadCommandRequest {
    std::uint32_t commandId;
    Load::MacAddress nodeMacAddress;
    std::uint8_t relayPin;

    bool hasPriority;
    std::uint16_t priority;

    bool hasMode;
    LoadMode::Value mode;

    bool hasSchedule;
    AutoSchedule schedule;
};

enum class SystemCommandAction : std::uint8_t {
    UNKNOWN = 0U,
    REQUEST_OPTIMIZATION_CYCLE = 1U,
    FACTORY_RESET_CENTRAL = 2U,
    FACTORY_RESET_NODE = 3U,
    SET_OPTIMIZER_INTERVAL = 4U,
    REPORT_OPTIMIZER_INTERVAL = 5U,
    /** Reboots Central only; never touches NVS, unlike FACTORY_RESET_CENTRAL. */
    REBOOT_CENTRAL = 6U
};

struct SystemCommandRequest {
    std::uint32_t commandId;
    SystemCommandAction action;
    char confirmText[32];
    Load::MacAddress targetNodeMacAddress;
    bool hasTargetNodeMacAddress;

    bool hasOptimizerIntervalSeconds;
    std::uint32_t optimizerIntervalSeconds;
};

/**
 * Simulation stays what it has always been: an alternative INPUT to the
 * SAME PowerManager battery measurement path (see
 * PowerManager::enableSimulation()/setSimulatedMeasurements()) — not a
 * second planner or a second PowerManager. Console and MQTT both parse
 * into this one canonical request and both drive it through the exact
 * same PowerManager calls.
 */
enum class SimulationCommandAction : std::uint8_t {
    UNKNOWN = 0U,
    ENABLE = 1U,
    DISABLE = 2U,
    SET_VALUES = 3U
};

struct SimulationCommandRequest {
    std::uint32_t commandId;
    SimulationCommandAction action;

    /** Only meaningful when action == SET_VALUES. */
    bool hasElectricalMeasurements;
    float batteryVoltageVolts;
    float batteryCurrentAmps;

    bool hasStateOfChargePercent;
    float stateOfChargePercent;
};

enum class ConfigCommandAction : std::uint8_t {
    UNKNOWN = 0U,
    COMMISSION_NODE = 1U,
    RENAME_NODE = 2U,
    DECOMMISSION_NODE = 3U,
    CONFIGURE_LOAD = 4U,
    CONFIGURE_BATTERY_SENSOR = 5U,
    REMOVE_LOAD = 6U,
    CONFIGURE_POWER_LIMITS = 7U
};

struct ConfigCommandRequest {
    std::uint32_t commandId;
    ConfigCommandAction action;
    Load::MacAddress nodeMacAddress;

    bool hasFriendlyName;
    char friendlyName[20];

    /**
     * Load configuration/registration fields — the current Load model
     * only: no nominal voltage/current, no startup watts. Matches
     * ConfigureLoadCommandPacket exactly (see HardwareConfigurationPackets.h).
     */
    bool hasLoadConfiguration;
    char loadName[16];
    std::uint8_t relayPin;
    bool relayActiveHigh;
    LoadMode::Value mode;
    std::uint16_t priority;
    LoadPowerType powerType;
    float powerRatingWatts;
    AutoSchedule schedule;

    bool hasRelayPin;

    bool hasBatterySensorConfiguration;
    float batteryShuntResistanceOhms;
    float batteryMaximumExpectedCurrentAmps;
    float batteryEmaAlpha;
    float batteryCapacityAmpHours;
    float batteryInitialStateOfChargePercent;
    float batteryNominalVoltageVolts;

    /**
     * Matches CentralConfigurationStore::PowerLimitsConfiguration exactly:
     * the immediate electrical safety limits plus the user's required-
     * runtime target (0 = no runtime target configured).
     */
    bool hasPowerLimitsConfiguration;
    float minimumStateOfChargePercent;
    float maximumBatteryDischargeCurrentAmps;
    float maximumMainCurrentAmps;
    float requiredRuntimeHours;
};

} // namespace kilowatts

#endif // KILOWATTS_SYSTEM_COMMAND_MODEL_H
