/**
 * @file MqttManager.cpp
 * @brief Implements Central Node MQTT connectivity and the kilowatts/v1
 *        topic contract.
 */

#include "MqttManager.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace kilowatts {


static const char *TAG = "MQTT_MANAGER";


namespace {

bool parseMacAddressText(const char* text, Load::MacAddress& macAddress)
{
    if (text == nullptr) {
        return false;
    }

    unsigned int bytes[6] = {};
    const int fieldsParsed = std::sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
                                          &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]);
    if (fieldsParsed != 6) {
        return false;
    }

    for (std::size_t i = 0U; i < 6U; ++i) {
        macAddress[i] = static_cast<std::uint8_t>(bytes[i]);
    }

    return true;
}


bool parseLoadModeText(const char* text, LoadMode::Value& mode)
{
    if (text == nullptr) {
        return false;
    }

    if (std::strcmp(text, "FIXED_ON") == 0) { mode = LoadMode::Fixed::ON; return true; }
    if (std::strcmp(text, "FIXED_OFF") == 0) { mode = LoadMode::Fixed::OFF; return true; }
    if (std::strcmp(text, "AUTO_ON") == 0) { mode = LoadMode::Auto::ON; return true; }
    if (std::strcmp(text, "AUTO_OFF") == 0) { mode = LoadMode::Auto::OFF; return true; }

    return false;
}


bool parseConfigCommandActionText(const char* text, ConfigCommandAction& action)
{
    if (text == nullptr) {
        return false;
    }

    if (std::strcmp(text, "COMMISSION_NODE") == 0) { action = ConfigCommandAction::COMMISSION_NODE; return true; }
    if (std::strcmp(text, "RENAME_NODE") == 0) { action = ConfigCommandAction::RENAME_NODE; return true; }
    if (std::strcmp(text, "DECOMMISSION_NODE") == 0) { action = ConfigCommandAction::DECOMMISSION_NODE; return true; }
    if (std::strcmp(text, "CONFIGURE_LOAD") == 0) { action = ConfigCommandAction::CONFIGURE_LOAD; return true; }
    if (std::strcmp(text, "CONFIGURE_BATTERY_SENSOR") == 0) { action = ConfigCommandAction::CONFIGURE_BATTERY_SENSOR; return true; }

    return false;
}


bool isUnsignedByteJsonNumber(const cJSON* item, std::uint8_t& value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 || item->valuedouble > 255.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<std::uint8_t>(item->valuedouble);
    return true;
}


bool isUnsignedIntJsonNumber(const cJSON* item, std::uint32_t& value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble <= 0.0 || item->valuedouble > 4294967295.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<std::uint32_t>(item->valuedouble);
    return true;
}


bool isUnsignedShortJsonNumber(const cJSON* item, std::uint16_t& value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 || item->valuedouble > 65535.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<std::uint16_t>(item->valuedouble);
    return true;
}


bool isFiniteJsonNumber(const cJSON* item, float& value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) {
        return false;
    }
    value = static_cast<float>(item->valuedouble);
    return std::isfinite(value);
}


bool parseSystemCommandActionText(const char* text, SystemCommandAction& action)
{
    if (text == nullptr) {
        return false;
    }

    if (std::strcmp(text, "REQUEST_OPTIMIZATION_CYCLE") == 0) { action = SystemCommandAction::REQUEST_OPTIMIZATION_CYCLE; return true; }
    if (std::strcmp(text, "FACTORY_RESET_CENTRAL") == 0) { action = SystemCommandAction::FACTORY_RESET_CENTRAL; return true; }
    if (std::strcmp(text, "FACTORY_RESET_NODE") == 0) { action = SystemCommandAction::FACTORY_RESET_NODE; return true; }
    if (std::strcmp(text, "APPLY_SAFETY_CONFIG") == 0) { action = SystemCommandAction::APPLY_SAFETY_CONFIG; return true; }

    return false;
}


bool parseDevelopmentCommandActionText(const char* text, DevelopmentCommandAction& action)
{
    if (text == nullptr) {
        return false;
    }

    if (std::strcmp(text, "START_SESSION") == 0) { action = DevelopmentCommandAction::START_SESSION; return true; }
    if (std::strcmp(text, "END_SESSION") == 0) { action = DevelopmentCommandAction::END_SESSION; return true; }
    if (std::strcmp(text, "SET_SENSOR_INPUT") == 0) { action = DevelopmentCommandAction::SET_SENSOR_INPUT; return true; }
    if (std::strcmp(text, "CLEAR_SENSOR_OVERRIDE") == 0) { action = DevelopmentCommandAction::CLEAR_SENSOR_OVERRIDE; return true; }

    return false;
}


void appendEscapedJsonStringField(std::string& out, const char* value)
{
    out.push_back('"');

    for (const char* c = value; value != nullptr && *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') {
            out.push_back('\\');
        }
        out.push_back(*c);
    }

    out.push_back('"');
}


/** Appends a JSON string field, or JSON null when value is nullptr/empty — the convention this project's diagnostic payloads use for "not currently applicable" rather than fabricating a placeholder string. */
void appendOptionalJsonStringField(std::string& out, const char* value)
{
    if (value == nullptr || value[0] == '\0') {
        out += "null";
        return;
    }

    appendEscapedJsonStringField(out, value);
}


const char* ackStatusText(AckStatus status)
{
    switch (status) {
        case AckStatus::ACCEPTED: return "ACCEPTED";
        case AckStatus::APPLIED: return "APPLIED";
        case AckStatus::REJECTED: return "REJECTED";
        case AckStatus::FAILED: return "FAILED";
    }

    return "UNKNOWN";
}


void formatMacAddressText(char* buffer, std::size_t bufferSize, const Load::MacAddress& macAddress)
{
    std::snprintf(buffer, bufferSize, "%02X:%02X:%02X:%02X:%02X:%02X",
                  macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
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
      developmentCommandHandler_(nullptr),
      developmentCommandHandlerContext_(nullptr)
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


void MqttManager::setDevelopmentCommandHandler(DevelopmentCommandHandler handler, void* context)
{
    developmentCommandHandler_ = handler;
    developmentCommandHandlerContext_ = context;
}


std::string MqttManager::fullTopic(const char* topicSuffix) const
{
    std::string topic = topicNamespace_;
    topic += "/";
    topic += topicSuffix;
    return topic;
}


bool MqttManager::begin(const Credentials& credentials)
{
    if (credentials.brokerHost == nullptr || credentials.brokerHost[0] == '\0') {
        ESP_LOGE(TAG, "begin() rejected: no broker host configured (see include/KilowattsSecrets.h)");
        return false;
    }

    esp_mqtt_client_config_t config{};

    /*
     * Host and port are supplied to esp-mqtt as two discrete fields
     * (never hand-concatenated into a "scheme://host:port" string) —
     * credentials.brokerUseTls alone selects the transport, so there is
     * no URI-scheme string to get subtly wrong.
     */
    config.broker.address.hostname = credentials.brokerHost;
    config.broker.address.port = credentials.brokerPort;
    config.broker.address.transport = credentials.brokerUseTls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
    config.credentials.username = credentials.username;
    config.credentials.authentication.password = credentials.password;
    config.credentials.client_id = deviceId_.c_str();

    /*
     * Only actually consulted once MQTT_TRANSPORT_OVER_SSL is selected
     * above — without it, esp-mqtt has no CA store and TLS negotiation
     * fails. esp_crt_bundle_attach uses ESP-IDF's bundled Mozilla CA root
     * store (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE, already enabled in
     * sdkconfig.central) rather than a single pinned certificate, so it
     * verifies any standard publicly-trusted broker certificate. Harmless
     * to set when brokerUseTls is false: esp-mqtt simply never consults
     * it over a plain TCP transport.
     */
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;

    /*
     * Session persistence is deliberately left at esp-mqtt's own default
     * reconnect/backoff behaviour: MqttManager does not reimplement
     * reconnection itself, since esp-mqtt already retries with backoff
     * and simply raises MQTT_EVENT_DISCONNECTED/MQTT_EVENT_CONNECTED as
     * the underlying state changes.
     */
    client_ = esp_mqtt_client_init(&config);
    if (client_ == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init() failed");
        return false;
    }

    esp_err_t result = esp_mqtt_client_register_event(
        client_, MQTT_EVENT_ANY, &MqttManager::handleMqttEvent, this);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not register MQTT event handler: %s", esp_err_to_name(result));
        return false;
    }

    state_.store(MqttConnectionState::CONNECTING);

    result = esp_mqtt_client_start(client_);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start() failed: %s", esp_err_to_name(result));
        state_.store(MqttConnectionState::DISCONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "MQTT client starting: broker=%s://%s:%u clientId=%s",
             credentials.brokerUseTls ? "mqtts" : "mqtt", credentials.brokerHost,
             static_cast<unsigned int>(credentials.brokerPort), deviceId_.c_str());
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
        /*
         * MQTT/Wi-Fi loss must never block local control: the caller
         * (Central's Optimisation/MQTT Task) simply skips publishing this
         * cycle and keeps running Best-First Search and relay control
         * against the last valid local state.
         */
        return false;
    }

    const std::string topic = fullTopic(topicSuffix);
    const int messageId = esp_mqtt_client_publish(
        client_, topic.c_str(), payload.c_str(), static_cast<int>(payload.size()), qos, retain ? 1 : 0);

    return messageId >= 0;
}


void MqttManager::publishAcknowledgement(std::uint32_t commandId, const char* commandType, AckStatus status,
                                          const char* reason, const char* target)
{
    std::string json = "{\"schemaVersion\":" + std::to_string(schemaVersion_) +
                        ",\"commandId\":" + std::to_string(commandId) +
                        ",\"commandType\":";
    appendOptionalJsonStringField(json, commandType);
    json += ",\"status\":\"";
    json += ackStatusText(status);
    json += "\",\"reason\":";
    appendOptionalJsonStringField(json, reason);
    json += ",\"target\":";
    appendOptionalJsonStringField(json, target);
    json += "}";

    publish(TOPIC_ACKS, json, /* qos */ 1, /* retain */ false);
}


void MqttManager::publishEvent(const char* eventType, const char* target, const char* detail)
{
    std::string json = "{\"schemaVersion\":" + std::to_string(schemaVersion_) + ",\"eventType\":";
    appendOptionalJsonStringField(json, eventType);
    json += ",\"target\":";
    appendOptionalJsonStringField(json, target);
    json += ",\"detail\":";
    appendOptionalJsonStringField(json, detail);
    json += "}";

    publish(TOPIC_EVENTS, json, /* qos */ 1, /* retain */ false);
}


void MqttManager::onConnected()
{
    state_.store(MqttConnectionState::CONNECTED);
    ESP_LOGI(TAG, "MQTT connected");

    const std::string loadTopic = fullTopic(TOPIC_COMMANDS_LOAD);
    const std::string systemTopic = fullTopic(TOPIC_COMMANDS_SYSTEM);
    const std::string configTopic = fullTopic(TOPIC_COMMANDS_CONFIG);
    const std::string developmentTopic = fullTopic(TOPIC_COMMANDS_DEVELOPMENT);

    esp_mqtt_client_subscribe(client_, loadTopic.c_str(), 1);
    esp_mqtt_client_subscribe(client_, systemTopic.c_str(), 1);
    esp_mqtt_client_subscribe(client_, configTopic.c_str(), 1);
    esp_mqtt_client_subscribe(client_, developmentTopic.c_str(), 1);
}


void MqttManager::onDisconnected()
{
    state_.store(MqttConnectionState::DISCONNECTED);
    ESP_LOGW(TAG, "MQTT disconnected; local control continues using the last valid configuration");
}


void MqttManager::onDataReceived(const char* topic, std::size_t topicLength, const char* data, std::size_t dataLength)
{
    const std::string receivedTopic(topic, topicLength);

    if (receivedTopic == fullTopic(TOPIC_COMMANDS_LOAD)) {
        handleLoadCommandMessage(data, dataLength);
    } else if (receivedTopic == fullTopic(TOPIC_COMMANDS_SYSTEM)) {
        handleSystemCommandMessage(data, dataLength);
    } else if (receivedTopic == fullTopic(TOPIC_COMMANDS_CONFIG)) {
        handleConfigCommandMessage(data, dataLength);
    } else if (receivedTopic == fullTopic(TOPIC_COMMANDS_DEVELOPMENT)) {
        handleDevelopmentCommandMessage(data, dataLength);
    } else {
        ESP_LOGW(TAG, "Received data on an unrecognised topic: %s", receivedTopic.c_str());
    }
}


void MqttManager::handleLoadCommandMessage(const char* data, std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Rejected commands/load message: malformed JSON");
        publishAcknowledgement(0U, "LOAD", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    LoadCommandRequest request{};
    const cJSON* commandIdField = cJSON_GetObjectItemCaseSensitive(root, "commandId");
    if (!isUnsignedIntJsonNumber(commandIdField, request.commandId)) {
        ESP_LOGW(TAG, "Rejected commands/load message: missing/invalid commandId");
        publishAcknowledgement(0U, "LOAD", AckStatus::REJECTED, "missing or invalid commandId", nullptr);
        cJSON_Delete(root);
        return;
    }
    const std::uint32_t commandId = request.commandId;

    const cJSON* nodeMacField = cJSON_GetObjectItemCaseSensitive(root, "nodeMac");
    const cJSON* relayPinField = cJSON_GetObjectItemCaseSensitive(root, "relayPin");

    if (!cJSON_IsString(nodeMacField) || !parseMacAddressText(nodeMacField->valuestring, request.nodeMacAddress) ||
        !isUnsignedByteJsonNumber(relayPinField, request.relayPin)) {
        ESP_LOGW(TAG, "Rejected commands/load message commandId=%u: missing/invalid nodeMac or relayPin",
                 static_cast<unsigned int>(commandId));
        publishAcknowledgement(commandId, "LOAD", AckStatus::REJECTED, "missing or invalid nodeMac/relayPin", nullptr);
        cJSON_Delete(root);
        return;
    }
    char targetText[18] = {};
    formatMacAddressText(targetText, sizeof(targetText), request.nodeMacAddress);

    const cJSON* priorityField = cJSON_GetObjectItemCaseSensitive(root, "priority");
    if (priorityField != nullptr) {
        if (!isUnsignedShortJsonNumber(priorityField, request.priority)) {
            publishAcknowledgement(commandId, "LOAD", AckStatus::REJECTED, "invalid priority", targetText);
            cJSON_Delete(root);
            return;
        }
        request.hasPriority = true;
    }

    const cJSON* modeField = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (cJSON_IsString(modeField)) {
        LoadMode::Value parsedMode{};
        if (parseLoadModeText(modeField->valuestring, parsedMode)) {
            request.hasMode = true;
            request.mode = parsedMode;
        } else {
            ESP_LOGW(TAG, "Rejected commands/load message commandId=%u: invalid mode '%s'",
                     static_cast<unsigned int>(commandId), modeField->valuestring);
            publishAcknowledgement(commandId, "LOAD", AckStatus::REJECTED, "invalid mode", targetText);
            cJSON_Delete(root);
            return;
        }
    }

    const cJSON* scheduleField = cJSON_GetObjectItemCaseSensitive(root, "schedule");
    if (cJSON_IsObject(scheduleField)) {
        const cJSON* enabledField = cJSON_GetObjectItemCaseSensitive(scheduleField, "enabled");
        const cJSON* hourField = cJSON_GetObjectItemCaseSensitive(scheduleField, "hour");
        const cJSON* minuteField = cJSON_GetObjectItemCaseSensitive(scheduleField, "minute");

        std::uint8_t hour = 0U;
        std::uint8_t minute = 0U;
        if (!cJSON_IsBool(enabledField) ||
            !isUnsignedByteJsonNumber(hourField, hour) ||
            !isUnsignedByteJsonNumber(minuteField, minute)) {
            publishAcknowledgement(commandId, "LOAD", AckStatus::REJECTED,
                                   "invalid schedule", targetText);
            cJSON_Delete(root);
            return;
        }
        const bool enabled = cJSON_IsTrue(enabledField);

        if (enabled && (hour > 23U || minute > 59U)) {
            ESP_LOGW(TAG, "Rejected commands/load message commandId=%u: schedule hour/minute out of range",
                     static_cast<unsigned int>(commandId));
            publishAcknowledgement(commandId, "LOAD", AckStatus::REJECTED, "schedule hour/minute out of range", targetText);
            cJSON_Delete(root);
            return;
        }

        request.hasSchedule = true;
        request.schedule = AutoSchedule{enabled, hour, minute};
    }

    cJSON_Delete(root);

    if (loadCommandHandler_ == nullptr) {
        ESP_LOGE(TAG, "Rejected commands/load message commandId=%u: no handler registered",
                 static_cast<unsigned int>(commandId));
        publishAcknowledgement(commandId, "LOAD", AckStatus::REJECTED, "no handler registered", targetText);
        return;
    }

    const LoadCommandResult result = loadCommandHandler_(loadCommandHandlerContext_, request);
    publishAcknowledgement(commandId, "LOAD", result.accepted ? AckStatus::APPLIED : AckStatus::REJECTED,
                            result.reason, targetText);
}


void MqttManager::handleSystemCommandMessage(const char* data, std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        publishAcknowledgement(0U, "SYSTEM", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    SystemCommandRequest request{};

    const cJSON* commandIdField = cJSON_GetObjectItemCaseSensitive(root, "commandId");
    if (!isUnsignedIntJsonNumber(commandIdField, request.commandId)) {
        cJSON_Delete(root);
        publishAcknowledgement(0U, "SYSTEM", AckStatus::REJECTED, "missing or invalid commandId", nullptr);
        return;
    }

    const cJSON* actionField = cJSON_GetObjectItemCaseSensitive(root, "action");
    request.action = SystemCommandAction::UNKNOWN;
    if (cJSON_IsString(actionField)) {
        parseSystemCommandActionText(actionField->valuestring, request.action);
    }

    const cJSON* confirmField = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    if (cJSON_IsString(confirmField)) {
        std::size_t i = 0U;
        for (; i < sizeof(request.confirmText) - 1U && confirmField->valuestring[i] != '\0'; ++i) {
            request.confirmText[i] = confirmField->valuestring[i];
        }
        request.confirmText[i] = '\0';
    }

    const cJSON* targetNodeMacField = cJSON_GetObjectItemCaseSensitive(root, "targetNodeMac");
    if (cJSON_IsString(targetNodeMacField) &&
        parseMacAddressText(targetNodeMacField->valuestring, request.targetNodeMacAddress)) {
        request.hasTargetNodeMacAddress = true;
    }

    if (request.action == SystemCommandAction::APPLY_SAFETY_CONFIG) {
        const cJSON* safetyConfig = cJSON_GetObjectItemCaseSensitive(root, "safetyConfig");
        if (!cJSON_IsObject(safetyConfig) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(safetyConfig, "minimumStateOfChargePercent"),
                                request.minimumStateOfChargePercent) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(safetyConfig, "warningStateOfChargePercent"),
                                request.warningStateOfChargePercent) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(safetyConfig, "targetRuntimeHours"),
                                request.targetRuntimeHours) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(safetyConfig, "safetyFactor"),
                                request.safetyFactor) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(safetyConfig, "maximumBatteryDischargeCurrentAmps"),
                                request.maximumBatteryDischargeCurrentAmps) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(safetyConfig, "maximumMainCurrentAmps"),
                                request.maximumMainCurrentAmps)) {
            cJSON_Delete(root);
            publishAcknowledgement(request.commandId, "SYSTEM", AckStatus::REJECTED,
                                   "invalid safetyConfig", nullptr);
            return;
        }
        request.hasSafetyPolicy = true;
    }

    cJSON_Delete(root);

    if (request.action == SystemCommandAction::UNKNOWN) {
        publishAcknowledgement(request.commandId, "SYSTEM", AckStatus::REJECTED, "unrecognised action", nullptr);
        return;
    }

    if (systemCommandHandler_ == nullptr) {
        publishAcknowledgement(request.commandId, "SYSTEM", AckStatus::REJECTED, "no handler registered", nullptr);
        return;
    }

    const LoadCommandResult result = systemCommandHandler_(systemCommandHandlerContext_, request);
    publishAcknowledgement(request.commandId, "SYSTEM", result.accepted ? AckStatus::APPLIED : AckStatus::REJECTED,
                            result.reason, nullptr);
}


void MqttManager::handleConfigCommandMessage(const char* data, std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Rejected commands/config message: malformed JSON");
        publishAcknowledgement(0U, "CONFIG", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    ConfigCommandRequest request{};
    const cJSON* commandIdField = cJSON_GetObjectItemCaseSensitive(root, "commandId");
    if (!isUnsignedIntJsonNumber(commandIdField, request.commandId)) {
        ESP_LOGW(TAG, "Rejected commands/config message: missing/invalid commandId");
        publishAcknowledgement(0U, "CONFIG", AckStatus::REJECTED, "missing or invalid commandId", nullptr);
        cJSON_Delete(root);
        return;
    }
    const std::uint32_t commandId = request.commandId;

    const cJSON* actionField = cJSON_GetObjectItemCaseSensitive(root, "action");
    ConfigCommandAction parsedAction = ConfigCommandAction::UNKNOWN;
    if (!cJSON_IsString(actionField) || !parseConfigCommandActionText(actionField->valuestring, parsedAction)) {
        ESP_LOGW(TAG, "Rejected commands/config message commandId=%u: missing/unrecognised action",
                 static_cast<unsigned int>(commandId));
        publishAcknowledgement(commandId, "CONFIG", AckStatus::REJECTED, "missing or unrecognised action", nullptr);
        cJSON_Delete(root);
        return;
    }
    request.action = parsedAction;

    const cJSON* nodeMacField = cJSON_GetObjectItemCaseSensitive(root, "nodeMac");
    if (!cJSON_IsString(nodeMacField) || !parseMacAddressText(nodeMacField->valuestring, request.nodeMacAddress)) {
        ESP_LOGW(TAG, "Rejected commands/config message commandId=%u: missing/invalid nodeMac",
                 static_cast<unsigned int>(commandId));
        publishAcknowledgement(commandId, "CONFIG", AckStatus::REJECTED, "missing or invalid nodeMac", nullptr);
        cJSON_Delete(root);
        return;
    }

    char targetText[18] = {};
    formatMacAddressText(targetText, sizeof(targetText), request.nodeMacAddress);

    /*
     * Copied out of the cJSON tree now, rather than kept as a pointer into
     * it (actionField->valuestring) - every use below happens after
     * cJSON_Delete(root), which would otherwise leave commandTypeText
     * dangling.
     */
    char commandTypeText[32] = {};
    {
        std::size_t i = 0U;
        for (; i < sizeof(commandTypeText) - 1U && actionField->valuestring[i] != '\0'; ++i) {
            commandTypeText[i] = actionField->valuestring[i];
        }
        commandTypeText[i] = '\0';
    }

    if (request.action == ConfigCommandAction::COMMISSION_NODE || request.action == ConfigCommandAction::RENAME_NODE) {
        const cJSON* friendlyNameField = cJSON_GetObjectItemCaseSensitive(root, "friendlyName");
        if (!cJSON_IsString(friendlyNameField) || friendlyNameField->valuestring[0] == '\0') {
            ESP_LOGW(TAG, "Rejected commands/config message commandId=%u: missing/empty friendlyName",
                     static_cast<unsigned int>(commandId));
            publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED,
                                    "missing or empty friendlyName", targetText);
            cJSON_Delete(root);
            return;
        }

        request.hasFriendlyName = true;
        std::size_t i = 0U;
        for (; i < sizeof(request.friendlyName) - 1U && friendlyNameField->valuestring[i] != '\0'; ++i) {
            request.friendlyName[i] = friendlyNameField->valuestring[i];
        }
        request.friendlyName[i] = '\0';
    }

    if (request.action == ConfigCommandAction::CONFIGURE_LOAD) {
        const cJSON* loadField = cJSON_GetObjectItemCaseSensitive(root, "load");
        if (!cJSON_IsObject(loadField)) {
            publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED,
                                    "load object required", targetText);
            cJSON_Delete(root);
            return;
        }

        const cJSON* nameField = cJSON_GetObjectItemCaseSensitive(loadField, "name");
        const cJSON* relayPinField = cJSON_GetObjectItemCaseSensitive(loadField, "relayPin");
        const cJSON* relayActiveHighField = cJSON_GetObjectItemCaseSensitive(loadField, "relayActiveHigh");
        const cJSON* modeField = cJSON_GetObjectItemCaseSensitive(loadField, "mode");
        const cJSON* priorityField = cJSON_GetObjectItemCaseSensitive(loadField, "priority");
        const cJSON* nominalVoltageField = cJSON_GetObjectItemCaseSensitive(loadField, "nominalVoltageVolts");
        const cJSON* nominalCurrentField = cJSON_GetObjectItemCaseSensitive(loadField, "nominalCurrentAmps");
        const cJSON* branchMaximumField = cJSON_GetObjectItemCaseSensitive(loadField, "branchMaximumCurrentAmps");
        const cJSON* startupWattsField = cJSON_GetObjectItemCaseSensitive(loadField, "startupWatts");
        const cJSON* scheduleField = cJSON_GetObjectItemCaseSensitive(loadField, "schedule");

        if (!cJSON_IsString(nameField) || nameField->valuestring[0] == '\0' ||
            std::strlen(nameField->valuestring) >= sizeof(request.loadName) ||
            !isUnsignedByteJsonNumber(relayPinField, request.relayPin) ||
            !cJSON_IsBool(relayActiveHighField) ||
            !cJSON_IsString(modeField) || !parseLoadModeText(modeField->valuestring, request.mode) ||
            !isUnsignedShortJsonNumber(priorityField, request.priority) ||
            !isFiniteJsonNumber(nominalVoltageField, request.nominalVoltageVolts) ||
            !isFiniteJsonNumber(nominalCurrentField, request.nominalCurrentAmps) ||
            !isFiniteJsonNumber(branchMaximumField, request.branchMaximumCurrentAmps) ||
            !isFiniteJsonNumber(startupWattsField, request.startupWatts) ||
            !cJSON_IsObject(scheduleField)) {
            publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED,
                                    "invalid load configuration", targetText);
            cJSON_Delete(root);
            return;
        }

        const float plannedRunningWatts = request.nominalVoltageVolts * request.nominalCurrentAmps;
        if (request.nominalVoltageVolts <= 0.0F || request.nominalCurrentAmps <= 0.0F ||
            request.branchMaximumCurrentAmps <= 0.0F || !std::isfinite(plannedRunningWatts) ||
            plannedRunningWatts <= 0.0F || request.startupWatts < plannedRunningWatts) {
            publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED,
                                    "invalid nominal load rating", targetText);
            cJSON_Delete(root);
            return;
        }

        const cJSON* scheduleEnabledField = cJSON_GetObjectItemCaseSensitive(scheduleField, "enabled");
        const cJSON* scheduleHourField = cJSON_GetObjectItemCaseSensitive(scheduleField, "hour");
        const cJSON* scheduleMinuteField = cJSON_GetObjectItemCaseSensitive(scheduleField, "minute");
        std::uint8_t hour = 0U;
        std::uint8_t minute = 0U;
        if (!cJSON_IsBool(scheduleEnabledField) ||
            !isUnsignedByteJsonNumber(scheduleHourField, hour) ||
            !isUnsignedByteJsonNumber(scheduleMinuteField, minute) ||
            (cJSON_IsTrue(scheduleEnabledField) && (hour > 23U || minute > 59U))) {
            publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED,
                                    "invalid load schedule", targetText);
            cJSON_Delete(root);
            return;
        }

        std::snprintf(request.loadName, sizeof(request.loadName), "%s", nameField->valuestring);
        request.relayActiveHigh = cJSON_IsTrue(relayActiveHighField);
        const bool scheduleEnabled = cJSON_IsTrue(scheduleEnabledField);
        request.schedule = AutoSchedule{scheduleEnabled, hour, minute};
        request.hasLoadConfiguration = true;
    }

    if (request.action == ConfigCommandAction::CONFIGURE_BATTERY_SENSOR) {
        const cJSON* batteryField = cJSON_GetObjectItemCaseSensitive(root, "batterySensor");
        if (!cJSON_IsObject(batteryField) ||
            !isUnsignedByteJsonNumber(cJSON_GetObjectItemCaseSensitive(batteryField, "i2cAddress"),
                                      request.batteryI2cAddress) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(batteryField, "shuntResistanceOhms"),
                                request.batteryShuntResistanceOhms) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(batteryField, "maximumExpectedCurrentAmps"),
                                request.batteryMaximumExpectedCurrentAmps) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(batteryField, "emaAlpha"),
                                request.batteryEmaAlpha) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(batteryField, "batteryCapacityAmpHours"),
                                request.batteryCapacityAmpHours) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(batteryField, "initialStateOfChargePercent"),
                                request.batteryInitialStateOfChargePercent) ||
            !isFiniteJsonNumber(cJSON_GetObjectItemCaseSensitive(batteryField, "nominalVoltageVolts"),
                                request.batteryNominalVoltageVolts)) {
            publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED,
                                    "invalid batterySensor configuration", targetText);
            cJSON_Delete(root);
            return;
        }
        request.hasBatterySensorConfiguration = true;
    }

    cJSON_Delete(root);

    if (configCommandHandler_ == nullptr) {
        ESP_LOGE(TAG, "Rejected commands/config message commandId=%u: no handler registered",
                 static_cast<unsigned int>(commandId));
        publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED, "no handler registered", targetText);
        return;
    }

    const LoadCommandResult result = configCommandHandler_(configCommandHandlerContext_, request);
    /*
     * Battery commissioning and Central's authoritative decommission state
     * are complete once their local NVS transactions succeed. Smart-node
     * load and identity operations still have a separate ESP-NOW final ACK.
     */
    const bool completesLocally =
        request.action == ConfigCommandAction::CONFIGURE_BATTERY_SENSOR ||
        request.action == ConfigCommandAction::DECOMMISSION_NODE;
    publishAcknowledgement(commandId, commandTypeText,
                            result.accepted ? (completesLocally ? AckStatus::APPLIED : AckStatus::ACCEPTED)
                                            : AckStatus::REJECTED,
                            result.reason, targetText);
}


void MqttManager::handleDevelopmentCommandMessage(const char* data, std::size_t dataLength)
{
    cJSON* root = cJSON_ParseWithLength(data, dataLength);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Rejected commands/development message: malformed JSON");
        publishAcknowledgement(0U, "DEVELOPMENT", AckStatus::REJECTED, "malformed JSON", nullptr);
        return;
    }

    DevelopmentCommandRequest request{};
    const cJSON* commandIdField = cJSON_GetObjectItemCaseSensitive(root, "commandId");
    if (!isUnsignedIntJsonNumber(commandIdField, request.commandId)) {
        ESP_LOGW(TAG, "Rejected commands/development message: missing/invalid commandId");
        publishAcknowledgement(0U, "DEVELOPMENT", AckStatus::REJECTED, "missing or invalid commandId", nullptr);
        cJSON_Delete(root);
        return;
    }
    const std::uint32_t commandId = request.commandId;

    const cJSON* actionField = cJSON_GetObjectItemCaseSensitive(root, "action");
    DevelopmentCommandAction parsedAction = DevelopmentCommandAction::UNKNOWN;
    if (!cJSON_IsString(actionField) || !parseDevelopmentCommandActionText(actionField->valuestring, parsedAction)) {
        ESP_LOGW(TAG, "Rejected commands/development message commandId=%u: missing/unrecognised action",
                 static_cast<unsigned int>(commandId));
        publishAcknowledgement(commandId, "DEVELOPMENT", AckStatus::REJECTED, "missing or unrecognised action", nullptr);
        cJSON_Delete(root);
        return;
    }
    request.action = parsedAction;

    const cJSON* nodeMacField = cJSON_GetObjectItemCaseSensitive(root, "nodeMac");
    if (!cJSON_IsString(nodeMacField) || !parseMacAddressText(nodeMacField->valuestring, request.targetNodeMacAddress)) {
        ESP_LOGW(TAG, "Rejected commands/development message commandId=%u: missing/invalid nodeMac",
                 static_cast<unsigned int>(commandId));
        publishAcknowledgement(commandId, "DEVELOPMENT", AckStatus::REJECTED, "missing or invalid nodeMac", nullptr);
        cJSON_Delete(root);
        return;
    }

    char targetText[18] = {};
    formatMacAddressText(targetText, sizeof(targetText), request.targetNodeMacAddress);

    char commandTypeText[24] = {};
    {
        std::size_t i = 0U;
        for (; i < sizeof(commandTypeText) - 1U && actionField->valuestring[i] != '\0'; ++i) {
            commandTypeText[i] = actionField->valuestring[i];
        }
        commandTypeText[i] = '\0';
    }

    if (request.action == DevelopmentCommandAction::SET_SENSOR_INPUT ||
        request.action == DevelopmentCommandAction::CLEAR_SENSOR_OVERRIDE) {
        const cJSON* i2cAddressField = cJSON_GetObjectItemCaseSensitive(root, "i2cAddress");
        if (!isUnsignedByteJsonNumber(i2cAddressField, request.i2cAddress)) {
            ESP_LOGW(TAG, "Rejected commands/development message commandId=%u: missing i2cAddress",
                     static_cast<unsigned int>(commandId));
            publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED, "missing i2cAddress", targetText);
            cJSON_Delete(root);
            return;
        }
        request.hasSensorInput = true;
        if (request.action == DevelopmentCommandAction::SET_SENSOR_INPUT) {
            const cJSON* voltageField = cJSON_GetObjectItemCaseSensitive(root, "voltageVolts");
            const cJSON* currentField = cJSON_GetObjectItemCaseSensitive(root, "currentAmps");
            if (!isFiniteJsonNumber(voltageField, request.voltageVolts) ||
                !isFiniteJsonNumber(currentField, request.currentAmps)) {
                ESP_LOGW(TAG, "Rejected commands/development message commandId=%u: missing voltageVolts/currentAmps",
                         static_cast<unsigned int>(commandId));
                publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED,
                                        "missing voltageVolts/currentAmps", targetText);
                cJSON_Delete(root);
                return;
            }
        }
    }

    cJSON_Delete(root);

    if (developmentCommandHandler_ == nullptr) {
        ESP_LOGE(TAG, "Rejected commands/development message commandId=%u: no handler registered",
                 static_cast<unsigned int>(commandId));
        publishAcknowledgement(commandId, commandTypeText, AckStatus::REJECTED, "no handler registered", targetText);
        return;
    }

    const LoadCommandResult result = developmentCommandHandler_(developmentCommandHandlerContext_, request);
    publishAcknowledgement(commandId, commandTypeText, result.accepted ? AckStatus::ACCEPTED : AckStatus::REJECTED,
                            result.reason, targetText);
}


void MqttManager::printDiagnosticReport() const
{
    const char* stateText = "UNKNOWN";
    switch (state_.load()) {
        case MqttConnectionState::DISCONNECTED: stateText = "DISCONNECTED"; break;
        case MqttConnectionState::CONNECTING: stateText = "CONNECTING"; break;
        case MqttConnectionState::CONNECTED: stateText = "CONNECTED"; break;
    }

    ESP_LOGI(TAG, "================ MQTT MANAGER DIAGNOSTIC ================");
    ESP_LOGI(TAG, "Namespace : %s", topicNamespace_.c_str());
    ESP_LOGI(TAG, "Device ID : %s", deviceId_.c_str());
    ESP_LOGI(TAG, "State     : %s", stateText);
    ESP_LOGI(TAG, "==========================================================");
}


void MqttManager::handleMqttEvent(void* handlerArgs, esp_event_base_t base, std::int32_t eventId, void* eventData)
{
    (void)base;

    MqttManager* self = static_cast<MqttManager*>(handlerArgs);
    if (self == nullptr) {
        return;
    }

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
                self->onDataReceived(event->topic, static_cast<std::size_t>(event->topic_len),
                                      event->data, static_cast<std::size_t>(event->data_len));
            }
            break;
        default:
            break;
    }
}


} // namespace kilowatts
