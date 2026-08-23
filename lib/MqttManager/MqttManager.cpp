/**
 * @file MqttManager.cpp
 * @brief MQTT connection, state publishing and dashboard command parsing.
 */
#include "MqttManager.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace kilowatts {

namespace {

static const char* TAG = "MQTT";

bool parseMac(const char* text, Load::MacAddress& mac)
{
    if (text == nullptr) {
        return false;
    }

    unsigned int bytes[6]{};
    if (std::sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
                    &bytes[0], &bytes[1], &bytes[2],
                    &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }

    for (std::size_t i = 0U; i < 6U; ++i) {
        mac[i] = static_cast<std::uint8_t>(bytes[i]);
    }
    return true;
}

bool readU8(const cJSON* item, std::uint8_t& value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 || item->valuedouble > 255.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<std::uint8_t>(item->valuedouble);
    return true;
}

bool readU16(const cJSON* item, std::uint16_t& value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 || item->valuedouble > 65535.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<std::uint16_t>(item->valuedouble);
    return true;
}

bool readU32(const cJSON* item, std::uint32_t& value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble <= 0.0 || item->valuedouble > 4294967295.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<std::uint32_t>(item->valuedouble);
    return true;
}

bool readFloat(const cJSON* item, float& value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) {
        return false;
    }
    value = static_cast<float>(item->valuedouble);
    return std::isfinite(value);
}

bool parseMode(const char* text, LoadMode::Value& mode)
{
    if (text == nullptr) return false;
    if (std::strcmp(text, "FIXED_OFF") == 0) { mode = LoadMode::Fixed::OFF; return true; }
    if (std::strcmp(text, "FIXED_ON") == 0)  { mode = LoadMode::Fixed::ON; return true; }
    if (std::strcmp(text, "AUTO_OFF") == 0)  { mode = LoadMode::Auto::OFF; return true; }
    if (std::strcmp(text, "AUTO_ON") == 0)   { mode = LoadMode::Auto::ON; return true; }
    return false;
}

bool parseSystemAction(const char* text, SystemCommandAction& action)
{
    if (text == nullptr) return false;
    if (std::strcmp(text, "REQUEST_OPTIMIZATION_CYCLE") == 0) {
        action = SystemCommandAction::REQUEST_OPTIMIZATION_CYCLE; return true;
    }
    if (std::strcmp(text, "FACTORY_RESET_CENTRAL") == 0) {
        action = SystemCommandAction::FACTORY_RESET_CENTRAL; return true;
    }
    if (std::strcmp(text, "FACTORY_RESET_NODE") == 0) {
        action = SystemCommandAction::FACTORY_RESET_NODE; return true;
    }
    if (std::strcmp(text, "SET_OPTIMIZER_INTERVAL") == 0) {
        action = SystemCommandAction::SET_OPTIMIZER_INTERVAL; return true;
    }
    if (std::strcmp(text, "REPORT_OPTIMIZER_INTERVAL") == 0) {
        action = SystemCommandAction::REPORT_OPTIMIZER_INTERVAL; return true;
    }
    return false;
}

bool parseConfigAction(const char* text, ConfigCommandAction& action)
{
    if (text == nullptr) return false;
    if (std::strcmp(text, "COMMISSION_NODE") == 0) {
        action = ConfigCommandAction::COMMISSION_NODE; return true;
    }
    if (std::strcmp(text, "RENAME_NODE") == 0) {
        action = ConfigCommandAction::RENAME_NODE; return true;
    }
    if (std::strcmp(text, "DECOMMISSION_NODE") == 0) {
        action = ConfigCommandAction::DECOMMISSION_NODE; return true;
    }
    if (std::strcmp(text, "CONFIGURE_LOAD") == 0) {
        action = ConfigCommandAction::CONFIGURE_LOAD; return true;
    }
    if (std::strcmp(text, "REMOVE_LOAD") == 0) {
        action = ConfigCommandAction::REMOVE_LOAD; return true;
    }
    if (std::strcmp(text, "CONFIGURE_BATTERY_SENSOR") == 0) {
        action = ConfigCommandAction::CONFIGURE_BATTERY_SENSOR; return true;
    }
    if (std::strcmp(text, "CONFIGURE_POWER_LIMITS") == 0) {
        action = ConfigCommandAction::CONFIGURE_POWER_LIMITS; return true;
    }
    return false;
}

bool parsePowerType(const char* text, LoadPowerType& powerType)
{
    if (text == nullptr) return false;
    if (std::strcmp(text, "AC") == 0) { powerType = LoadPowerType::AC; return true; }
    if (std::strcmp(text, "DC") == 0) { powerType = LoadPowerType::DC; return true; }
    return false;
}

bool parseSimulationAction(const char* text, SimulationCommandAction& action)
{
    if (text == nullptr) return false;
    if (std::strcmp(text, "ENABLE") == 0) { action = SimulationCommandAction::ENABLE; return true; }
    if (std::strcmp(text, "DISABLE") == 0) { action = SimulationCommandAction::DISABLE; return true; }
    if (std::strcmp(text, "SET_VALUES") == 0) { action = SimulationCommandAction::SET_VALUES; return true; }
    return false;
}

void appendJsonString(std::string& out, const char* value)
{
    if (value == nullptr || value[0] == '\0') {
        out += "null";
        return;
    }

    out.push_back('"');
    for (const char* c = value; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') out.push_back('\\');
        out.push_back(*c);
    }
    out.push_back('"');
}

const char* ackText(AckStatus status)
{
    switch (status) {
        case AckStatus::ACCEPTED: return "ACCEPTED";
        case AckStatus::APPLIED: return "APPLIED";
        case AckStatus::REJECTED: return "REJECTED";
        case AckStatus::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

bool parseSchedule(const cJSON* object, AutoSchedule& schedule)
{
    if (!cJSON_IsObject(object)) {
        return false;
    }

    const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(object, "enabled");
    std::uint8_t hour = 0U;
    std::uint8_t minute = 0U;
    if (!cJSON_IsBool(enabled) ||
        !readU8(cJSON_GetObjectItemCaseSensitive(object, "hour"), hour) ||
        !readU8(cJSON_GetObjectItemCaseSensitive(object, "minute"), minute)) {
        return false;
    }

    const bool scheduleEnabled = cJSON_IsTrue(enabled) != 0;
    if (scheduleEnabled && (hour > 23U || minute > 59U)) {
        return false;
    }

    schedule = AutoSchedule{scheduleEnabled, hour, minute};
    return true;
}

} // namespace

MqttManager::MqttManager(const char* topicNamespace, const char* deviceId, std::uint32_t schemaVersion)
    : topicNamespace_(topicNamespace != nullptr ? topicNamespace : ""),
      deviceId_(deviceId != nullptr ? deviceId : ""),
      schemaVersion_(schemaVersion),
      client_(nullptr),
      state_(MqttConnectionState::DISCONNECTED),
      loadCommandHandler_(nullptr),
      loadCommandHandlerContext_(nullptr),
      systemCommandHandler_(nullptr),
      systemCommandHandlerContext_(nullptr),
      configCommandHandler_(nullptr),
      configCommandHandlerContext_(nullptr),
      simulationCommandHandler_(nullptr),
      simulationCommandHandlerContext_(nullptr)
{
}

MqttManager::~MqttManager()
{
    if (client_ != nullptr) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
    }
}

void MqttManager::setLoadCommandHandler(LoadCommandHandler handler, void* context)
{
    loadCommandHandler_ = handler;
    loadCommandHandlerContext_ = context;
}

void MqttManager::setSystemCommandHandler(SystemCommandHandler handler, void* context)
{
    systemCommandHandler_ = handler;
    systemCommandHandlerContext_ = context;
}

void MqttManager::setConfigCommandHandler(ConfigCommandHandler handler, void* context)
{
    configCommandHandler_ = handler;
    configCommandHandlerContext_ = context;
}

void MqttManager::setSimulationCommandHandler(SimulationCommandHandler handler, void* context)
{
    simulationCommandHandler_ = handler;
    simulationCommandHandlerContext_ = context;
}

std::string MqttManager::fullTopic(const char* suffix) const
{
    return topicNamespace_ + "/" + suffix;
}

bool MqttManager::begin(const Credentials& credentials)
{
    if (credentials.brokerHost == nullptr || credentials.brokerHost[0] == '\0') {
        return false;
    }

    /*
     * begin() may be called again after an earlier attempt (e.g. a retry
     * following a failed start, or re-provisioned credentials); stop and
     * destroy any previous client first so a retry can't leak the earlier
     * handle's task/socket.
     */
    if (client_ != nullptr) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }

    esp_mqtt_client_config_t config{};
    config.broker.address.hostname = credentials.brokerHost;
    config.broker.address.port = credentials.brokerPort;
    config.broker.address.transport = credentials.brokerUseTls
        ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    config.credentials.username = credentials.username;
    config.credentials.authentication.password = credentials.password;
    config.credentials.client_id = deviceId_.c_str();

    client_ = esp_mqtt_client_init(&config);
    if (client_ == nullptr) {
        return false;
    }

    if (esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, &MqttManager::handleMqttEvent, this) != ESP_OK) {
        return false;
    }

    state_.store(MqttConnectionState::CONNECTING);
    if (esp_mqtt_client_start(client_) != ESP_OK) {
        state_.store(MqttConnectionState::DISCONNECTED);
        return false;
    }
    return true;
}

bool MqttManager::isConnected() const
{
    return state_.load() == MqttConnectionState::CONNECTED;
}

MqttConnectionState MqttManager::getState() const
{
    return state_.load();
}

bool MqttManager::publish(const char* topicSuffix, const std::string& payload, int qos, bool retain)
{
    if (!isConnected() || client_ == nullptr) {
        return false;
    }
    const std::string topic = fullTopic(topicSuffix);
    return esp_mqtt_client_publish(
        client_, topic.c_str(), payload.c_str(), static_cast<int>(payload.size()), qos, retain ? 1 : 0) >= 0;
}

void MqttManager::publishAcknowledgement(
    std::uint32_t commandId,
    const char* commandType,
    AckStatus status,
    const char* reason,
    const char* target)
{
    std::string json = "{\"schemaVersion\":" + std::to_string(schemaVersion_) +
                       ",\"commandId\":" + std::to_string(commandId) +
                       ",\"commandType\":";
    appendJsonString(json, commandType);
    json += ",\"status\":\"";
    json += ackText(status);
    json += "\",\"reason\":";
    appendJsonString(json, reason);
    json += ",\"target\":";
    appendJsonString(json, target);
    json += "}";
    publish(TOPIC_ACKS, json, 1, false);
}

void MqttManager::publishEvent(const char* eventType, const char* target, const char* detail)
{
    std::string json = "{\"schemaVersion\":" + std::to_string(schemaVersion_) + ",\"eventType\":";
    appendJsonString(json, eventType);
    json += ",\"target\":";
    appendJsonString(json, target);
    json += ",\"detail\":";
    appendJsonString(json, detail);
    json += "}";
    publish(TOPIC_EVENTS, json, 1, false);
}

void MqttManager::onConnected()
{
    state_.store(MqttConnectionState::CONNECTED);
    esp_mqtt_client_subscribe(client_, fullTopic(TOPIC_COMMANDS_LOAD).c_str(), 1);
    esp_mqtt_client_subscribe(client_, fullTopic(TOPIC_COMMANDS_SYSTEM).c_str(), 1);
    esp_mqtt_client_subscribe(client_, fullTopic(TOPIC_COMMANDS_CONFIG).c_str(), 1);
    esp_mqtt_client_subscribe(client_, fullTopic(TOPIC_COMMANDS_SIMULATION).c_str(), 1);
    ESP_LOGI(TAG, "mqtt: connected; command topics subscribed");
}

void MqttManager::onDisconnected()
{
    state_.store(MqttConnectionState::DISCONNECTED);
    ESP_LOGW(TAG, "mqtt: disconnected; local control remains active");
}

void MqttManager::onDataReceived(
    const char* topic,
    std::size_t topicLength,
    const char* data,
    std::size_t dataLength)
{
    const std::string received(topic, topicLength);
    if (received == fullTopic(TOPIC_COMMANDS_LOAD)) {
        handleLoadCommandMessage(data, dataLength);
    } else if (received == fullTopic(TOPIC_COMMANDS_SYSTEM)) {
        handleSystemCommandMessage(data, dataLength);
    } else if (received == fullTopic(TOPIC_COMMANDS_CONFIG)) {
        handleConfigCommandMessage(data, dataLength);
    } else if (received == fullTopic(TOPIC_COMMANDS_SIMULATION)) {
        handleSimulationCommandMessage(data, dataLength);
    }
}

void MqttManager::handleLoadCommandMessage(const char* data, std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "LOAD", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    LoadCommandRequest request{};
    if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "commandId"), request.commandId) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "nodeMac")) ||
        !parseMac(cJSON_GetObjectItemCaseSensitive(root, "nodeMac")->valuestring, request.nodeMacAddress) ||
        !readU8(cJSON_GetObjectItemCaseSensitive(root, "relayPin"), request.relayPin)) {
        cJSON_Delete(root);
        publishAcknowledgement(0U, "LOAD", AckStatus::REJECTED, "invalid commandId/nodeMac/relayPin", nullptr);
        return;
    }

    char target[18]{};
    formatMacAddressText(target, sizeof(target), request.nodeMacAddress);

    const cJSON* priority = cJSON_GetObjectItemCaseSensitive(root, "priority");
    if (priority != nullptr) {
        if (!readU16(priority, request.priority)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, "LOAD", AckStatus::REJECTED, "invalid priority", target);
            return;
        }
        request.hasPriority = true;
    }

    const cJSON* mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (mode != nullptr) {
        if (!cJSON_IsString(mode) || !parseMode(mode->valuestring, request.mode)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, "LOAD", AckStatus::REJECTED, "invalid mode", target);
            return;
        }
        request.hasMode = true;
    }

    const cJSON* schedule = cJSON_GetObjectItemCaseSensitive(root, "schedule");
    if (schedule != nullptr) {
        if (!parseSchedule(schedule, request.schedule)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, "LOAD", AckStatus::REJECTED, "invalid schedule", target);
            return;
        }
        request.hasSchedule = true;
    }

    cJSON_Delete(root);

    if (loadCommandHandler_ == nullptr) {
        publishAcknowledgement(request.commandId, "LOAD", AckStatus::REJECTED, "no handler", target);
        return;
    }

    const CommandResult result = loadCommandHandler_(loadCommandHandlerContext_, request);
    publishAcknowledgement(
        request.commandId,
        "LOAD",
        result.accepted ? AckStatus::APPLIED : AckStatus::REJECTED,
        result.reason,
        target);
}

void MqttManager::handleSystemCommandMessage(const char* data, std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "SYSTEM", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    SystemCommandRequest request{};
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "commandId"), request.commandId) ||
        !cJSON_IsString(action) || !parseSystemAction(action->valuestring, request.action)) {
        cJSON_Delete(root);
        publishAcknowledgement(0U, "SYSTEM", AckStatus::REJECTED, "invalid command", nullptr);
        return;
    }

    const cJSON* confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    if (cJSON_IsString(confirm)) {
        std::snprintf(request.confirmText, sizeof(request.confirmText), "%s", confirm->valuestring);
    }

    const cJSON* target = cJSON_GetObjectItemCaseSensitive(root, "targetNodeMac");
    if (cJSON_IsString(target) && parseMac(target->valuestring, request.targetNodeMacAddress)) {
        request.hasTargetNodeMacAddress = true;
    }

    std::uint32_t optimizerIntervalSeconds = 0U;
    std::uint32_t optimizerIntervalMinutes = 0U;

    if (readU32(cJSON_GetObjectItemCaseSensitive(root, "optimizerIntervalSeconds"), optimizerIntervalSeconds) ||
        readU32(cJSON_GetObjectItemCaseSensitive(root, "intervalSeconds"), optimizerIntervalSeconds)) {
        request.hasOptimizerIntervalSeconds = true;
        request.optimizerIntervalSeconds = optimizerIntervalSeconds;
    } else if (readU32(cJSON_GetObjectItemCaseSensitive(root, "intervalMinutes"), optimizerIntervalMinutes)) {
        if (optimizerIntervalMinutes <= 1440U) {
            request.hasOptimizerIntervalSeconds = true;
            request.optimizerIntervalSeconds = optimizerIntervalMinutes * 60U;
        }
    }

    cJSON_Delete(root);

    if (systemCommandHandler_ == nullptr) {
        publishAcknowledgement(request.commandId, "SYSTEM", AckStatus::REJECTED, "no handler", nullptr);
        return;
    }

    const CommandResult result = systemCommandHandler_(systemCommandHandlerContext_, request);
    publishAcknowledgement(
        request.commandId,
        "SYSTEM",
        result.accepted ? AckStatus::APPLIED : AckStatus::REJECTED,
        result.reason,
        nullptr);
}

void MqttManager::handleConfigCommandMessage(const char* data, std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "CONFIG", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    ConfigCommandRequest request{};
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON* nodeMac = cJSON_GetObjectItemCaseSensitive(root, "nodeMac");

    if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "commandId"), request.commandId) ||
        !cJSON_IsString(action) || !parseConfigAction(action->valuestring, request.action) ||
        !cJSON_IsString(nodeMac) || !parseMac(nodeMac->valuestring, request.nodeMacAddress)) {
        cJSON_Delete(root);
        publishAcknowledgement(0U, "CONFIG", AckStatus::REJECTED, "invalid commandId/action/nodeMac", nullptr);
        return;
    }

    char target[18]{};
    formatMacAddressText(target, sizeof(target), request.nodeMacAddress);
    const char* commandType = action->valuestring;

    if (request.action == ConfigCommandAction::COMMISSION_NODE ||
        request.action == ConfigCommandAction::RENAME_NODE) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(root, "friendlyName");
        if (!cJSON_IsString(name) || name->valuestring[0] == '\0' ||
            std::strlen(name->valuestring) >= sizeof(request.friendlyName)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "invalid friendlyName", target);
            return;
        }
        request.hasFriendlyName = true;
        std::snprintf(request.friendlyName, sizeof(request.friendlyName), "%s", name->valuestring);
    }

    if (request.action == ConfigCommandAction::CONFIGURE_LOAD) {
        const cJSON* load = cJSON_GetObjectItemCaseSensitive(root, "load");
        const cJSON* name = cJSON_IsObject(load) ? cJSON_GetObjectItemCaseSensitive(load, "name") : nullptr;
        const cJSON* mode = cJSON_IsObject(load) ? cJSON_GetObjectItemCaseSensitive(load, "mode") : nullptr;
        const cJSON* powerType = cJSON_IsObject(load) ? cJSON_GetObjectItemCaseSensitive(load, "powerType") : nullptr;

        if (!cJSON_IsObject(load) ||
            !cJSON_IsString(name) || name->valuestring[0] == '\0' ||
            std::strlen(name->valuestring) >= sizeof(request.loadName) ||
            !readU8(cJSON_GetObjectItemCaseSensitive(load, "relayPin"), request.relayPin) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(load, "relayActiveHigh")) ||
            !cJSON_IsString(mode) || !parseMode(mode->valuestring, request.mode) ||
            !cJSON_IsString(powerType) || !parsePowerType(powerType->valuestring, request.powerType) ||
            !readU16(cJSON_GetObjectItemCaseSensitive(load, "priority"), request.priority) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(load, "powerRatingWatts"), request.powerRatingWatts) ||
            !parseSchedule(cJSON_GetObjectItemCaseSensitive(load, "schedule"), request.schedule)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "invalid load configuration", target);
            return;
        }

        if (request.powerRatingWatts < 0.0F) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "invalid load power rating", target);
            return;
        }

        std::snprintf(request.loadName, sizeof(request.loadName), "%s", name->valuestring);
        request.relayActiveHigh = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(load, "relayActiveHigh")) != 0;
        request.hasLoadConfiguration = true;
        request.hasRelayPin = true;
    }

    if (request.action == ConfigCommandAction::REMOVE_LOAD) {
        if (!readU8(cJSON_GetObjectItemCaseSensitive(root, "relayPin"), request.relayPin)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "relayPin required", target);
            return;
        }
        request.hasRelayPin = true;
    }

    if (request.action == ConfigCommandAction::CONFIGURE_BATTERY_SENSOR) {
        const cJSON* battery = cJSON_GetObjectItemCaseSensitive(root, "batterySensor");
        if (!cJSON_IsObject(battery) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(battery, "shuntResistanceOhms"), request.batteryShuntResistanceOhms) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(battery, "maximumExpectedCurrentAmps"), request.batteryMaximumExpectedCurrentAmps) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(battery, "emaAlpha"), request.batteryEmaAlpha) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(battery, "batteryCapacityAmpHours"), request.batteryCapacityAmpHours) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(battery, "initialStateOfChargePercent"), request.batteryInitialStateOfChargePercent) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(battery, "nominalVoltageVolts"), request.batteryNominalVoltageVolts)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "invalid batterySensor", target);
            return;
        }
        request.hasBatterySensorConfiguration = true;
    }

    if (request.action == ConfigCommandAction::CONFIGURE_POWER_LIMITS) {
        const cJSON* limits = cJSON_GetObjectItemCaseSensitive(root, "powerLimits");
        if (!cJSON_IsObject(limits) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(limits, "minimumStateOfChargePercent"), request.minimumStateOfChargePercent) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(limits, "maximumBatteryDischargeCurrentAmps"), request.maximumBatteryDischargeCurrentAmps) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(limits, "maximumMainCurrentAmps"), request.maximumMainCurrentAmps) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(limits, "requiredRuntimeHours"), request.requiredRuntimeHours)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "invalid powerLimits", target);
            return;
        }
        request.hasPowerLimitsConfiguration = true;
    }

    cJSON_Delete(root);

    if (configCommandHandler_ == nullptr) {
        publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "no handler", target);
        return;
    }

    const CommandResult result = configCommandHandler_(configCommandHandlerContext_, request);
    const AckStatus status = !result.accepted
        ? AckStatus::REJECTED
        : (result.completed ? AckStatus::APPLIED : AckStatus::ACCEPTED);

    publishAcknowledgement(request.commandId, commandType, status, result.reason, target);
}

void MqttManager::handleSimulationCommandMessage(const char* data, std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "SIMULATION", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    SimulationCommandRequest request{};
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "commandId"), request.commandId) ||
        !cJSON_IsString(action) || !parseSimulationAction(action->valuestring, request.action)) {
        cJSON_Delete(root);
        publishAcknowledgement(0U, "SIMULATION", AckStatus::REJECTED, "invalid commandId/action", nullptr);
        return;
    }

    const char* commandType = action->valuestring;

    if (request.action == SimulationCommandAction::SET_VALUES) {
        const cJSON* values = cJSON_GetObjectItemCaseSensitive(root, "values");
        if (!cJSON_IsObject(values)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "invalid values", nullptr);
            return;
        }

        const cJSON* voltage = cJSON_GetObjectItemCaseSensitive(values, "batteryVoltageVolts");
        const cJSON* current = cJSON_GetObjectItemCaseSensitive(values, "batteryCurrentAmps");
        const cJSON* soc = cJSON_GetObjectItemCaseSensitive(values, "stateOfChargePercent");

        const bool hasVoltage = voltage != nullptr;
        const bool hasCurrent = current != nullptr;

        if (hasVoltage != hasCurrent ||
            (hasVoltage && (!readFloat(voltage, request.batteryVoltageVolts) ||
                            !readFloat(current, request.batteryCurrentAmps))) ||
            (soc != nullptr && !readFloat(soc, request.stateOfChargePercent))) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "invalid simulation values", nullptr);
            return;
        }

        request.hasElectricalMeasurements = hasVoltage && hasCurrent;
        request.hasStateOfChargePercent = soc != nullptr;

        if (!request.hasElectricalMeasurements && !request.hasStateOfChargePercent) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "no simulation values provided", nullptr);
            return;
        }
    }

    cJSON_Delete(root);

    if (simulationCommandHandler_ == nullptr) {
        publishAcknowledgement(request.commandId, commandType, AckStatus::REJECTED, "no handler", nullptr);
        return;
    }

    const CommandResult result = simulationCommandHandler_(simulationCommandHandlerContext_, request);
    const AckStatus status = !result.accepted
        ? AckStatus::REJECTED
        : (result.completed ? AckStatus::APPLIED : AckStatus::ACCEPTED);

    publishAcknowledgement(request.commandId, commandType, status, result.reason, nullptr);
}

void MqttManager::printDiagnosticReport() const
{
    const char* text = "DISCONNECTED";
    if (state_.load() == MqttConnectionState::CONNECTING) text = "CONNECTING";
    if (state_.load() == MqttConnectionState::CONNECTED) text = "CONNECTED";
    ESP_LOGI(TAG, "State=%s namespace=%s", text, topicNamespace_.c_str());
}

void MqttManager::handleMqttEvent(void* handlerArgs, esp_event_base_t base, std::int32_t eventId, void* eventData)
{
    (void)base;
    MqttManager* self = static_cast<MqttManager*>(handlerArgs);
    if (self == nullptr) return;

    const esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(eventData);
    switch (eventId) {
        case MQTT_EVENT_CONNECTED:
            self->onConnected();
            break;
        case MQTT_EVENT_DISCONNECTED:
            self->onDisconnected();
            break;
        case MQTT_EVENT_DATA:
            if (event != nullptr && event->topic_len > 0 && event->data_len > 0) {
                self->onDataReceived(
                    event->topic,
                    static_cast<std::size_t>(event->topic_len),
                    event->data,
                    static_cast<std::size_t>(event->data_len));
            }
            break;
        default:
            break;
    }
}

} // namespace kilowatts
