/**
 * @file MqttManager.h
 * @brief Declares Central Node MQTT connectivity and the kilowatts/v1
 *        topic contract.
 *
 * MqttManager's one responsibility: own the MQTT broker connection
 * (connect/reconnect/publish/subscribe) and translate the kilowatts/v1
 * wire contract (JSON payloads, topics, QoS, retained flags) to and from
 * plain typed requests the caller (Central's orchestration code in
 * src/central/main.cpp) supplies or handles. It does not calculate
 * battery/power values, does not run Best-First Search, does not
 * actuate relays, does not build the system-state/tree JSON payloads
 * itself (SystemStateJson/TopologyTree do that — MqttManager only
 * publishes whatever payload string it is given), and does not own
 * Wi-Fi connectivity (WiFiManager does).
 *
 * Topics (namespace from CentralNodeConfig::MQTT_TOPIC_NAMESPACE,
 * "kilowatts/v1" by default):
 *
 *   kilowatts/v1/state/system     Central publishes, retained, QoS 1
 *   kilowatts/v1/state/tree       Central publishes, retained, QoS 1
 *   kilowatts/v1/state/loads      Central publishes, retained, QoS 1
 *   kilowatts/v1/state/nodes      Central publishes, retained, QoS 1
 *   kilowatts/v1/config/nodes     Central publishes, retained, QoS 1
 *   kilowatts/v1/events           Central publishes, not retained, QoS 1
 *   kilowatts/v1/commands/load    Application publishes, Central subscribes, QoS 1
 *   kilowatts/v1/commands/system  Application publishes, Central subscribes, QoS 1
 *   kilowatts/v1/commands/config  Application publishes, Central subscribes, QoS 1
 *   kilowatts/v1/commands/development  Application publishes, Central subscribes, QoS 1
 *   kilowatts/v1/acks             Central publishes, not retained, QoS 1
 *
 * Every acknowledgement on kilowatts/v1/acks carries commandId, commandType,
 * status (see AckStatus) and reason, so a client never has to infer outcome
 * from a bare boolean. A commissioning command's initial ACCEPTED/REJECTED
 * ack (this class's own syntactic + the handler's immediate domain
 * validation) is published synchronously from the commands/config message
 * handler; a second APPLIED/FAILED ack for the same commandId follows later
 * once Central's ESP-NOW round trip with the target Node actually completes
 * (see src/central/main.cpp) — publishAcknowledgement() is public precisely
 * so that second, asynchronous ack can be published from outside the
 * initial message-handling call stack.
 *
 * MQTT/Wi-Fi loss must never stop local sensor monitoring, Best-First
 * Search, ESP-NOW, or relay control (Section "MQTT failure must not stop
 * local control") — this class only ever reports connectivity state
 * honestly; it never blocks the caller waiting for a connection.
 *
 * This module requires the real ESP-IDF MQTT client (esp-mqtt,
 * mqtt_client.h) and cJSON for parsing incoming commands, and is
 * therefore ESP32-target-only, like WiFiManager, EspNowCommunication and
 * ChipInfo — it has no host build split and no host-native test.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
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


/**
 * One parsed, validated-at-the-syntax-level (not yet domain-validated)
 * kilowatts/v1/commands/load request. hasPriority/hasMode/hasSchedule
 * let the application change only a subset of a Load's configuration in
 * one command — a field the application did not send is left exactly as
 * it already is by the caller applying this request, not overwritten
 * with a default.
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


/** Result the caller's handler returns for one LoadCommandRequest. */
struct LoadCommandResult {
    bool accepted;
    char reason[80];
};


/**
 * Caller-supplied handler invoked for every syntactically valid
 * kilowatts/v1/commands/load message. context is whatever pointer was
 * passed to setLoadCommandHandler(); MqttManager never interprets it.
 * The handler is responsible for every domain validation the document
 * requires (Node MAC known, relay pin known, Load exists, priority
 * range, schedule hour/minute range, valid mode) and, on acceptance,
 * for updating the domain Load, persisting the change
 * (LoadConfigurationStore), and signalling the Optimisation Task —
 * MqttManager itself does none of that.
 */
using LoadCommandHandler = LoadCommandResult (*)(void* context, const LoadCommandRequest& request);


/**
 * kilowatts/v1/commands/system: requesting an immediate extra planning
 * cycle outside the normal ~5s cadence (Section 4.6.1's Optimisation Task
 * period), or a factory reset (Section "Factory Reset") of Central itself
 * or a remote Node. Unrecognised actions are rejected with a structured
 * acknowledgement rather than silently ignored.
 */
enum class SystemCommandAction : std::uint8_t {
    UNKNOWN = 0U,
    REQUEST_OPTIMIZATION_CYCLE = 1U,
    FACTORY_RESET_CENTRAL = 2U,
    FACTORY_RESET_NODE = 3U,
    APPLY_SAFETY_CONFIG = 4U
};


/**
 * confirmText/targetNodeMacAddress are only meaningful for
 * FACTORY_RESET_CENTRAL/FACTORY_RESET_NODE (Section "Protect This
 * Operation From Accidental Invocation"): confirmText must equal
 * "FACTORY_RESET_CONFIRMED" exactly, checked by the domain handler, not
 * by MqttManager's own syntactic parsing.
 */
struct SystemCommandRequest {
    std::uint32_t commandId;
    SystemCommandAction action;
    char confirmText[32];
    Load::MacAddress targetNodeMacAddress;
    bool hasTargetNodeMacAddress;

    /** Meaningful only for APPLY_SAFETY_CONFIG. */
    bool hasSafetyPolicy;
    float minimumStateOfChargePercent;
    float warningStateOfChargePercent;
    float targetRuntimeHours;
    float safetyFactor;
    float maximumBatteryDischargeCurrentAmps;
    float maximumMainCurrentAmps;
};


using SystemCommandHandler = LoadCommandResult (*)(void* context, const SystemCommandRequest& request);


/**
 * kilowatts/v1/commands/config supports the commissioning-lifecycle
 * operations and runtime Smart-Node load commissioning. A load is never
 * inferred from a relay pin: the complete relay, branch and installer-verified
 * electrical rating facts must be supplied together by the installer portal.
 * The only INA219 belongs to Central's separately commissioned battery bus.
 */
enum class ConfigCommandAction : std::uint8_t {
    UNKNOWN = 0U,
    COMMISSION_NODE = 1U,
    RENAME_NODE = 2U,
    DECOMMISSION_NODE = 3U,
    CONFIGURE_LOAD = 4U,
    CONFIGURE_BATTERY_SENSOR = 5U
};


/**
 * One parsed, syntactically-validated kilowatts/v1/commands/config
 * request. hasFriendlyName is false for DECOMMISSION_NODE, which needs no
 * name.
 */
struct ConfigCommandRequest {
    std::uint32_t commandId;
    ConfigCommandAction action;
    Load::MacAddress nodeMacAddress;

    bool hasFriendlyName;
    char friendlyName[20];   // matches NodeCommissioningRegistry::FRIENDLY_NAME_BUFFER_SIZE / CommissionCommandPacket::friendlyName

    /** Meaningful only for CONFIGURE_LOAD. */
    bool hasLoadConfiguration;
    char loadName[16];
    std::uint8_t relayPin;
    bool relayActiveHigh;
    LoadMode::Value mode;
    std::uint16_t priority;
    float nominalVoltageVolts;
    float nominalCurrentAmps;
    float branchMaximumCurrentAmps;
    float startupWatts;
    AutoSchedule schedule;

    /** Meaningful only for CONFIGURE_BATTERY_SENSOR (target is Central's MAC). */
    bool hasBatterySensorConfiguration;
    std::uint8_t batteryI2cAddress;
    float batteryShuntResistanceOhms;
    float batteryMaximumExpectedCurrentAmps;
    float batteryEmaAlpha;
    float batteryCapacityAmpHours;
    float batteryInitialStateOfChargePercent;
};


/**
 * The handler's own return only reflects Central's immediate, local
 * outcome. CONFIGURE_BATTERY_SENSOR and DECOMMISSION_NODE complete locally
 * once Central has durably applied them; COMMISSION_NODE, RENAME_NODE and
 * CONFIGURE_LOAD publish a later APPLIED/FAILED outcome only after the
 * addressed Smart Node confirms over ESP-NOW.
 */
using ConfigCommandHandler = LoadCommandResult (*)(void* context, const ConfigCommandRequest& request);


/**
 * kilowatts/v1/commands/development (Section "Development Session Is
 * Explicit"): the only way a Node's OperatingEnvironment ever becomes
 * DEVELOPMENT, and the only way a sensor ever gets a simulated
 * voltage/current override — never automatic, never compile-time.
 */
enum class DevelopmentCommandAction : std::uint8_t {
    UNKNOWN = 0U,
    START_SESSION = 1U,
    END_SESSION = 2U,
    SET_SENSOR_INPUT = 3U,
    CLEAR_SENSOR_OVERRIDE = 4U
};


/**
 * targetNodeMacAddress selects which Node this command applies to (Central
 * itself, or a remote Smart Node reached over ESP-NOW — see
 * src/central/main.cpp's handleDevelopmentCommand()). hasSensorInput/
 * i2cAddress/voltageVolts/currentAmps are only meaningful for
 * SET_SENSOR_INPUT/CLEAR_SENSOR_OVERRIDE.
 */
struct DevelopmentCommandRequest {
    std::uint32_t commandId;
    DevelopmentCommandAction action;
    Load::MacAddress targetNodeMacAddress;

    bool hasSensorInput;
    std::uint8_t i2cAddress;
    float voltageVolts;
    float currentAmps;
};


using DevelopmentCommandHandler = LoadCommandResult (*)(void* context, const DevelopmentCommandRequest& request);


/** Machine-readable outcome for one kilowatts/v1/acks entry (Section "Command IDs And Acks"). */
enum class AckStatus : std::uint8_t {
    ACCEPTED = 0U,
    APPLIED = 1U,
    REJECTED = 2U,
    FAILED = 3U
};


class MqttManager {

public:

    /**
     * Broker host and port are kept as two separate fields, never a
     * pre-combined "scheme://host:port" string — begin() itself is what
     * combines them, through esp-mqtt's discrete
     * broker.address.hostname/port/transport configuration fields, so
     * there is never a hand-built URI string to get subtly wrong.
     */
    struct Credentials {
        const char* brokerHost;
        std::uint16_t brokerPort;
        bool brokerUseTls;
        const char* username;
        const char* password;
    };


    /** Well-known topic suffixes, appended to the configured namespace. */
    static constexpr const char* TOPIC_STATE_SYSTEM = "state/system";
    static constexpr const char* TOPIC_STATE_TREE = "state/tree";
    static constexpr const char* TOPIC_STATE_LOADS = "state/loads";
    static constexpr const char* TOPIC_STATE_NODES = "state/nodes";
    static constexpr const char* TOPIC_CONFIG_NODES = "config/nodes";
    static constexpr const char* TOPIC_EVENTS = "events";
    static constexpr const char* TOPIC_COMMANDS_LOAD = "commands/load";
    static constexpr const char* TOPIC_COMMANDS_SYSTEM = "commands/system";
    static constexpr const char* TOPIC_COMMANDS_CONFIG = "commands/config";
    static constexpr const char* TOPIC_COMMANDS_DEVELOPMENT = "commands/development";
    static constexpr const char* TOPIC_ACKS = "acks";


    explicit MqttManager(const char* topicNamespace, const char* deviceId, std::uint32_t schemaVersion);

    ~MqttManager();

    MqttManager(const MqttManager&) = delete;
    MqttManager& operator=(const MqttManager&) = delete;


    /**
     * Registers the handler invoked for every syntactically valid
     * kilowatts/v1/commands/load message. Must be called before begin()
     * connects (or at least before a command could arrive) so no command
     * is ever silently dropped for lack of a handler.
     */
    void setLoadCommandHandler(LoadCommandHandler handler, void* context);

    /** Registers the handler invoked for every syntactically valid kilowatts/v1/commands/system message. */
    void setSystemCommandHandler(SystemCommandHandler handler, void* context);

    /** Registers the handler invoked for every syntactically valid kilowatts/v1/commands/config message. */
    void setConfigCommandHandler(ConfigCommandHandler handler, void* context);

    /** Registers the handler invoked for every syntactically valid kilowatts/v1/commands/development message. */
    void setDevelopmentCommandHandler(DevelopmentCommandHandler handler, void* context);


    /**
     * Starts the MQTT client and connects to credentials.brokerHost:brokerPort. The
     * caller is responsible for calling this only once Wi-Fi actually has
     * an IP address (see WiFiManager::isConnected()) — MqttManager does
     * not itself wait for or depend on Wi-Fi state beyond that.
     *
     * Returns false when the underlying esp_mqtt_client could not be
     * created/started; this is independent of whether the broker
     * connection has actually completed yet — see isConnected() for that.
     */
    bool begin(const Credentials& credentials);


    /** Equivalent to getState() == CONNECTED. */
    bool isConnected() const;

    MqttConnectionState getState() const;


    /**
     * Publishes payload to <namespace>/<topicSuffix>. Returns false
     * (payload not queued) when not currently connected — MQTT loss must
     * never block the caller; local control continues regardless of this
     * return value.
     */
    bool publish(const char* topicSuffix, const std::string& payload, int qos, bool retain);


    /**
     * Publishes a structured acknowledgement for commandId to TOPIC_ACKS —
     * see this header's own file comment for why this is public (a
     * commissioning command publishes a second, later ack for the same
     * commandId once its ESP-NOW round trip completes, from outside the
     * commands/config message-handling call stack). commandType is a
     * short caller-supplied label ("LOAD", "SYSTEM", "COMMISSION_NODE",
     * ...); target is published as JSON null when nullptr/empty.
     */
    void publishAcknowledgement(std::uint32_t commandId, const char* commandType, AckStatus status,
                                 const char* reason, const char* target);


    /**
     * Publishes one event object to TOPIC_EVENTS (not retained). target is
     * published as JSON null when nullptr/empty.
     */
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
    void handleDevelopmentCommandMessage(const char* data, std::size_t dataLength);

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

    DevelopmentCommandHandler developmentCommandHandler_;
    void* developmentCommandHandlerContext_;
};


} // namespace kilowatts

#endif // KILOWATTS_MQTT_MANAGER_H
