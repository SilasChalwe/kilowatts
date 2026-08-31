#ifndef KILOWATTS_MQTT_MANAGER_H
#define KILOWATTS_MQTT_MANAGER_H

#include "Load.h"
#include "SystemCommandModel.h"

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

using LoadCommandHandler = CommandResult (*)(void* context, const LoadCommandRequest& request);
using SystemCommandHandler = CommandResult (*)(void* context, const SystemCommandRequest& request);
using ConfigCommandHandler = CommandResult (*)(void* context, const ConfigCommandRequest& request);
using SimulationCommandHandler = CommandResult (*)(void* context, const SimulationCommandRequest& request);
using PowerPlanningCommandHandler = CommandResult (*)(void* context, const PowerPlanningCommandRequest& request);

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

    // Only five external topics exist.
    static constexpr const char* TOPIC_STATUS = "status";
    static constexpr const char* TOPIC_STATE = "state";
    static constexpr const char* TOPIC_COMMAND = "command";
    static constexpr const char* TOPIC_ACK = "ack";
    static constexpr const char* TOPIC_ALERT = "alert";

    // Internal state parts combined into TOPIC_STATE.
    static constexpr const char* TOPIC_STATE_SYSTEM = "_state/system";
    static constexpr const char* TOPIC_STATE_TREE = "_state/tree";
    static constexpr const char* TOPIC_STATE_LOADS = "_state/loads";
    static constexpr const char* TOPIC_STATE_NODES = "_state/nodes";
    static constexpr const char* TOPIC_CONFIG_NODES = "_state/config-nodes";

    explicit MqttManager(const char* topicNamespace, const char* deviceId, std::uint32_t schemaVersion);
    ~MqttManager();

    MqttManager(const MqttManager&) = delete;
    MqttManager& operator=(const MqttManager&) = delete;

    void setLoadCommandHandler(LoadCommandHandler handler, void* context);
    void setSystemCommandHandler(SystemCommandHandler handler, void* context);
    void setConfigCommandHandler(ConfigCommandHandler handler, void* context);
    void setSimulationCommandHandler(SimulationCommandHandler handler, void* context);
    void setPowerPlanningCommandHandler(PowerPlanningCommandHandler handler, void* context);

    bool begin(const Credentials& credentials);
    bool isConnected() const;
    MqttConnectionState getState() const;

    bool publish(const char* topicSuffix, const std::string& payload, int qos, bool retain);
    bool publishStatus();

    void publishAcknowledgement(
        std::uint32_t commandId,
        const char* commandType,
        AckStatus status,
        const char* reason,
        const char* target);

    void publishEvent(const char* eventType, const char* target, const char* detail);
    void publishAlert(const char* alertType, const char* severity, const char* detail);
    void printDiagnosticReport() const;

private:
    static void handleMqttEvent(void* handlerArgs, esp_event_base_t base, std::int32_t eventId, void* eventData);

    void onConnected();
    void onDisconnected();
    void onDataReceived(const char* topic, std::size_t topicLength, const char* data, std::size_t dataLength);

    void handleNodeCommandMessage(const char* data, std::size_t dataLength);
    void handleLoadCommandMessage(const char* data, std::size_t dataLength);
    void handleSystemCommandMessage(const char* data, std::size_t dataLength);
    void handleBatteryCommandMessage(const char* data, std::size_t dataLength);
    void handleSensorCommandMessage(const char* data, std::size_t dataLength);

    std::string fullTopic(const char* topicSuffix) const;
    bool publishRaw(const char* topicSuffix, const std::string& payload, int qos, bool retain);
    bool publishCombinedState();

    std::string topicNamespace_;
    std::string deviceId_;
    std::string statusTopic_;
    std::uint32_t schemaVersion_;

    std::string stateSystemJson_;
    std::string stateLoadsJson_;
    std::string stateNodesJson_;

    esp_mqtt_client_handle_t client_;
    std::atomic<MqttConnectionState> state_;

    LoadCommandHandler loadCommandHandler_;
    void* loadCommandHandlerContext_;
    SystemCommandHandler systemCommandHandler_;
    void* systemCommandHandlerContext_;
    ConfigCommandHandler configCommandHandler_;
    void* configCommandHandlerContext_;
    SimulationCommandHandler simulationCommandHandler_;
    void* simulationCommandHandlerContext_;
    PowerPlanningCommandHandler powerPlanningCommandHandler_;
    void* powerPlanningCommandHandlerContext_;
};

} // namespace kilowatts

#endif // KILOWATTS_MQTT_MANAGER_H
