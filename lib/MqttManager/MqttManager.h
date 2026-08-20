/**
 * @file MqttManager.h
 * @brief MQTT connection and dashboard command contract for Central.
 */
#ifndef KILOWATTS_MQTT_MANAGER_H
#define KILOWATTS_MQTT_MANAGER_H

#include "Load.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "mqtt_client.h"

namespace kilowatts {

enum class MqttConnectionState : std::uint8_t {
    DISCONNECTED = 0U,
    CONNECTING = 1U,
    CONNECTED = 2U
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

struct LoadCommandResult {
    bool accepted;
    /** true when the requested change is already finished locally. */
    bool completed;
    char reason[80];
};

using LoadCommandHandler = LoadCommandResult (*)(void* context, const LoadCommandRequest& request);

enum class SystemCommandAction : std::uint8_t {
    UNKNOWN = 0U,
    REQUEST_OPTIMIZATION_CYCLE = 1U,
    FACTORY_RESET_CENTRAL = 2U,
    FACTORY_RESET_NODE = 3U,
    APPLY_SAFETY_CONFIG = 4U
};

struct SystemCommandRequest {
    std::uint32_t commandId;
    SystemCommandAction action;
    char confirmText[32];
    Load::MacAddress targetNodeMacAddress;
    bool hasTargetNodeMacAddress;

    bool hasSafetyPolicy;
    float minimumStateOfChargePercent;
    float warningStateOfChargePercent;
    float targetRuntimeHours;
    float safetyFactor;
    float maximumBatteryDischargeCurrentAmps;
    float maximumMainCurrentAmps;
};

using SystemCommandHandler = LoadCommandResult (*)(void* context, const SystemCommandRequest& request);

enum class ConfigCommandAction : std::uint8_t {
    UNKNOWN = 0U,
    COMMISSION_NODE = 1U,
    RENAME_NODE = 2U,
    DECOMMISSION_NODE = 3U,
    CONFIGURE_LOAD = 4U,
    CONFIGURE_BATTERY_SENSOR = 5U,
    REMOVE_LOAD = 6U
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
    float nominalVoltageVolts;
    float nominalCurrentAmps;
    float startupWatts;
    AutoSchedule schedule;

    bool hasRelayPin;

    bool hasBatterySensorConfiguration;
    float batteryShuntResistanceOhms;
    float batteryMaximumExpectedCurrentAmps;
    float batteryEmaAlpha;
    float batteryCapacityAmpHours;
    float batteryInitialStateOfChargePercent;
    float batteryNominalVoltageVolts;
};

using ConfigCommandHandler = LoadCommandResult (*)(void* context, const ConfigCommandRequest& request);

enum class AckStatus : std::uint8_t {
    ACCEPTED = 0U,
    APPLIED = 1U,
    REJECTED = 2U,
    FAILED = 3U
};

class MqttManager {
public:
    struct Credentials {
        const char* brokerHost;
        std::uint16_t brokerPort;
        bool brokerUseTls;
        const char* username;
        const char* password;
    };

    static constexpr const char* TOPIC_STATE_SYSTEM = "state/system";
    static constexpr const char* TOPIC_STATE_TREE = "state/tree";
    static constexpr const char* TOPIC_STATE_LOADS = "state/loads";
    static constexpr const char* TOPIC_STATE_NODES = "state/nodes";
    static constexpr const char* TOPIC_CONFIG_NODES = "config/nodes";
    static constexpr const char* TOPIC_EVENTS = "events";
    static constexpr const char* TOPIC_COMMANDS_LOAD = "commands/load";
    static constexpr const char* TOPIC_COMMANDS_SYSTEM = "commands/system";
    static constexpr const char* TOPIC_COMMANDS_CONFIG = "commands/config";
    static constexpr const char* TOPIC_ACKS = "acks";

    explicit MqttManager(const char* topicNamespace, const char* deviceId, std::uint32_t schemaVersion);
    ~MqttManager();

    MqttManager(const MqttManager&) = delete;
    MqttManager& operator=(const MqttManager&) = delete;

    void setLoadCommandHandler(LoadCommandHandler handler, void* context);
    void setSystemCommandHandler(SystemCommandHandler handler, void* context);
    void setConfigCommandHandler(ConfigCommandHandler handler, void* context);

    bool begin(const Credentials& credentials);
    bool isConnected() const;
    MqttConnectionState getState() const;

    bool publish(const char* topicSuffix, const std::string& payload, int qos, bool retain);

    void publishAcknowledgement(
        std::uint32_t commandId,
        const char* commandType,
        AckStatus status,
        const char* reason,
        const char* target);

    void publishEvent(const char* eventType, const char* target, const char* detail);
    void printDiagnosticReport() const;

private:
    static void handleMqttEvent(void* handlerArgs, esp_event_base_t base, std::int32_t eventId, void* eventData);

    void onConnected();
    void onDisconnected();
    void onDataReceived(const char* topic, std::size_t topicLength, const char* data, std::size_t dataLength);

    void handleLoadCommandMessage(const char* data, std::size_t dataLength);
    void handleSystemCommandMessage(const char* data, std::size_t dataLength);
    void handleConfigCommandMessage(const char* data, std::size_t dataLength);

    std::string fullTopic(const char* topicSuffix) const;

    std::string topicNamespace_;
    std::string deviceId_;
    std::uint32_t schemaVersion_;

    esp_mqtt_client_handle_t client_;
    std::atomic<MqttConnectionState> state_;

    LoadCommandHandler loadCommandHandler_;
    void* loadCommandHandlerContext_;
    SystemCommandHandler systemCommandHandler_;
    void* systemCommandHandlerContext_;
    ConfigCommandHandler configCommandHandler_;
    void* configCommandHandlerContext_;
};

} // namespace kilowatts

#endif // KILOWATTS_MQTT_MANAGER_H
