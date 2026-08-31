/**
 * @file SystemCommandModel.h
 * @brief Canonical requests shared by the Central console and operational handlers.
 */
#ifndef KILOWATTS_SYSTEM_COMMAND_MODEL_H
#define KILOWATTS_SYSTEM_COMMAND_MODEL_H

#include "Load.h"
#include "MqttCredentialsStore.h"
#include "WiFiCredentialsStore.h"

#include <cstdint>

namespace kilowatts {

struct CommandResult {
    bool accepted;
    bool completed;
    char reason[128];
};

struct BatterySensorCommandRequest {
    float shuntResistanceOhms;
    float maximumExpectedCurrentAmps;
    float emaAlpha;
    float batteryCapacityAmpHours;
    float initialStateOfChargePercent;
    float nominalVoltageVolts;
};

struct PowerPlanningCommandRequest {
    float P_budget;
    float P_reserve;
    float minimumStateOfChargePercent;
    float requiredRuntimeHours;
};

enum class NetworkCommandTarget : std::uint8_t {
    WIFI = 0U,
    MQTT = 1U
};

/** Central-console network setup only. MQTT does not expose these requests. */
struct NetworkCommandRequest {
    NetworkCommandTarget target;
    enum class Action : std::uint8_t {
        STATUS = 0U,
        SET = 1U,
        CLEAR = 2U,
        SETUP = 3U,
        SCAN = 4U,
        SET_CHANNEL = 5U
    } action;

    char ssid[WiFiCredentialsStore::SSID_BUFFER_SIZE];
    char wifiPassword[WiFiCredentialsStore::PASSWORD_BUFFER_SIZE];
    std::uint8_t wifiChannel;

    char mqttHost[MqttCredentialsStore::HOST_BUFFER_SIZE];
    std::uint16_t mqttPort;
    bool mqttUseTls;
    char mqttUsername[MqttCredentialsStore::USERNAME_BUFFER_SIZE];
    char mqttPassword[MqttCredentialsStore::PASSWORD_BUFFER_SIZE];
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

/** Internal canonical configuration request used by Central handlers. */
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
