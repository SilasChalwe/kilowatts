#ifndef KILOWATTS_SMART_RUNTIME_H
#define KILOWATTS_SMART_RUNTIME_H

#include "ChipInfo.h"
#include "CommissioningPackets.h"
#include "CurrentTimeProvider.h"
#include "EspNowCommunication.h"
#include "FirmwareVersion.h"
#include "HardwareConfigurationPackets.h"
#include "Load.h"
#include "Node.h"
#include "NodeIdentityStore.h"
#include "NodeLifecycle.h"
#include "NodeLoadHardwareStore.h"
#include "NodeReportPackets.h"
#include "SmartConsole.h"
#include "RadioConfig.h"
#include "RelayController.h"
#include "SmartNodeConfig.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

using namespace kilowatts;

static const char* TAG = "SMART_NODE";

namespace {

constexpr std::uint32_t NODE_REPORT_PERIOD_MS = 2000U;
constexpr std::uint32_t WATCHDOG_PERIOD_MS = 60000U;
constexpr std::uint32_t DISCOVERY_RETRY_PERIOD_MS = 2000U;
constexpr std::uint32_t DISCOVERY_WINDOW_MS = 500U;

EspNowCommunication communication(kilowatts::KILOWATTS_RADIO_CHANNEL);
CurrentTimeProvider currentTimeProvider;
RelayController relays;
ChipInfo chipInfo;
NodeIdentityStore identityStore;
NodeLoadHardwareStore smartNodeConfigurationStore;
SmartConsole smartConsole;

Node* thisSmartNode = nullptr;
SemaphoreHandle_t nodeMutex = nullptr;
SemaphoreHandle_t identityMutex = nullptr;
QueueHandle_t relayCommandQueue = nullptr;

struct RelayCommandQueueItem {
    RelayCommandPacket command;
};

bool isCommissioned()
{
    const NodeLifecycleState state = identityStore.getLifecycleState();
    return state == NodeLifecycleState::COMMISSIONED || state == NodeLifecycleState::OPERATIONAL;
}

void computeAutomaticNodeName(
    const EspNowCommunication::MacAddress& mac,
    char* buffer,
    std::size_t bufferSize)
{
    std::snprintf(buffer, bufferSize, "Smart-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void sendIdentityReport()
{
    IdentityReportPacket packet{};
    packet.role = static_cast<std::uint8_t>(NodeRole::SMART);
    packet.lifecycleState = static_cast<std::uint8_t>(identityStore.getLifecycleState());
    std::snprintf(packet.firmwareVersion, sizeof(packet.firmwareVersion), "%s", KILOWATTS_FIRMWARE_VERSION);
    chipInfo.getChipModelText(packet.chipModel, sizeof(packet.chipModel));

    packet.relayCapabilityCount = 0U;

    packet.freeHeapBytes = chipInfo.getFreeHeapBytes();
    packet.minFreeHeapBytes = chipInfo.getMinFreeHeapBytes();
    packet.flashSizeBytes = chipInfo.getFlashSizeBytes();
    packet.psramSizeBytes = chipInfo.getPsramSizeBytes();
    packet.siliconRevision = static_cast<std::uint16_t>(chipInfo.getSiliconRevision());
    packet.cpuCores = static_cast<std::uint8_t>(chipInfo.getCpuCores());
    packet.cpuFrequencyMhz = chipInfo.getCpuFrequencyMhz();
    packet.temperatureAvailable = chipInfo.getTemperatureCelsius(packet.temperatureCelsius) ? 1U : 0U;
    chipInfo.getResetReasonText(packet.resetReason, sizeof(packet.resetReason));

    communication.sendToCentral(EspNowCommunication::MessageType::IDENTITY_REPORT, packet);
}

/**
 * Builds page `pageIndex` of `totalPages` for this report cycle. A Node's
 * loads are chunked MAX_LOADS_PER_NODE_PACKET at a time — one ESP-NOW
 * packet cannot fit all MAX_LOADS_PER_NODE loads at once (see the
 * NodeReportPacket size static_assert in NodeReportPackets.h) — and
 * CentralNodeRegistry::applyNodeReport() accumulates every page of a
 * sequence before trusting the load set it describes.
 */
NodeReportPacket buildNodeReportPacket(
    const EspNowCommunication::MacAddress& localMac,
    std::uint16_t sequence,
    std::uint8_t pageIndex,
    std::uint8_t totalPages)
{
    NodeReportPacket packet{};
    std::snprintf(packet.nodeName, sizeof(packet.nodeName), "%s", communication.getLocalNodeName());
    packet.nodeMacAddress = localMac;
    packet.upstreamNodeMacAddress = communication.hasUpstreamNode()
        ? communication.getUpstreamNodeMacAddress() : localMac;
    packet.hopCountToCentral = communication.getHopCountToCentral();
    packet.reportSequenceId = sequence;
    packet.pageIndex = pageIndex;
    packet.totalPages = totalPages;

    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(200U)) != pdTRUE || thisSmartNode == nullptr) {
        return packet;
    }

    const std::size_t totalLoads =
        std::min<std::size_t>(thisSmartNode->getNumberOfLoads(), MAX_LOADS_PER_NODE);
    const std::size_t startIndex =
        static_cast<std::size_t>(pageIndex) * MAX_LOADS_PER_NODE_PACKET;
    const std::size_t endIndex =
        std::min(startIndex + MAX_LOADS_PER_NODE_PACKET, totalLoads);

    packet.numberOfLoads = static_cast<std::uint8_t>(
        startIndex < endIndex ? (endIndex - startIndex) : 0U);

    for (std::size_t i = 0U; i < packet.numberOfLoads; ++i) {
        const Load* load = thisSmartNode->getLoad(startIndex + i);
        if (load == nullptr) continue;

        LoadReportPacket& out = packet.loads[i];
        std::snprintf(out.name, sizeof(out.name), "%s", load->getName().c_str());
        out.relayPin = load->getRelayPin();
        out.mode = static_cast<std::uint8_t>(load->getMode());
        out.powerType = static_cast<std::uint8_t>(load->getPowerType());
        out.priority = load->getPriority();
        out.powerRatingWatts = load->getPowerRatingWatts();

        const AutoSchedule schedule = load->getSchedule();
        out.scheduleEnabled = schedule.enabled ? 1U : 0U;
        out.scheduleStartHour = schedule.startHour;
        out.scheduleStartMinute = schedule.startMinute;
        out.scheduleEndHour = schedule.endHour;
        out.scheduleEndMinute = schedule.endMinute;
    }

    xSemaphoreGive(nodeMutex);
    return packet;
}

/** Number of MAX_LOADS_PER_NODE_PACKET-sized pages needed to report loadCount loads. */
std::uint8_t computeReportPageCount(std::size_t loadCount)
{
    const std::size_t bounded = std::min<std::size_t>(loadCount, MAX_LOADS_PER_NODE);
    const std::size_t pages =
        (bounded + MAX_LOADS_PER_NODE_PACKET - 1U) / MAX_LOADS_PER_NODE_PACKET;
    return static_cast<std::uint8_t>(pages == 0U ? 1U : pages);
}

HardwareConfigurationFailureReason applyConfigureLoadCommand(const ConfigureLoadCommandPacket& command)
{
    if (!isCommissioned()) return HardwareConfigurationFailureReason::NODE_NOT_COMMISSIONED;

    NodeLoadHardwareStore::LoadConfiguration configuration{};
    std::snprintf(configuration.name, sizeof(configuration.name), "%s", command.loadName);
    configuration.relayPin = command.relayPin;
    configuration.relayActiveHigh = command.relayActiveHigh != 0U;
    configuration.powerRatingWatts = command.powerRatingWatts;
    configuration.powerType = static_cast<LoadPowerType>(command.powerType);
    configuration.mode = static_cast<LoadMode::Value>(command.mode);
    configuration.priority = command.priority;
    configuration.schedule = AutoSchedule{
        command.scheduleEnabled != 0U,
        command.scheduleStartHour,
        command.scheduleStartMinute,
        command.scheduleEndHour,
        command.scheduleEndMinute};

    HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
    }
    smartNodeConfigurationStore.configureNewLoad(configuration, relays, *thisSmartNode, reason);
    xSemaphoreGive(nodeMutex);
    return reason;
}

HardwareConfigurationFailureReason applyRemoveLoadCommand(const RemoveLoadCommandPacket& command)
{
    if (!isCommissioned()) return HardwareConfigurationFailureReason::NODE_NOT_COMMISSIONED;

    HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
    }
    smartNodeConfigurationStore.removeLoad(command.relayPin, relays, *thisSmartNode, reason);
    xSemaphoreGive(nodeMutex);
    return reason;
}


/* ------------------------------------------------------------------------- */
/* Local console                                                             */
/* ------------------------------------------------------------------------- */

const char* hardwareConfigurationFailureText(HardwareConfigurationFailureReason reason)
{
    switch (reason) {
        case HardwareConfigurationFailureReason::NONE: return "applied";
        case HardwareConfigurationFailureReason::NODE_NOT_COMMISSIONED: return "Node is not commissioned";
        case HardwareConfigurationFailureReason::UNSUPPORTED_RELAY_PIN: return "relay pin is not supported";
        case HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN: return "relay pin is already configured";
        case HardwareConfigurationFailureReason::INVALID_POWER_RATING: return "invalid Load power rating";
        case HardwareConfigurationFailureReason::INVALID_CONFIGURATION: return "invalid Load configuration";
        case HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED: return "relay hardware initialization failed";
        case HardwareConfigurationFailureReason::PERSISTENCE_FAILED: return "Node could not persist configuration";
        case HardwareConfigurationFailureReason::CAPACITY_REACHED: return "Node configuration capacity reached";
    }
    return "unknown hardware configuration failure";
}

const char* consoleLoadModeText(LoadMode::Value mode)
{
    switch (mode) {
        case LoadMode::Value::FIXED_OFF: return "FIXED_OFF";
        case LoadMode::Value::FIXED_ON:  return "FIXED_ON";
        case LoadMode::Value::AUTO_OFF:  return "AUTO_OFF";
        case LoadMode::Value::AUTO_ON:   return "AUTO_ON";
    }
    return "UNKNOWN";
}

const char* consoleLoadPowerTypeText(LoadPowerType powerType)
{
    switch (powerType) {
        case LoadPowerType::AC: return "AC";
        case LoadPowerType::DC: return "DC";
    }
    return "UNKNOWN";
}

CommandResult smartCommandResult(bool accepted, bool completed, const char* reason)
{
    CommandResult result{};
    result.accepted = accepted;
    result.completed = completed;
    std::snprintf(result.reason, sizeof(result.reason), "%s", reason != nullptr ? reason : "");
    return result;
}

void consoleSmartStatus(void*)
{
    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::printf("STATUS: BUSY\n");
        return;
    }

    std::printf("SMART NODE STATUS\n");
    std::printf("Commissioned : %s\n", isCommissioned() ? "YES" : "NO");
    std::printf("Loads        : %u\n",
                static_cast<unsigned int>(
                    thisSmartNode != nullptr ? thisSmartNode->getNumberOfLoads() : 0U));
    std::printf("Radio channel: %u\n", static_cast<unsigned int>(communication.getChannel()));
    std::printf("Upstream hop : %u\n", static_cast<unsigned int>(communication.getHopCountToCentral()));

    xSemaphoreGive(nodeMutex);
}

void consoleSmartLoads(void*)
{
    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::printf("LOADS: BUSY\n");
        return;
    }

    std::printf("LOADS\n");
    std::size_t count = 0U;
    if (thisSmartNode != nullptr) {
        for (std::size_t i = 0U; i < thisSmartNode->getNumberOfLoads(); ++i) {
            const Load* load = thisSmartNode->getLoad(i);
            if (load == nullptr) continue;
            ++count;
            std::printf("pin=%u | %-16s | %7.2f W | priority=%u | %-9s | %s\n",
                        static_cast<unsigned int>(load->getRelayPin()),
                        load->getName().c_str(),
                        static_cast<double>(load->getPowerRatingWatts()),
                        static_cast<unsigned int>(load->getPriority()),
                        consoleLoadModeText(load->getMode()),
                        consoleLoadPowerTypeText(load->getPowerType()));
        }
    }
    if (count == 0U) {
        std::printf("None\n");
    }

    xSemaphoreGive(nodeMutex);
}

void consoleSmartLoadStatus(void*, std::uint8_t relayPin)
{
    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::printf("LOAD: BUSY\n");
        return;
    }

    const Load* load = thisSmartNode != nullptr ? thisSmartNode->getLoadByRelayPin(relayPin) : nullptr;
    if (load == nullptr) {
        std::printf("Load not found\n");
        xSemaphoreGive(nodeMutex);
        return;
    }

    const AutoSchedule schedule = load->getSchedule();
    std::printf("LOAD STATUS\n");
    std::printf("Relay pin      : %u\n", static_cast<unsigned int>(relayPin));
    std::printf("Name           : %s\n", load->getName().c_str());
    std::printf("Power rating   : %.2f W\n", static_cast<double>(load->getPowerRatingWatts()));
    std::printf("Priority       : %u\n", static_cast<unsigned int>(load->getPriority()));
    std::printf("Power type     : %s\n", consoleLoadPowerTypeText(load->getPowerType()));
    std::printf("Mode           : %s\n", consoleLoadModeText(load->getMode()));
    if (schedule.enabled) {
        std::printf("AUTO schedule  : %02u:%02u-%02u:%02u\n",
                    static_cast<unsigned int>(schedule.startHour),
                    static_cast<unsigned int>(schedule.startMinute),
                    static_cast<unsigned int>(schedule.endHour),
                    static_cast<unsigned int>(schedule.endMinute));
    } else {
        std::printf("AUTO schedule  : none\n");
    }

    for (std::size_t i = 0U; i < relays.getNumberOfRelays(); ++i) {
        const RelayController::RelayConfiguration* config = relays.getRelay(i);
        if (config != nullptr && config->relayPin == relayPin) {
            std::printf("Relay polarity : %s\n", config->activeHigh ? "active-HIGH" : "active-LOW");
            std::printf("Relay applied  : %s\n", relays.isHardwareApplied(relayPin) ? "yes" : "no");
            break;
        }
    }

    xSemaphoreGive(nodeMutex);
}

CommandResult consoleSmartConfigureLoad(void*, const SmartLoadConfigurationRequest& request)
{
    NodeLoadHardwareStore::LoadConfiguration configuration{};
    std::snprintf(configuration.name, sizeof(configuration.name), "%s", request.name);
    configuration.relayPin = request.relayPin;
    configuration.relayActiveHigh = request.relayActiveHigh;
    configuration.powerRatingWatts = request.powerRatingWatts;
    configuration.powerType = request.powerType;
    configuration.mode = request.mode;
    configuration.priority = request.priority;
    configuration.schedule = request.schedule;

    HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return smartCommandResult(false, false, "state is busy");
    }

    const bool configured =
        smartNodeConfigurationStore.configureNewLoad(configuration, relays, *thisSmartNode, reason);

    xSemaphoreGive(nodeMutex);

    return configured
        ? smartCommandResult(true, true, "Load configured")
        : smartCommandResult(false, false, hardwareConfigurationFailureText(reason));
}

CommandResult consoleSmartRemoveLoad(void*, std::uint8_t relayPin)
{
    HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return smartCommandResult(false, false, "state is busy");
    }

    const bool removed =
        smartNodeConfigurationStore.removeLoad(relayPin, relays, *thisSmartNode, reason);

    xSemaphoreGive(nodeMutex);

    return removed
        ? smartCommandResult(true, true, "Load removed")
        : smartCommandResult(false, false, hardwareConfigurationFailureText(reason));
}

/**
 * In-place priority/mode/schedule change, without touching the relay pin,
 * name, power rating or polarity - reads the Load's current configuration
 * and re-applies it with only the requested fields changed, the same
 * remove-then-reconfigure mechanism configureNewLoad() already uses, so
 * there is no second, independently-maintained update path.
 */
CommandResult consoleSmartUpdateLoad(void*, const SmartLoadUpdateRequest& request)
{
    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return smartCommandResult(false, false, "state is busy");
    }

    const NodeLoadHardwareStore::LoadConfiguration* existing =
        smartNodeConfigurationStore.findByRelayPin(request.relayPin);

    if (existing == nullptr) {
        xSemaphoreGive(nodeMutex);
        return smartCommandResult(false, false, "Load not found");
    }

    NodeLoadHardwareStore::LoadConfiguration updated = *existing;
    if (request.hasPriority) updated.priority = request.priority;
    if (request.hasMode) updated.mode = request.mode;
    if (request.hasSchedule) updated.schedule = request.schedule;

    HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
    (void)smartNodeConfigurationStore.removeLoad(request.relayPin, relays, *thisSmartNode, reason);

    const bool configured =
        smartNodeConfigurationStore.configureNewLoad(updated, relays, *thisSmartNode, reason);

    xSemaphoreGive(nodeMutex);

    return configured
        ? smartCommandResult(true, true, "Load updated")
        : smartCommandResult(false, false, hardwareConfigurationFailureText(reason));
}

bool startSmartConsole()
{
    SmartConsole::Callbacks callbacks{};
    callbacks.status = &consoleSmartStatus;
    callbacks.loads = &consoleSmartLoads;
    callbacks.loadStatus = &consoleSmartLoadStatus;
    callbacks.configureLoad = &consoleSmartConfigureLoad;
    callbacks.removeLoad = &consoleSmartRemoveLoad;
    callbacks.updateLoad = &consoleSmartUpdateLoad;
    callbacks.context = nullptr;

    return smartConsole.begin(callbacks);
}

void relayControlTask(void* parameter)
{
    (void)parameter;
    RelayCommandQueueItem item{};

    while (true) {
        if (xQueueReceive(relayCommandQueue, &item, portMAX_DELAY) != pdTRUE) continue;

        const bool desiredOn =
            static_cast<RelayCommandState>(item.command.desiredState) == RelayCommandState::ON;
        const bool registered = relays.isRelayRegistered(item.command.relayPin);
        const bool success = registered && relays.setRelayState(item.command.relayPin, desiredOn);

        RelayCommandAcknowledgementPacket acknowledgement{};
        acknowledgement.relayPin = item.command.relayPin;
        acknowledgement.commandId = item.command.commandId;
        acknowledgement.requestedState = item.command.desiredState;
        acknowledgement.success = success ? 1U : 0U;
        acknowledgement.failureReason = static_cast<std::uint8_t>(
            success ? RelayCommandFailureReason::NONE
                    : (!registered ? RelayCommandFailureReason::RELAY_PIN_NOT_REGISTERED
                                   : RelayCommandFailureReason::GPIO_WRITE_FAILED));

        communication.sendToCentral(EspNowCommunication::MessageType::ACKNOWLEDGEMENT, acknowledgement);
    }
}

[[noreturn]] void performSmartFactoryReset()
{
    nvs_flash_erase();
    nvs_flash_init();
    vTaskDelay(pdMS_TO_TICKS(200U));
    esp_restart();
    while (true) {}
}

void handleCommissionCommand(const CommissionCommandPacket& command)
{
    bool accepted = false;
    NodeLifecycleState resultingState = NodeLifecycleState::UNCOMMISSIONED;
    char appliedName[EspNowCommunication::NODE_NAME_LENGTH]{};

    if (xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
        const NodeIdentityStore previous = identityStore;
        accepted = identityStore.applyCommission(command.friendlyName);
        if (accepted && !identityStore.persist()) {
            identityStore = previous;
            accepted = false;
        }
        resultingState = identityStore.getLifecycleState();
        std::snprintf(appliedName, sizeof(appliedName), "%s", identityStore.getFriendlyName());
        xSemaphoreGive(identityMutex);
    }

    if (accepted) communication.setLocalNodeName(appliedName);

    CommissionAckPacket ack{};
    ack.commandId = command.commandId;
    ack.success = accepted ? 1U : 0U;
    ack.resultingState = static_cast<std::uint8_t>(resultingState);
    communication.sendToCentral(EspNowCommunication::MessageType::COMMISSION_ACK, ack);
    sendIdentityReport();
}

void handleDecommissionCommand(const DecommissionCommandPacket& command)
{
    HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
    bool loadsCleared = false;

    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
        loadsCleared = smartNodeConfigurationStore.clearAllConfigurations(relays, *thisSmartNode, reason);
        xSemaphoreGive(nodeMutex);
    }

    bool identityCleared = false;
    if (loadsCleared && xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
        const NodeIdentityStore previous = identityStore;
        identityStore.applyDecommission();
        identityCleared = identityStore.persist();
        if (!identityCleared) identityStore = previous;
        xSemaphoreGive(identityMutex);
    }

    const bool success = loadsCleared && identityCleared;
    if (success) {
        char automaticName[EspNowCommunication::NODE_NAME_LENGTH]{};
        computeAutomaticNodeName(communication.getLocalMacAddress(), automaticName, sizeof(automaticName));
        communication.setLocalNodeName(automaticName);
    }

    DecommissionAckPacket ack{};
    ack.commandId = command.commandId;
    ack.success = success ? 1U : 0U;
    communication.sendToCentral(EspNowCommunication::MessageType::DECOMMISSION_ACK, ack);
    sendIdentityReport();
}

void handleHardwareConfigurationMessage(const EspNowCommunication::ReceivedMessage& received)
{
    if (received.message.header.payloadLength == sizeof(ConfigureLoadCommandPacket)) {
        ConfigureLoadCommandPacket command{};
        std::memcpy(&command, received.message.payload.data(), sizeof(command));
        const HardwareConfigurationFailureReason reason = applyConfigureLoadCommand(command);

        ConfigureLoadAcknowledgementPacket ack{};
        ack.commandId = command.commandId;
        ack.relayPin = command.relayPin;
        ack.success = reason == HardwareConfigurationFailureReason::NONE ? 1U : 0U;
        ack.failureReason = static_cast<std::uint8_t>(reason);
        communication.sendToCentral(EspNowCommunication::MessageType::CONFIGURE_LOAD_ACK, ack);
        return;
    }

    if (received.message.header.payloadLength == sizeof(RemoveLoadCommandPacket)) {
        RemoveLoadCommandPacket command{};
        std::memcpy(&command, received.message.payload.data(), sizeof(command));
        const HardwareConfigurationFailureReason reason = applyRemoveLoadCommand(command);

        RemoveLoadAcknowledgementPacket ack{};
        ack.commandId = command.commandId;
        ack.relayPin = command.relayPin;
        ack.success = reason == HardwareConfigurationFailureReason::NONE ? 1U : 0U;
        ack.failureReason = static_cast<std::uint8_t>(reason);
        communication.sendToCentral(
            EspNowCommunication::MessageType::CONFIGURE_LOAD_ACK,
            &ack,
            sizeof(ack));
    }
}

void espNowCommunicationTask(void* parameter)
{
    (void)parameter;

    const auto localMac = communication.getLocalMacAddress();
    std::uint16_t reportSequence = 0U;
    TickType_t lastReport = xTaskGetTickCount();
    TickType_t lastDiscovery = 0U;

    int discoveryFailures = 0;
    const int DISCOVERY_FAILURES_BEFORE_SWEEP = 3;
    while (true) {
        const TickType_t now = xTaskGetTickCount();
        if (!communication.hasUpstreamNode() &&
            (now - lastDiscovery) >= pdMS_TO_TICKS(DISCOVERY_RETRY_PERIOD_MS)) {
            lastDiscovery = now;
            const bool discovered = communication.discoverUpstreamNode(DISCOVERY_WINDOW_MS);
            if (!discovered) {
                discoveryFailures++;
            } else {
                discoveryFailures = 0;
            }

            if (discoveryFailures >= DISCOVERY_FAILURES_BEFORE_SWEEP) {
                ESP_LOGW(TAG, "Discovery failed %d times, performing channel sweep...", discoveryFailures);
                discoveryFailures = 0;
                for (uint8_t ch = 1; ch <= 13; ++ch) {
                    esp_err_t setCh = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
                    if (setCh != ESP_OK) {
                        ESP_LOGW(TAG, "esp_wifi_set_channel(%u) failed: %s", ch, esp_err_to_name(setCh));
                        continue;
                    }
                    ESP_LOGI(TAG, "Attempting discovery on channel %u", ch);
                    if (communication.discoverUpstreamNode(DISCOVERY_WINDOW_MS)) {
                        ESP_LOGI(TAG, "Discovery succeeded on channel %u", ch);
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
        }

        EspNowCommunication::ReceivedMessage received{};
        if (communication.receive(received, 250U)) {
            const bool localDestination = received.message.header.destinationMacAddress == localMac;
            const auto type = received.message.header.messageType;

            if (localDestination && type == EspNowCommunication::MessageType::RELAY_COMMAND &&
                received.message.header.payloadLength == sizeof(RelayCommandPacket)) {
                RelayCommandQueueItem item{};
                std::memcpy(&item.command, received.message.payload.data(), sizeof(item.command));
                xQueueSend(relayCommandQueue, &item, pdMS_TO_TICKS(100U));

            } else if (localDestination && type == EspNowCommunication::MessageType::COMMISSION_COMMAND &&
                       received.message.header.payloadLength == sizeof(CommissionCommandPacket)) {
                CommissionCommandPacket command{};
                std::memcpy(&command, received.message.payload.data(), sizeof(command));
                handleCommissionCommand(command);

            } else if (localDestination && type == EspNowCommunication::MessageType::DECOMMISSION_COMMAND &&
                       received.message.header.payloadLength == sizeof(DecommissionCommandPacket)) {
                DecommissionCommandPacket command{};
                std::memcpy(&command, received.message.payload.data(), sizeof(command));
                handleDecommissionCommand(command);

            } else if (localDestination && type == EspNowCommunication::MessageType::CONFIGURE_LOAD_COMMAND) {
                handleHardwareConfigurationMessage(received);

            } else if (localDestination && type == EspNowCommunication::MessageType::FACTORY_RESET_COMMAND &&
                       received.message.header.payloadLength == sizeof(FactoryResetCommandPacket)) {
                FactoryResetCommandPacket command{};
                std::memcpy(&command, received.message.payload.data(), sizeof(command));
                if (command.confirmToken == FACTORY_RESET_CONFIRM_TOKEN) performSmartFactoryReset();

            } else if (!localDestination) {
                if (communication.hasUpstreamNode() &&
                    received.senderMacAddress != communication.getUpstreamNodeMacAddress()) {
                    communication.forwardMessageTo(communication.getUpstreamNodeMacAddress(), received.message);
                } else {
                    communication.forwardMessageTo(received.message.header.destinationMacAddress, received.message);
                }
            }
        }

        if ((now - lastReport) >= pdMS_TO_TICKS(NODE_REPORT_PERIOD_MS)) {
            lastReport = now;
            ++reportSequence;

            std::size_t loadCount = 0U;
            if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                if (thisSmartNode != nullptr) {
                    loadCount = thisSmartNode->getNumberOfLoads();
                }
                xSemaphoreGive(nodeMutex);
            }

            const std::uint8_t totalPages = computeReportPageCount(loadCount);
            for (std::uint8_t page = 0U; page < totalPages; ++page) {
                const NodeReportPacket report =
                    buildNodeReportPacket(localMac, reportSequence, page, totalPages);
                communication.sendToCentral(EspNowCommunication::MessageType::NODE_REPORT, report);
            }
        }
    }
}

void watchdogTask(void* parameter)
{
    (void)parameter;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_PERIOD_MS));
        sendIdentityReport();
    }
}

void printSmartBootSummary(const EspNowCommunication::MacAddress& localMac)
{
    char mac[18]{};
    formatMacAddressText(mac, sizeof(mac), localMac);
    ESP_LOGI(TAG, "Smart Node %s | name=%s | state=%s | loads=%u",
             mac,
             communication.getLocalNodeName(),
             toText(identityStore.getLifecycleState()),
             static_cast<unsigned int>(thisSmartNode != nullptr ? thisSmartNode->getNumberOfLoads() : 0U));
}

} // namespace

#endif // KILOWATTS_SMART_RUNTIME_H
