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
#include "RadioConfig.h"
#include "RelayController.h"
#include "SmartNodeConfig.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
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

    /* No compiled-in safe-pin list to declare anymore — the installer names a relay pin directly per Load. */
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

NodeReportPacket buildNodeReportPacket(
    const EspNowCommunication::MacAddress& localMac,
    std::uint16_t sequence)
{
    NodeReportPacket packet{};
    std::snprintf(packet.nodeName, sizeof(packet.nodeName), "%s", communication.getLocalNodeName());
    packet.nodeMacAddress = localMac;
    packet.upstreamNodeMacAddress = communication.hasUpstreamNode()
        ? communication.getUpstreamNodeMacAddress() : localMac;
    packet.hopCountToCentral = communication.getHopCountToCentral();
    packet.reportSequenceId = sequence;
    packet.pageIndex = 0U;
    packet.totalPages = 1U;

    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(200U)) != pdTRUE || thisSmartNode == nullptr) {
        return packet;
    }

    packet.numberOfLoads = static_cast<std::uint8_t>(
        std::min<std::size_t>(thisSmartNode->getNumberOfLoads(), MAX_LOADS_PER_NODE_PACKET));

    for (std::size_t i = 0U; i < packet.numberOfLoads; ++i) {
        const Load* load = thisSmartNode->getLoad(i);
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
        out.scheduleHour = schedule.hour;
        out.scheduleMinute = schedule.minute;
    }

    xSemaphoreGive(nodeMutex);
    return packet;
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
        command.scheduleHour,
        command.scheduleMinute};

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

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        if (!communication.hasUpstreamNode() &&
            (now - lastDiscovery) >= pdMS_TO_TICKS(DISCOVERY_RETRY_PERIOD_MS)) {
            lastDiscovery = now;
            communication.discoverUpstreamNode(DISCOVERY_WINDOW_MS);
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
            const NodeReportPacket report = buildNodeReportPacket(localMac, reportSequence);
            communication.sendToCentral(EspNowCommunication::MessageType::NODE_REPORT, report);
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