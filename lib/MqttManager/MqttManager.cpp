#include "MqttManager.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace kilowatts {
namespace {

constexpr const char* TAG = "MQTT";
constexpr const char* MQTT_STATUS_ONLINE = "online";
constexpr const char* MQTT_STATUS_OFFLINE = "offline";

bool parseMac(const char* text, Load::MacAddress& mac)
{
    if (text == nullptr) return false;

    unsigned int bytes[6]{};
    if (std::sscanf(
            text,
            "%2x:%2x:%2x:%2x:%2x:%2x",
            &bytes[0], &bytes[1], &bytes[2],
            &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }

    for (std::size_t i = 0U; i < 6U; ++i) {
        if (bytes[i] > 255U) return false;
        mac[i] = static_cast<std::uint8_t>(bytes[i]);
    }
    return true;
}

bool readU8(const cJSON* item, std::uint8_t& value)
{
    if (!cJSON_IsNumber(item) ||
        !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 ||
        item->valuedouble > 255.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<std::uint8_t>(item->valuedouble);
    return true;
}

bool readU16(const cJSON* item, std::uint16_t& value)
{
    if (!cJSON_IsNumber(item) ||
        !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 ||
        item->valuedouble > 65535.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<std::uint16_t>(item->valuedouble);
    return true;
}

bool readU32(const cJSON* item, std::uint32_t& value)
{
    if (!cJSON_IsNumber(item) ||
        !std::isfinite(item->valuedouble) ||
        item->valuedouble <= 0.0 ||
        item->valuedouble > 4294967295.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<std::uint32_t>(item->valuedouble);
    return true;
}

bool readFloat(const cJSON* item, float& value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) return false;
    value = static_cast<float>(item->valuedouble);
    return std::isfinite(value);
}

bool readString(const cJSON* item, char* destination, std::size_t size)
{
    if (!cJSON_IsString(item) || item->valuestring == nullptr ||
        item->valuestring[0] == '\0' || std::strlen(item->valuestring) >= size) {
        return false;
    }
    std::snprintf(destination, size, "%s", item->valuestring);
    return true;
}

bool parseMode(const char* text, LoadMode::Value& mode)
{
    if (text == nullptr) return false;
    if (std::strcmp(text, "FIXED_OFF") == 0) { mode = LoadMode::Fixed::OFF; return true; }
    if (std::strcmp(text, "FIXED_ON") == 0) { mode = LoadMode::Fixed::ON; return true; }
    if (std::strcmp(text, "AUTO_OFF") == 0) { mode = LoadMode::Auto::OFF; return true; }
    if (std::strcmp(text, "AUTO_ON") == 0) { mode = LoadMode::Auto::ON; return true; }
    return false;
}

bool parsePowerType(const char* text, LoadPowerType& powerType)
{
    if (text == nullptr) return false;
    if (std::strcmp(text, "AC") == 0) { powerType = LoadPowerType::AC; return true; }
    if (std::strcmp(text, "DC") == 0) { powerType = LoadPowerType::DC; return true; }
    return false;
}

bool parseSchedule(const cJSON* object, AutoSchedule& schedule)
{
    if (object == nullptr) {
        schedule = AutoSchedule{};
        return true;
    }
    if (!cJSON_IsObject(object)) return false;

    const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(object, "enabled");
    if (!cJSON_IsBool(enabled)) return false;
    if (!cJSON_IsTrue(enabled)) {
        schedule = AutoSchedule{};
        return true;
    }

    std::uint8_t startHour = 0U;
    std::uint8_t startMinute = 0U;
    std::uint8_t endHour = 0U;
    std::uint8_t endMinute = 0U;

    if (!readU8(cJSON_GetObjectItemCaseSensitive(object, "startHour"), startHour) ||
        !readU8(cJSON_GetObjectItemCaseSensitive(object, "startMinute"), startMinute) ||
        !readU8(cJSON_GetObjectItemCaseSensitive(object, "endHour"), endHour) ||
        !readU8(cJSON_GetObjectItemCaseSensitive(object, "endMinute"), endMinute) ||
        startHour > 23U || startMinute > 59U ||
        endHour > 23U || endMinute > 59U) {
        return false;
    }

    if (startHour == endHour && startMinute == endMinute) return false;
    schedule = AutoSchedule{true, startHour, startMinute, endHour, endMinute};
    return true;
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

AckStatus resultStatus(const CommandResult& result)
{
    if (!result.accepted) return AckStatus::REJECTED;
    return result.completed ? AckStatus::APPLIED : AckStatus::ACCEPTED;
}

} // namespace

MqttManager::MqttManager(
    const char* topicNamespace,
    const char* deviceId,
    std::uint32_t schemaVersion)
    : topicNamespace_(topicNamespace != nullptr ? topicNamespace : ""),
      deviceId_(deviceId != nullptr ? deviceId : ""),
      statusTopic_(),
      schemaVersion_(schemaVersion),
      stateSystemJson_(),
      stateLoadsJson_(),
      stateNodesJson_(),
      client_(nullptr),
      state_(MqttConnectionState::DISCONNECTED),
      loadCommandHandler_(nullptr),
      loadCommandHandlerContext_(nullptr),
      systemCommandHandler_(nullptr),
      systemCommandHandlerContext_(nullptr),
      configCommandHandler_(nullptr),
      configCommandHandlerContext_(nullptr),
      simulationCommandHandler_(nullptr),
      simulationCommandHandlerContext_(nullptr),
      powerPlanningCommandHandler_(nullptr),
      powerPlanningCommandHandlerContext_(nullptr)
{
}

MqttManager::~MqttManager()
{
    if (client_ != nullptr) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
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

void MqttManager::setPowerPlanningCommandHandler(PowerPlanningCommandHandler handler, void* context)
{
    powerPlanningCommandHandler_ = handler;
    powerPlanningCommandHandlerContext_ = context;
}

std::string MqttManager::fullTopic(const char* suffix) const
{
    return topicNamespace_ + "/" + suffix;
}

bool MqttManager::begin(const Credentials& credentials)
{
    if (credentials.brokerHost == nullptr || credentials.brokerHost[0] == '\0') return false;

    if (client_ != nullptr) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }

    esp_mqtt_client_config_t config{};
    config.broker.address.hostname = credentials.brokerHost;
    config.broker.address.port = credentials.brokerPort;
    config.broker.address.transport = credentials.brokerUseTls
        ? MQTT_TRANSPORT_OVER_SSL
        : MQTT_TRANSPORT_OVER_TCP;
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    config.credentials.username = credentials.username;
    config.credentials.authentication.password = credentials.password;
    config.credentials.client_id = deviceId_.c_str();

    statusTopic_ = fullTopic(TOPIC_STATUS);
    config.session.last_will.topic = statusTopic_.c_str();
    config.session.last_will.msg = MQTT_STATUS_OFFLINE;
    config.session.last_will.msg_len = static_cast<int>(std::strlen(MQTT_STATUS_OFFLINE));
    config.session.last_will.qos = 1;
    config.session.last_will.retain = true;

    client_ = esp_mqtt_client_init(&config);
    if (client_ == nullptr) return false;

    if (esp_mqtt_client_register_event(
            client_, MQTT_EVENT_ANY, &MqttManager::handleMqttEvent, this) != ESP_OK) {
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        return false;
    }

    state_.store(MqttConnectionState::CONNECTING);
    if (esp_mqtt_client_start(client_) != ESP_OK) {
        state_.store(MqttConnectionState::DISCONNECTED);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
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

bool MqttManager::publishRaw(
    const char* topicSuffix,
    const std::string& payload,
    int qos,
    bool retain)
{
    if (!isConnected() || client_ == nullptr || topicSuffix == nullptr) return false;

    const std::string topic = fullTopic(topicSuffix);
    return esp_mqtt_client_publish(
               client_, topic.c_str(), payload.c_str(),
               static_cast<int>(payload.size()), qos, retain ? 1 : 0) >= 0;
}

bool MqttManager::publishCombinedState()
{
    std::string json = "{\"system\":";
    json += stateSystemJson_.empty() ? "null" : stateSystemJson_;
    json += ",\"loads\":";
    json += stateLoadsJson_.empty() ? "null" : stateLoadsJson_;
    json += ",\"nodes\":";
    json += stateNodesJson_.empty() ? "null" : stateNodesJson_;
    json += "}";
    return publishRaw(TOPIC_STATE, json, 0, true);
}

bool MqttManager::publish(
    const char* topicSuffix,
    const std::string& payload,
    int qos,
    bool retain)
{
    if (topicSuffix == nullptr) return false;

    if (std::strcmp(topicSuffix, TOPIC_STATE_SYSTEM) == 0) {
        stateSystemJson_ = payload;
        return true;
    }
    if (std::strcmp(topicSuffix, TOPIC_STATE_LOADS) == 0) {
        stateLoadsJson_ = payload;
        return true;
    }
    if (std::strcmp(topicSuffix, TOPIC_STATE_NODES) == 0) {
        stateNodesJson_ = payload;
        return publishCombinedState();
    }

    // Older internal state builders may still call these while Central assembles state.
    // They are never external MQTT topics.
    if (std::strcmp(topicSuffix, TOPIC_STATE_TREE) == 0 ||
        std::strcmp(topicSuffix, TOPIC_CONFIG_NODES) == 0) {
        return true;
    }

    return publishRaw(topicSuffix, payload, qos, retain);
}

bool MqttManager::publishStatus()
{
    return publishRaw(TOPIC_STATUS, MQTT_STATUS_ONLINE, 1, true);
}

void MqttManager::publishAcknowledgement(
    std::uint32_t commandId,
    const char* commandType,
    AckStatus status,
    const char* reason,
    const char* target)
{
    std::string json =
        "{\"schemaVersion\":" + std::to_string(schemaVersion_) +
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
    (void)publishRaw(TOPIC_ACK, json, 1, false);
}

void MqttManager::publishEvent(
    const char* eventType,
    const char* target,
    const char* detail)
{
    std::string text;
    if (target != nullptr && target[0] != '\0') text += target;
    if (detail != nullptr && detail[0] != '\0') {
        if (!text.empty()) text += ": ";
        text += detail;
    }
    publishAlert(eventType, "info", text.empty() ? nullptr : text.c_str());
}

void MqttManager::publishAlert(
    const char* alertType,
    const char* severity,
    const char* detail)
{
    std::string json =
        "{\"schemaVersion\":" + std::to_string(schemaVersion_) +
        ",\"alertType\":";
    appendJsonString(json, alertType);
    json += ",\"severity\":";
    appendJsonString(json, severity);
    json += ",\"detail\":";
    appendJsonString(json, detail);
    json += "}";
    (void)publishRaw(TOPIC_ALERT, json, 1, false);
}

void MqttManager::onConnected()
{
    state_.store(MqttConnectionState::CONNECTED);
    (void)publishStatus();
    esp_mqtt_client_subscribe(client_, fullTopic(TOPIC_COMMAND).c_str(), 1);
    ESP_LOGI(TAG, "mqtt connected; subscribed to command");
}

void MqttManager::onDisconnected()
{
    state_.store(MqttConnectionState::DISCONNECTED);
    ESP_LOGW(TAG, "mqtt disconnected");
}

void MqttManager::onDataReceived(
    const char* topic,
    std::size_t topicLength,
    const char* data,
    std::size_t dataLength)
{
    if (std::string(topic, topicLength) != fullTopic(TOPIC_COMMAND)) return;

    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "command", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const std::string commandType = cJSON_IsString(type) && type->valuestring != nullptr
        ? type->valuestring
        : "";
    cJSON_Delete(root);

    if (commandType == "node") {
        handleNodeCommandMessage(data, dataLength);
    } else if (commandType == "load") {
        handleLoadCommandMessage(data, dataLength);
    } else if (commandType == "battery") {
        handleBatteryCommandMessage(data, dataLength);
    } else if (commandType == "sensor") {
        handleSensorCommandMessage(data, dataLength);
    } else if (commandType == "system") {
        handleSystemCommandMessage(data, dataLength);
    } else {
        publishAcknowledgement(0U, "command", AckStatus::REJECTED,
            "type must be node, load, battery, sensor or system", nullptr);
    }
}

void MqttManager::handleNodeCommandMessage(
    const char* data,
    std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "node", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    ConfigCommandRequest request{};
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON* mac = cJSON_GetObjectItemCaseSensitive(root, "mac");

    if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "commandId"), request.commandId) ||
        !cJSON_IsString(action) || action->valuestring == nullptr ||
        !cJSON_IsString(mac) || !parseMac(mac->valuestring, request.nodeMacAddress)) {
        cJSON_Delete(root);
        publishAcknowledgement(0U, "node", AckStatus::REJECTED,
            "commandId, action and mac required", nullptr);
        return;
    }

    const std::string actionText = action->valuestring;
    if (actionText == "add") {
        request.action = ConfigCommandAction::COMMISSION_NODE;
    } else if (actionText == "update") {
        request.action = ConfigCommandAction::RENAME_NODE;
    } else if (actionText == "delete") {
        request.action = ConfigCommandAction::DECOMMISSION_NODE;
    } else {
        cJSON_Delete(root);
        publishAcknowledgement(request.commandId, "node", AckStatus::REJECTED,
            "action must be add, update or delete", nullptr);
        return;
    }

    if (request.action != ConfigCommandAction::DECOMMISSION_NODE) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (!readString(name, request.friendlyName, sizeof(request.friendlyName))) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, "node", AckStatus::REJECTED,
                "name required", nullptr);
            return;
        }
        request.hasFriendlyName = true;
    }

    char target[18]{};
    formatMacAddressText(target, sizeof(target), request.nodeMacAddress);
    cJSON_Delete(root);

    if (configCommandHandler_ == nullptr) {
        publishAcknowledgement(request.commandId, "node", AckStatus::REJECTED,
            "no handler", target);
        return;
    }

    const CommandResult result = configCommandHandler_(configCommandHandlerContext_, request);
    publishAcknowledgement(request.commandId, "node", resultStatus(result), result.reason, target);
}

void MqttManager::handleLoadCommandMessage(
    const char* data,
    std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "load", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    std::uint32_t commandId = 0U;
    Load::MacAddress nodeMac{};
    std::uint8_t relayPin = 0U;
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON* mac = cJSON_GetObjectItemCaseSensitive(root, "nodeMac");

    if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "commandId"), commandId) ||
        !cJSON_IsString(action) || action->valuestring == nullptr ||
        !cJSON_IsString(mac) || !parseMac(mac->valuestring, nodeMac) ||
        !readU8(cJSON_GetObjectItemCaseSensitive(root, "relayPin"), relayPin)) {
        cJSON_Delete(root);
        publishAcknowledgement(0U, "load", AckStatus::REJECTED,
            "commandId, action, nodeMac and relayPin required", nullptr);
        return;
    }

    char target[18]{};
    formatMacAddressText(target, sizeof(target), nodeMac);
    const std::string actionText = action->valuestring;

    if (actionText == "update") {
        LoadCommandRequest request{};
        request.commandId = commandId;
        request.nodeMacAddress = nodeMac;
        request.relayPin = relayPin;

        const cJSON* priority = cJSON_GetObjectItemCaseSensitive(root, "priority");
        if (priority != nullptr) {
            if (!readU16(priority, request.priority)) {
                cJSON_Delete(root);
                publishAcknowledgement(commandId, "load", AckStatus::REJECTED,
                    "invalid priority", target);
                return;
            }
            request.hasPriority = true;
        }

        const cJSON* mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        if (mode != nullptr) {
            if (!cJSON_IsString(mode) || !parseMode(mode->valuestring, request.mode)) {
                cJSON_Delete(root);
                publishAcknowledgement(commandId, "load", AckStatus::REJECTED,
                    "invalid mode", target);
                return;
            }
            request.hasMode = true;
        }

        const cJSON* schedule = cJSON_GetObjectItemCaseSensitive(root, "schedule");
        if (schedule != nullptr) {
            if (!parseSchedule(schedule, request.schedule)) {
                cJSON_Delete(root);
                publishAcknowledgement(commandId, "load", AckStatus::REJECTED,
                    "invalid schedule", target);
                return;
            }
            request.hasSchedule = true;
        }

        cJSON_Delete(root);

        if (!request.hasPriority && !request.hasMode && !request.hasSchedule) {
            publishAcknowledgement(commandId, "load", AckStatus::REJECTED,
                "provide priority, mode or schedule", target);
            return;
        }
        if (loadCommandHandler_ == nullptr) {
            publishAcknowledgement(commandId, "load", AckStatus::REJECTED,
                "no handler", target);
            return;
        }

        const CommandResult result = loadCommandHandler_(loadCommandHandlerContext_, request);
        publishAcknowledgement(commandId, "load", resultStatus(result), result.reason, target);
        return;
    }

    ConfigCommandRequest request{};
    request.commandId = commandId;
    request.nodeMacAddress = nodeMac;
    request.relayPin = relayPin;
    request.hasRelayPin = true;

    if (actionText == "delete") {
        request.action = ConfigCommandAction::REMOVE_LOAD;
        cJSON_Delete(root);
    } else if (actionText == "add") {
        request.action = ConfigCommandAction::CONFIGURE_LOAD;
        request.hasLoadConfiguration = true;

        const cJSON* name = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON* mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        const cJSON* powerType = cJSON_GetObjectItemCaseSensitive(root, "powerType");
        const cJSON* activeHigh = cJSON_GetObjectItemCaseSensitive(root, "activeHigh");

        if (!readString(name, request.loadName, sizeof(request.loadName)) ||
            !readFloat(cJSON_GetObjectItemCaseSensitive(root, "power"), request.powerRatingWatts) ||
            request.powerRatingWatts < 0.0F ||
            !readU16(cJSON_GetObjectItemCaseSensitive(root, "priority"), request.priority) ||
            !cJSON_IsString(mode) || !parseMode(mode->valuestring, request.mode) ||
            !cJSON_IsString(powerType) || !parsePowerType(powerType->valuestring, request.powerType) ||
            !cJSON_IsBool(activeHigh) ||
            !parseSchedule(cJSON_GetObjectItemCaseSensitive(root, "schedule"), request.schedule)) {
            cJSON_Delete(root);
            publishAcknowledgement(commandId, "load", AckStatus::REJECTED,
                "invalid load settings", target);
            return;
        }
        request.relayActiveHigh = cJSON_IsTrue(activeHigh) != 0;
        cJSON_Delete(root);
    } else {
        cJSON_Delete(root);
        publishAcknowledgement(commandId, "load", AckStatus::REJECTED,
            "action must be add, update or delete", target);
        return;
    }

    if (configCommandHandler_ == nullptr) {
        publishAcknowledgement(commandId, "load", AckStatus::REJECTED,
            "no handler", target);
        return;
    }

    const CommandResult result = configCommandHandler_(configCommandHandlerContext_, request);
    publishAcknowledgement(commandId, "load", resultStatus(result), result.reason, target);
}

void MqttManager::handleBatteryCommandMessage(
    const char* data,
    std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "battery", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    std::uint32_t commandId = 0U;
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    PowerPlanningCommandRequest request{};

    if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "commandId"), commandId) ||
        !cJSON_IsString(action) || action->valuestring == nullptr ||
        std::strcmp(action->valuestring, "set") != 0 ||
        !readFloat(cJSON_GetObjectItemCaseSensitive(root, "budget"), request.P_budget) ||
        !readFloat(cJSON_GetObjectItemCaseSensitive(root, "reserve"), request.P_reserve) ||
        !readFloat(cJSON_GetObjectItemCaseSensitive(root, "minSoc"), request.minimumStateOfChargePercent)) {
        cJSON_Delete(root);
        publishAcknowledgement(commandId, "battery", AckStatus::REJECTED,
            "use action=set with budget, reserve and minSoc", nullptr);
        return;
    }

    request.requiredRuntimeHours = 0.0F;
    const cJSON* runtime = cJSON_GetObjectItemCaseSensitive(root, "runtime");
    if (runtime != nullptr && !readFloat(runtime, request.requiredRuntimeHours)) {
        cJSON_Delete(root);
        publishAcknowledgement(commandId, "battery", AckStatus::REJECTED,
            "invalid runtime", nullptr);
        return;
    }
    cJSON_Delete(root);

    if (request.P_budget <= 0.0F ||
        request.P_reserve < 0.0F || request.P_reserve > request.P_budget ||
        request.minimumStateOfChargePercent < 0.0F || request.minimumStateOfChargePercent > 100.0F ||
        request.requiredRuntimeHours < 0.0F) {
        publishAcknowledgement(commandId, "battery", AckStatus::REJECTED,
            "invalid battery settings", nullptr);
        return;
    }

    if (powerPlanningCommandHandler_ == nullptr) {
        publishAcknowledgement(commandId, "battery", AckStatus::REJECTED,
            "no handler", nullptr);
        return;
    }

    const CommandResult result = powerPlanningCommandHandler_(
        powerPlanningCommandHandlerContext_, request);
    publishAcknowledgement(commandId, "battery", resultStatus(result), result.reason, nullptr);
}

void MqttManager::handleSensorCommandMessage(
    const char* data,
    std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "sensor", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    SimulationCommandRequest request{};
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "commandId"), request.commandId) ||
        !cJSON_IsString(action) || action->valuestring == nullptr) {
        cJSON_Delete(root);
        publishAcknowledgement(0U, "sensor", AckStatus::REJECTED,
            "invalid commandId/action", nullptr);
        return;
    }

    const std::string actionText = action->valuestring;
    if (actionText == "sim") {
        request.action = SimulationCommandAction::ENABLE;
        cJSON_Delete(root);
    } else if (actionText == "ina219") {
        request.action = SimulationCommandAction::DISABLE;
        cJSON_Delete(root);
    } else if (actionText == "values") {
        request.action = SimulationCommandAction::SET_VALUES;
        const cJSON* voltage = cJSON_GetObjectItemCaseSensitive(root, "voltage");
        const cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current");
        const cJSON* soc = cJSON_GetObjectItemCaseSensitive(root, "soc");
        const bool hasVoltage = voltage != nullptr;
        const bool hasCurrent = current != nullptr;

        if (hasVoltage != hasCurrent ||
            (hasVoltage && (!readFloat(voltage, request.batteryVoltageVolts) ||
                            !readFloat(current, request.batteryCurrentAmps))) ||
            (soc != nullptr && (!readFloat(soc, request.stateOfChargePercent) ||
                                request.stateOfChargePercent < 0.0F ||
                                request.stateOfChargePercent > 100.0F))) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, "sensor", AckStatus::REJECTED,
                "invalid sensor values", nullptr);
            return;
        }

        request.hasElectricalMeasurements = hasVoltage && hasCurrent;
        request.hasStateOfChargePercent = soc != nullptr;
        cJSON_Delete(root);

        if (!request.hasElectricalMeasurements && !request.hasStateOfChargePercent) {
            publishAcknowledgement(request.commandId, "sensor", AckStatus::REJECTED,
                "provide voltage/current or soc", nullptr);
            return;
        }
    } else {
        cJSON_Delete(root);
        publishAcknowledgement(request.commandId, "sensor", AckStatus::REJECTED,
            "action must be ina219, sim or values", nullptr);
        return;
    }

    if (simulationCommandHandler_ == nullptr) {
        publishAcknowledgement(request.commandId, "sensor", AckStatus::REJECTED,
            "no handler", nullptr);
        return;
    }

    const CommandResult result = simulationCommandHandler_(
        simulationCommandHandlerContext_, request);
    publishAcknowledgement(request.commandId, "sensor", resultStatus(result), result.reason, nullptr);
}

void MqttManager::handleSystemCommandMessage(
    const char* data,
    std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "system", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    SystemCommandRequest request{};
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "commandId"), request.commandId) ||
        !cJSON_IsString(action) || action->valuestring == nullptr) {
        cJSON_Delete(root);
        publishAcknowledgement(0U, "system", AckStatus::REJECTED,
            "invalid commandId/action", nullptr);
        return;
    }

    const std::string actionText = action->valuestring;
    if (actionText == "optimize") {
        request.action = SystemCommandAction::REQUEST_OPTIMIZATION_CYCLE;
    } else if (actionText == "restart") {
        request.action = SystemCommandAction::REBOOT_CENTRAL;
    } else if (actionText == "interval") {
        request.action = SystemCommandAction::SET_OPTIMIZER_INTERVAL;
        if (!readU32(cJSON_GetObjectItemCaseSensitive(root, "seconds"),
                request.optimizerIntervalSeconds)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, "system", AckStatus::REJECTED,
                "seconds required", nullptr);
            return;
        }
        request.hasOptimizerIntervalSeconds = true;
    } else {
        cJSON_Delete(root);
        publishAcknowledgement(request.commandId, "system", AckStatus::REJECTED,
            "action must be optimize, restart or interval", nullptr);
        return;
    }
    cJSON_Delete(root);

    if (systemCommandHandler_ == nullptr) {
        publishAcknowledgement(request.commandId, "system", AckStatus::REJECTED,
            "no handler", nullptr);
        return;
    }

    const CommandResult result = systemCommandHandler_(systemCommandHandlerContext_, request);
    publishAcknowledgement(request.commandId, "system", resultStatus(result), result.reason, nullptr);
}

void MqttManager::printDiagnosticReport() const
{
    const char* text = "DISCONNECTED";
    if (state_.load() == MqttConnectionState::CONNECTING) text = "CONNECTING";
    else if (state_.load() == MqttConnectionState::CONNECTED) text = "CONNECTED";

    ESP_LOGI(TAG, "State=%s namespace=%s", text, topicNamespace_.c_str());
}

void MqttManager::handleMqttEvent(
    void* handlerArgs,
    esp_event_base_t base,
    std::int32_t eventId,
    void* eventData)
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
