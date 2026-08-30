/**
 * @file SystemCommandModel.h
 * @brief Canonical requests shared by Console and MQTT.
 */
#ifndef KILOWATTS_SYSTEM_COMMAND_MODEL_H
#define KILOWATTS_SYSTEM_COMMAND_MODEL_H

#include "Load.h"
#include <cstdint>

namespace kilowatts {

struct CommandResult {
    bool accepted;
    bool completed;
    char reason[128];
};

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

enum class SimulationCommandAction : std::uint8_t {
    UNKNOWN = 0U,
    ENABLE = 1U,
    DISABLE = 2U,
    SET_VALUES = 3U
};

/** Simulation changes only the voltage/current input source. */
struct SimulationCommandRequest {
    std::uint32_t commandId;
    SimulationCommandAction action;
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
    CONFIGURE_POWER_PLANNING = 7U
};

struct ConfigCommandRequest {
    std::uint32_t commandId;
    ConfigCommandAction action;
    Load::MacAddress nodeMacAddress;

    bool hasFriendlyName;
    char friendlyName[20];

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

    bool hasPowerPlanningConfiguration;
    float P_budget;
    float P_reserve;
    float minimumStateOfChargePercent;
    float requiredRuntimeHours;
};

} // namespace kilowatts

#endif // KILOWATTS_SYSTEM_COMMAND_MODEL_H
