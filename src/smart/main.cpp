#if defined(DEVICE_ROLE_SMART)

/**
 * @file main.cpp
 * @brief Smart Node orchestration entry point.
 *
 * This file only instantiates real modules, creates the FreeRTOS
 * tasks/queues/synchronization that wire them together, and starts the
 * system — every real responsibility (relay actuation, ESP-NOW transport,
 * schedule evaluation and time) lives in the
 * lib/ module that owns it.
 *
 * Pipeline implemented here (Section 4.6, "Smart Node" role):
 *
 *   ESP-NOW Communication Task (event-driven + periodic report)
 *       builds/sends NodeReportPacket, learns/forwards other traffic,
 *       hands RELAY_COMMAND messages to the Relay Control Task's queue
 *   Relay Control Task (event-driven)
 *       RelayController::setRelayState() -> readBackState() ->
 *       RelayCommandAcknowledgementPacket back to Central
 *   Watchdog/Diagnostics Task (~60s)
 *       link/relay health, re-discovers Central if the route was
 *       lost, sends a periodic IDENTITY_REPORT
 *
 * A freshly flashed/uncommissioned Node has zero Loads/Branches
 * (Section "New Smart Node Boot") until a COMMISSION_COMMAND from Central
 * assigns it a friendly name (see lib/NodeIdentityStore,
 * lib/CommissioningPackets) — handled inline inside the ESP-NOW
 * Communication Task's own receive dispatch, the same place RELAY_COMMAND
 * already is.
 *
 * nodeMutex protects thisSmartNode (the Node/Load objects), since the Relay
 * Control Task and the ESP-NOW Communication Task's report builder both
 * read or write Load state concurrently. identityMutex separately protects identityStore, written
 * by the ESP-NOW Communication Task and read by both it and the Watchdog
 * Task.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "ChipInfo.h"
#include "CommissioningPackets.h"
#include "CurrentTimeProvider.h"
#include "DevelopmentPackets.h"
#include "DevelopmentSession.h"
#include "EspNowCommunication.h"
#include "FirmwareVersion.h"
#include "HardwareConfigurationPackets.h"
#include "Load.h"
#include "Node.h"
#include "NodeIdentityStore.h"
#include "NodeLifecycle.h"
#include "NodeReportPackets.h"
#include "RadioConfig.h"
#include "RelayController.h"
#include "SmartNodeConfig.h"
#include "SmartNodeConfigurationStore.h"

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

static_assert(sizeof(ConfigureLoadCommandPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "ConfigureLoadCommandPacket is too large for one ESP-NOW message");
static_assert(sizeof(ConfigureLoadAcknowledgementPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "ConfigureLoadAcknowledgementPacket is too large for one ESP-NOW message");

static const char *TAG = "SMART_MAIN";

namespace {

constexpr std::uint32_t NODE_REPORT_PERIOD_MS = 2000U;
constexpr std::uint32_t WATCHDOG_PERIOD_MS = 60000U;

/** Section "Application-Level Node Report Ack": how long to wait for Central's NODE_REPORT_ACK before logging CENTRAL_ACK_TIMEOUT. */
constexpr std::uint32_t NODE_REPORT_ACK_TIMEOUT_MS = 3000U;

/*
 * Section "Smart Node Must Not Block On Central": upstream discovery is
 * retried here, asynchronously, from inside espNowCommunicationTask's own
 * loop — never as a blocking loop in app_main() before local tasks even
 * start. UPSTREAM_DISCOVERY_ATTEMPT_TIMEOUT_MS is deliberately short so
 * one retry never meaningfully delays RELAY_COMMAND routing or
 * NODE_REPORT sending; UPSTREAM_DISCOVERY_RETRY_PERIOD_MS paces how often
 * a retry is attempted while no route exists.
 */
constexpr std::uint32_t UPSTREAM_DISCOVERY_RETRY_PERIOD_MS = 2000U;
constexpr std::uint32_t UPSTREAM_DISCOVERY_ATTEMPT_TIMEOUT_MS = 500U;

/*
 * Long-lived objects: this file's tasks run for the lifetime of the
 * device, so these must outlive app_main() itself (which returns once
 * every task is created, per normal ESP-IDF practice) — plain function-
 * local variables would be destroyed the moment app_main() returned.
 */
EspNowCommunication communication(kilowatts::KILOWATTS_RADIO_CHANNEL);
CurrentTimeProvider currentTimeProvider;
RelayController relays;
ChipInfo chipInfo;
NodeIdentityStore identityStore;
SmartNodeConfigurationStore smartNodeConfigurationStore;

/**
 * This Node's explicit runtime Development Session (Section "Development
 * Session Is Explicit") - always PRODUCTION on boot, never inferred from
 * missing hardware or a compile-time flag. Smart Nodes have no individual
 * current sensor in the final design, so a received DEV_SESSION_COMMAND can
 * only start/end the session; battery sensor simulation remains Central-only.
 * Guarded by identityMutex, the same mutex used for this Node's other
 * session-level state (identityStore).
 */
DevelopmentSession developmentSession;

Node *thisSmartNode = nullptr;
SemaphoreHandle_t nodeMutex = nullptr;
QueueHandle_t relayCommandQueue = nullptr;

/**
 * Guards identityStore: written by espNowCommunicationTask (applying a
 * received COMMISSION_COMMAND/DECOMMISSION_COMMAND) and read by both
 * espNowCommunicationTask and watchdogTask (sendIdentityReport(), on its
 * own periodic cadence) - a separate mutex from nodeMutex since it
 * protects a genuinely different piece of shared state (Node identity,
 * not Load/Node domain data).
 */
SemaphoreHandle_t identityMutex = nullptr;

struct RelayCommandQueueItem {
    RelayCommandPacket command;
};


/**
 * "Smart-AABBCC" from the last three MAC octets - used whenever this Node
 * has no commissioned friendly name (never commissioned yet, or just
 * decommissioned), so it always has *some* stable, honestly-derived name
 * to advertise instead of a fabricated one.
 */
void computeAutomaticNodeName(const EspNowCommunication::MacAddress& localMac, char* buffer, std::size_t bufferSize)
{
    std::snprintf(buffer, bufferSize, "Smart-%02X%02X%02X", localMac[3], localMac[4], localMac[5]);
}


/** "AA:BB:CC:DD:EE:FF" — the one place this file formats a MAC address for logging. */
void formatMacAddressText(char* buffer, std::size_t bufferSize, const EspNowCommunication::MacAddress& macAddress)
{
    std::snprintf(buffer, bufferSize, "%02X:%02X:%02X:%02X:%02X:%02X",
                  macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
}


/**
 * Relay Control Task (Section 4.6.1, event-driven): applies a queued
 * RelayCommandPacket through RelayController, reads back the confirmed
 * state, updates the owning Load's physical truth, and replies with a
 * RelayCommandAcknowledgementPacket — never presenting the requested
 * state as confirmed until RelayController itself confirms it.
 */
void relayControlTask(void *parameter)
{
    (void)parameter;

    RelayCommandQueueItem item{};

    while (true) {
        if (xQueueReceive(relayCommandQueue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const bool desiredOn = static_cast<RelayCommandState>(item.command.desiredState) == RelayCommandState::ON;

        ESP_LOGI(TAG, "SMART_ACTUATE commandId=%u pin=%u desired=%s",
                 static_cast<unsigned int>(item.command.commandId), static_cast<unsigned int>(item.command.relayPin),
                 desiredOn ? "ON" : "OFF");

        const bool writeSucceeded = relays.setRelayState(item.command.relayPin, desiredOn);

        bool confirmedOn = false;
        const bool readBackSucceeded = relays.readBackState(item.command.relayPin, confirmedOn);

        RelayCommandAcknowledgementPacket acknowledgement{};
        acknowledgement.relayPin = item.command.relayPin;
        acknowledgement.commandId = item.command.commandId;
        acknowledgement.requestedState = item.command.desiredState;

        if (writeSucceeded && readBackSucceeded && confirmedOn == desiredOn) {
            acknowledgement.confirmedState = static_cast<std::uint8_t>(
                confirmedOn ? RelayCommandState::ON : RelayCommandState::OFF);
            acknowledgement.success = 1U;
            acknowledgement.failureReason = static_cast<std::uint8_t>(RelayCommandFailureReason::NONE);
        } else {
            acknowledgement.confirmedState = static_cast<std::uint8_t>(RelayCommandState::OFF);
            acknowledgement.success = 0U;
            acknowledgement.failureReason = static_cast<std::uint8_t>(
                !writeSucceeded ? RelayCommandFailureReason::GPIO_WRITE_FAILED
                                : (!readBackSucceeded ? RelayCommandFailureReason::RELAY_PIN_NOT_REGISTERED
                                                       : RelayCommandFailureReason::READBACK_MISMATCH));
        }

        if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            Load *load = thisSmartNode->getLoadByRelayPin(item.command.relayPin);
            if (load != nullptr) {
                if (acknowledgement.success != 0U) {
                    load->setConfirmedRelayState(confirmedOn);
                    load->setHealth(LoadHealth::AVAILABLE);
                } else {
                    /*
                     * A failed relay command does not prove the relay is
                     * OFF (Section "Fix Relay Confirmation Failure
                     * Semantics") — leave the previous trusted confirmed
                     * state/validity exactly as it was, only mark fault.
                     */
                    load->setHealth(LoadHealth::FAULTED);
                }
            }
            xSemaphoreGive(nodeMutex);
        }

        ESP_LOGI(TAG, "SMART_ACTUATION_RESULT commandId=%u pin=%u requested=%s confirmed=%s success=%s failureReason=%u",
                 static_cast<unsigned int>(item.command.commandId), static_cast<unsigned int>(item.command.relayPin),
                 desiredOn ? "ON" : "OFF",
                 acknowledgement.success != 0U ? (confirmedOn ? "ON" : "OFF") : "UNKNOWN",
                 acknowledgement.success != 0U ? "Yes" : "No",
                 static_cast<unsigned int>(acknowledgement.failureReason));

        /*
         * Only Central ever dispatches a RelayCommandPacket (only Central
         * runs Best-First Search / OFF-before-ON dispatch), so the
         * acknowledgement's destination is always Central itself —
         * sendToCentral() routes it back through this Node's own upstream
         * link regardless of how many hops away Central actually is.
         */
        ESP_LOGI(TAG, "SMART_TX RELAY_ACK commandId=%u pin=%u confirmed=%s success=%s",
                 static_cast<unsigned int>(acknowledgement.commandId), static_cast<unsigned int>(acknowledgement.relayPin),
                 acknowledgement.success != 0U ? (confirmedOn ? "ON" : "OFF") : "UNKNOWN",
                 acknowledgement.success != 0U ? "Yes" : "No");
        communication.sendToCentral(EspNowCommunication::MessageType::ACKNOWLEDGEMENT, acknowledgement);
    }
}


NodeReportPacket buildNodeReportPacket(const EspNowCommunication::MacAddress& localMac,
                                        std::uint16_t hopCount,
                                        std::uint16_t reportSequenceId)
{
    NodeReportPacket packet{};
    std::snprintf(packet.nodeName, sizeof(packet.nodeName), "%s", communication.getLocalNodeName());
    packet.nodeMacAddress = localMac;
    packet.upstreamNodeMacAddress = communication.hasUpstreamNode()
        ? communication.getUpstreamNodeMacAddress() : localMac;
    packet.hopCountToCentral = hopCount;
    packet.reportSequenceId = reportSequenceId;
    packet.pageIndex = 0U;
    packet.totalPages = 1U;

    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
        packet.numberOfLoads = static_cast<std::uint8_t>(
            thisSmartNode->getNumberOfLoads() > MAX_LOADS_PER_NODE_PACKET
                ? MAX_LOADS_PER_NODE_PACKET : thisSmartNode->getNumberOfLoads());

        for (std::size_t i = 0U; i < packet.numberOfLoads; ++i) {
            const Load *load = thisSmartNode->getLoad(i);
            if (load == nullptr) {
                continue;
            }

            LoadReportPacket &loadPacket = packet.loads[i];
            std::snprintf(loadPacket.name, sizeof(loadPacket.name), "%s", load->getName().c_str());
            loadPacket.relayPin = load->getRelayPin();
            loadPacket.mode = static_cast<std::uint8_t>(load->getMode());
            loadPacket.priority = load->getPriority();
            loadPacket.startupWatts = load->getPower().startupWatts;

            /*
             * One configured relay channel is one electrical branch in the
             * current design. This value is persisted together with the
             * channel and is therefore never fabricated from a UI default.
             */
            float branchMaximumCurrentAmps = 0.0F;
            if (smartNodeConfigurationStore.branchMaximumCurrentAmps(load->getRelayPin(), branchMaximumCurrentAmps)) {
                loadPacket.branchMaximumCurrentAmps = branchMaximumCurrentAmps;
            }

            const LoadElectricalRatings ratings = load->getElectricalRatings();
            loadPacket.nominalVoltageVolts = ratings.nominalVoltageVolts;
            loadPacket.nominalCurrentAmps = ratings.nominalCurrentAmps;

            loadPacket.confirmedRelayState = load->getConfirmedRelayState() ? 1U : 0U;
            loadPacket.confirmedRelayStateValid = load->isConfirmedRelayStateValid() ? 1U : 0U;

            const AutoSchedule schedule = load->getSchedule();
            loadPacket.scheduleEnabled = schedule.enabled ? 1U : 0U;
            loadPacket.scheduleHour = schedule.hour;
            loadPacket.scheduleMinute = schedule.minute;

            loadPacket.availability = static_cast<std::uint8_t>(load->getHealth());
        }

        xSemaphoreGive(nodeMutex);
    }

    return packet;
}


/**
 * Sends this Node's current identity/lifecycle to Central - deliberately
 * not on buildNodeReportPacket()'s hot ~2s cycle (see
 * lib/CommissioningPackets's own README). Called once at boot, on the
 * slower watchdog cadence for periodic refresh, and immediately after any
 * local lifecycle change (a commission/decommission command was just
 * applied) so Central learns the new state promptly rather than waiting
 * for the next periodic tick.
 */
void sendIdentityReport()
{
    IdentityReportPacket packet{};
    packet.role = static_cast<std::uint8_t>(NodeRole::SMART);
    std::snprintf(packet.firmwareVersion, sizeof(packet.firmwareVersion), "%s", KILOWATTS_FIRMWARE_VERSION);
    chipInfo.getChipModelText(packet.chipModel, sizeof(packet.chipModel));

    NodeLifecycleState lifecycleState = NodeLifecycleState::UNCOMMISSIONED;
    if (xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
        lifecycleState = identityStore.getLifecycleState();
        xSemaphoreGive(identityMutex);
    }
    packet.lifecycleState = static_cast<std::uint8_t>(lifecycleState);
    packet.relayCapabilityCount = static_cast<std::uint8_t>(
        SmartNodeConfig::getVerifiedRelayPinCount() > MAX_RELAY_GPIO_CAPABILITIES
            ? MAX_RELAY_GPIO_CAPABILITIES : SmartNodeConfig::getVerifiedRelayPinCount());
    for (std::size_t index = 0U; index < packet.relayCapabilityCount; ++index) {
        packet.relayPins[index] = SmartNodeConfig::getVerifiedRelayPin(index);
    }

    const bool sent = communication.sendToCentral(EspNowCommunication::MessageType::IDENTITY_REPORT, packet);
    ESP_LOGI(TAG, "IDENTITY_REPORT name='%s' state=%s %s", communication.getLocalNodeName(),
             toText(lifecycleState), sent ? "SENT" : "FAILED (no upstream route yet)");
}


struct DevCommandOutcome {
    bool success;
    char reason[64];
};


/**
 * Applies a locally-targeted Development Session command (Section
 * "Development Session Is Explicit") — Smart Nodes can start/end an explicit
 * session, but may not simulate per-load electrical measurements because the
 * only INA219 belongs to Central's battery bus. Called from both the ESP-NOW
 * receive dispatch and consoleTask(), never through a divergent path.
 */
DevCommandOutcome applyStartDevelopmentSession()
{
    DevCommandOutcome outcome{};
    if (xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
        outcome.success = developmentSession.start();
        xSemaphoreGive(identityMutex);
    }
    ESP_LOGW(TAG, "DEV_SESSION_START: node=%s result=%s", communication.getLocalNodeName(),
             outcome.success ? "OK" : "ALREADY_ACTIVE");
    std::snprintf(outcome.reason, sizeof(outcome.reason), outcome.success ? "session started" : "a session is already active");
    return outcome;
}


DevCommandOutcome applyEndDevelopmentSession()
{
    DevCommandOutcome outcome{};
    if (xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
        outcome.success = developmentSession.end();
        xSemaphoreGive(identityMutex);
    }
    ESP_LOGW(TAG, "DEV_SESSION_END: node=%s result=%s", communication.getLocalNodeName(),
             outcome.success ? "OK" : "NOT_ACTIVE");
    std::snprintf(outcome.reason, sizeof(outcome.reason), outcome.success ? "session ended" : "no session was active");
    return outcome;
}


/**
 * Factory reset (Section "Factory Reset"): establishes a safe physical
 * relay state, erases every installation-specific persisted record (a
 * full NVS erase — the ESP32's MAC address and chip identity live in
 * efuses, never NVS, so this can never touch immutable hardware identity),
 * and reboots into PRODUCTION + UNCOMMISSIONED. Never returns.
 *
 * Section "Do Not Confuse Firmware Flash With Factory Reset": this is a
 * deliberate, explicit action distinct from uploading new firmware — a
 * normal firmware upload does not call this and does not erase NVS.
 */
[[noreturn]] void performSmartFactoryReset()
{
    ESP_LOGW(TAG, "FACTORY_RESET: establishing safe physical outputs before erasing NVS");
    relays.printDiagnosticReport();

    ESP_LOGW(TAG, "FACTORY_RESET: erasing NVS (commissioning identity/name, relay/load configuration) - "
                  "immutable hardware identity is unaffected");
    const esp_err_t eraseResult = nvs_flash_erase();
    if (eraseResult != ESP_OK) {
        ESP_LOGE(TAG, "FACTORY_RESET: nvs_flash_erase() failed: %s", esp_err_to_name(eraseResult));
    }
    const esp_err_t initResult = nvs_flash_init();
    if (initResult != ESP_OK) {
        ESP_LOGE(TAG, "FACTORY_RESET: nvs_flash_init() after erase failed: %s", esp_err_to_name(initResult));
    }

    ESP_LOGW(TAG, "FACTORY_RESET: complete (NVS cleared, firmware unchanged). Rebooting into PRODUCTION + UNCOMMISSIONED.");
    vTaskDelay(pdMS_TO_TICKS(200U)); // let the log line above actually flush over UART before reset
    esp_restart();
}


HardwareConfigurationFailureReason applyConfigureLoadCommand(const ConfigureLoadCommandPacket& command)
{
    NodeLifecycleState lifecycleState = NodeLifecycleState::UNCOMMISSIONED;
    if (xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) != pdTRUE) {
        return HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
    }
    lifecycleState = identityStore.getLifecycleState();
    xSemaphoreGive(identityMutex);

    if (lifecycleState != NodeLifecycleState::COMMISSIONED &&
        lifecycleState != NodeLifecycleState::OPERATIONAL) {
        return HardwareConfigurationFailureReason::NODE_NOT_COMMISSIONED;
    }
    if (!SmartNodeConfig::isVerifiedRelayPin(command.relayPin)) {
        return HardwareConfigurationFailureReason::UNSUPPORTED_RELAY_PIN;
    }

    SmartNodeConfigurationStore::LoadConfiguration configuration{};
    std::memcpy(configuration.name, command.loadName, sizeof(configuration.name));
    configuration.name[sizeof(configuration.name) - 1U] = '\0';
    configuration.relayPin = command.relayPin;
    configuration.relayActiveHigh = command.relayActiveHigh != 0U;
    configuration.mode = static_cast<LoadMode::Value>(command.mode);
    configuration.priority = command.priority;
    configuration.nominalVoltageVolts = command.nominalVoltageVolts;
    configuration.nominalCurrentAmps = command.nominalCurrentAmps;
    configuration.branchMaximumCurrentAmps = command.branchMaximumCurrentAmps;
    configuration.startupWatts = command.startupWatts;
    configuration.schedule = AutoSchedule{command.scheduleEnabled != 0U, command.scheduleHour, command.scheduleMinute};

    if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
    }
    HardwareConfigurationFailureReason failureReason = HardwareConfigurationFailureReason::NONE;
    smartNodeConfigurationStore.configureNewLoad(configuration, relays, *thisSmartNode, failureReason);
    xSemaphoreGive(nodeMutex);
    return failureReason;
}


/**
 * A persisted installation record is never allowed to bypass the current
 * flashed board profile. This matters when a device is reflashed for a
 * different ESP32 carrier: a GPIO that was once valid may now be camera,
 * flash or boot hardware. The operator must verify the new profile and
 * explicitly commission channels again instead of silently energising an
 * old pin at boot.
 */
bool persistedRelayConfigurationMatchesBoardProfile()
{
    for (std::size_t index = 0U;
         index < smartNodeConfigurationStore.getNumberOfConfigurations(); ++index) {
        const SmartNodeConfigurationStore::LoadConfiguration* configuration =
            smartNodeConfigurationStore.getConfiguration(index);
        if (configuration != nullptr &&
            !SmartNodeConfig::isVerifiedRelayPin(configuration->relayPin)) {
            ESP_LOGE(TAG,
                     "NVS_RESTORE: configured relay GPIO %u is not declared safe by this flashed board profile",
                     static_cast<unsigned int>(configuration->relayPin));
            return false;
        }
    }
    return true;
}


/**
 * ESP-NOW Communication Task (Section 4.6.1, event-driven): periodically
 * sends this Node's report toward Central, and otherwise reproduces the
 * dynamic tree behaviour already proven for this project — learning
 * NODE_REPORTs, forwarding traffic up/down the tree, routing an addressed
 * RELAY_COMMAND to the Relay Control Task's queue, and applying an
 * addressed COMMISSION_COMMAND/DECOMMISSION_COMMAND (Section
 * "Commissioning").
 */
void espNowCommunicationTask(void *parameter)
{
    (void)parameter;

    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();
    std::uint16_t reportSequenceId = 0U;
    TickType_t lastReportTick = xTaskGetTickCount();
    TickType_t lastDiscoveryAttemptTick = 0U; // 0 forces an immediate first attempt below

    /*
     * Section "Application-Level Node Report Ack": tracks the one
     * outstanding NODE_REPORT this task is waiting on Central to
     * acknowledge. Local to this task (only it ever sends a NODE_REPORT or
     * receives a NODE_REPORT_ACK), so no mutex is needed for these.
     */
    bool awaitingReportAck = false;
    std::uint16_t lastSentReportSequenceId = 0U;
    TickType_t reportAckSentTick = 0U;

    while (true) {
        const TickType_t discoveryNow = xTaskGetTickCount();
        if (!communication.hasUpstreamNode() &&
            (discoveryNow - lastDiscoveryAttemptTick) >= pdMS_TO_TICKS(UPSTREAM_DISCOVERY_RETRY_PERIOD_MS)) {
            lastDiscoveryAttemptTick = discoveryNow;
            const bool discovered = communication.discoverUpstreamNode(UPSTREAM_DISCOVERY_ATTEMPT_TIMEOUT_MS);
            ESP_LOGI(TAG, "Upstream discovery attempt: %s", discovered ? "ROUTE FOUND" : "no route yet");
        }

        if (awaitingReportAck &&
            (discoveryNow - reportAckSentTick) >= pdMS_TO_TICKS(NODE_REPORT_ACK_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "NODE_REPORT seq=%u CENTRAL_ACK_TIMEOUT (no NODE_REPORT_ACK within %ums)",
                     static_cast<unsigned int>(lastSentReportSequenceId),
                     static_cast<unsigned int>(NODE_REPORT_ACK_TIMEOUT_MS));
            awaitingReportAck = false;
        }

        EspNowCommunication::ReceivedMessage received{};

        if (communication.receive(received, 1000U)) {
            const bool destinationIsLocal = received.message.header.destinationMacAddress == localMac;

            if (destinationIsLocal &&
                received.message.header.messageType == EspNowCommunication::MessageType::RELAY_COMMAND &&
                received.message.header.payloadLength == sizeof(RelayCommandPacket)) {

                RelayCommandQueueItem item{};
                std::memcpy(&item.command, received.message.payload.data(), sizeof(item.command));

                ESP_LOGI(TAG, "SMART_RX RELAY_COMMAND commandId=%u pin=%u desired=%s",
                         static_cast<unsigned int>(item.command.commandId), static_cast<unsigned int>(item.command.relayPin),
                         static_cast<RelayCommandState>(item.command.desiredState) == RelayCommandState::ON ? "ON" : "OFF");

                if (xQueueSend(relayCommandQueue, &item, pdMS_TO_TICKS(100U)) != pdTRUE) {
                    ESP_LOGW(TAG, "Relay command queue full; dropping commandId=%u",
                             static_cast<unsigned int>(item.command.commandId));
                }
            } else if (destinationIsLocal &&
                       received.message.header.messageType == EspNowCommunication::MessageType::COMMISSION_COMMAND &&
                       received.message.header.payloadLength == sizeof(CommissionCommandPacket)) {

                CommissionCommandPacket command{};
                std::memcpy(&command, received.message.payload.data(), sizeof(command));

                bool accepted = false;
                NodeLifecycleState resultingState = NodeLifecycleState::UNCOMMISSIONED;
                char appliedName[EspNowCommunication::NODE_NAME_LENGTH] = {};

                if (xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                    const NodeIdentityStore previousIdentity = identityStore;
                    accepted = identityStore.applyCommission(command.friendlyName);
                    if (accepted && !identityStore.persist()) {
                        /* A CommissionAck must never claim durable success when NVS failed. */
                        identityStore = previousIdentity;
                        accepted = false;
                    }
                    resultingState = identityStore.getLifecycleState();
                    std::snprintf(appliedName, sizeof(appliedName), "%s", identityStore.getFriendlyName());
                    xSemaphoreGive(identityMutex);
                }

                if (accepted) {
                    communication.setLocalNodeName(appliedName);
                }

                CommissionAckPacket acknowledgement{};
                acknowledgement.commandId = command.commandId;
                acknowledgement.success = accepted ? 1U : 0U;
                acknowledgement.resultingState = static_cast<std::uint8_t>(resultingState);
                communication.sendToCentral(EspNowCommunication::MessageType::COMMISSION_ACK, acknowledgement);

                ESP_LOGI(TAG, "Commission command commandId=%u name='%s' -> %s (state=%s)",
                         static_cast<unsigned int>(command.commandId), command.friendlyName,
                         accepted ? "ACCEPTED" : "REJECTED", toText(resultingState));

                sendIdentityReport();
            } else if (destinationIsLocal &&
                       received.message.header.messageType == EspNowCommunication::MessageType::DECOMMISSION_COMMAND &&
                       received.message.header.payloadLength == sizeof(DecommissionCommandPacket)) {

                DecommissionCommandPacket command{};
                std::memcpy(&command, received.message.payload.data(), sizeof(command));

                HardwareConfigurationFailureReason clearFailure =
                    HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
                bool localConfigurationCleared = false;
                if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
                    localConfigurationCleared = smartNodeConfigurationStore.clearAllConfigurations(
                        relays, *thisSmartNode, clearFailure);
                    xSemaphoreGive(nodeMutex);
                }

                bool identityPersisted = false;
                if (localConfigurationCleared &&
                    xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                    const NodeIdentityStore previousIdentity = identityStore;
                    identityStore.applyDecommission();
                    if (identityStore.persist()) {
                        identityPersisted = true;
                    } else {
                        /* Keep the truthful old identity if durable reset failed. */
                        identityStore = previousIdentity;
                    }
                    xSemaphoreGive(identityMutex);
                }

                char automaticName[EspNowCommunication::NODE_NAME_LENGTH] = {};
                computeAutomaticNodeName(localMac, automaticName, sizeof(automaticName));
                const bool accepted = localConfigurationCleared && identityPersisted;
                if (accepted) {
                    communication.setLocalNodeName(automaticName);
                }

                DecommissionAckPacket acknowledgement{};
                acknowledgement.commandId = command.commandId;
                acknowledgement.success = accepted ? 1U : 0U;
                communication.sendToCentral(EspNowCommunication::MessageType::DECOMMISSION_ACK, acknowledgement);

                ESP_LOGI(TAG, "Decommission command commandId=%u %s (clearReason=%u)%s",
                         static_cast<unsigned int>(command.commandId),
                         accepted ? "APPLIED; name reset" : "REJECTED",
                         static_cast<unsigned int>(clearFailure),
                         accepted ? "" : "; local configuration or identity persistence failed");

                sendIdentityReport();
            } else if (destinationIsLocal &&
                       received.message.header.messageType == EspNowCommunication::MessageType::CONFIGURE_LOAD_COMMAND &&
                       received.message.header.payloadLength == sizeof(ConfigureLoadCommandPacket)) {

                ConfigureLoadCommandPacket command{};
                std::memcpy(&command, received.message.payload.data(), sizeof(command));

                const HardwareConfigurationFailureReason reason = applyConfigureLoadCommand(command);
                ConfigureLoadAcknowledgementPacket acknowledgement{};
                acknowledgement.commandId = command.commandId;
                acknowledgement.relayPin = command.relayPin;
                acknowledgement.success = reason == HardwareConfigurationFailureReason::NONE ? 1U : 0U;
                acknowledgement.failureReason = static_cast<std::uint8_t>(reason);

                communication.sendToCentral(EspNowCommunication::MessageType::CONFIGURE_LOAD_ACK, acknowledgement);
                ESP_LOGI(TAG, "Configure Load command commandId=%u pin=%u result=%s reason=%u",
                         static_cast<unsigned int>(command.commandId), static_cast<unsigned int>(command.relayPin),
                         acknowledgement.success != 0U ? "APPLIED" : "REJECTED",
                         static_cast<unsigned int>(acknowledgement.failureReason));
            } else if (destinationIsLocal &&
                       received.message.header.messageType == EspNowCommunication::MessageType::NODE_REPORT_ACK &&
                       received.message.header.payloadLength == sizeof(NodeReportAcknowledgementPacket)) {

                NodeReportAcknowledgementPacket ack{};
                std::memcpy(&ack, received.message.payload.data(), sizeof(ack));

                char centralText[18] = {};
                formatMacAddressText(centralText, sizeof(centralText), received.message.header.originMacAddress);
                const bool accepted = static_cast<NodeReportAckStatus>(ack.status) == NodeReportAckStatus::ACCEPTED;

                ESP_LOGI(TAG, "SMART_RX NODE_REPORT_ACK seq=%u from=%s status=%s",
                         static_cast<unsigned int>(ack.reportSequenceId), centralText, accepted ? "ACCEPTED" : "REJECTED");

                if (awaitingReportAck && ack.reportSequenceId == lastSentReportSequenceId) {
                    awaitingReportAck = false;
                } else {
                    ESP_LOGW(TAG, "SMART_RX NODE_REPORT_ACK seq=%u: unexpected (not the currently awaited seq=%u, or none pending) - "
                                  "treated as a stale/duplicate ack",
                             static_cast<unsigned int>(ack.reportSequenceId), static_cast<unsigned int>(lastSentReportSequenceId));
                }

            } else if (destinationIsLocal &&
                       received.message.header.messageType == EspNowCommunication::MessageType::DEV_SESSION_COMMAND &&
                       received.message.header.payloadLength == sizeof(DevSessionCommandPacket)) {

                DevSessionCommandPacket command{};
                std::memcpy(&command, received.message.payload.data(), sizeof(command));

                const DevCommandOutcome outcome =
                    static_cast<DevSessionAction>(command.action) == DevSessionAction::START
                        ? applyStartDevelopmentSession() : applyEndDevelopmentSession();

                DevAckPacket acknowledgement{};
                acknowledgement.commandId = command.commandId;
                acknowledgement.success = outcome.success ? 1U : 0U;
                acknowledgement.resultingEnvironment = static_cast<std::uint8_t>(developmentSession.getEnvironment());
                communication.sendToCentral(EspNowCommunication::MessageType::DEV_ACK, acknowledgement);

            } else if (destinationIsLocal &&
                       received.message.header.messageType == EspNowCommunication::MessageType::DEV_SENSOR_INPUT_COMMAND &&
                       received.message.header.payloadLength == sizeof(DevSensorInputCommandPacket)) {

                DevSensorInputCommandPacket command{};
                std::memcpy(&command, received.message.payload.data(), sizeof(command));
                ESP_LOGW(TAG, "DEV_SENSOR_INPUT commandId=%u rejected: Smart Nodes have no INA219; battery sensor is Central-only",
                         static_cast<unsigned int>(command.commandId));
                DevAckPacket acknowledgement{};
                acknowledgement.commandId = command.commandId;
                acknowledgement.success = 0U;
                acknowledgement.resultingEnvironment = static_cast<std::uint8_t>(developmentSession.getEnvironment());
                communication.sendToCentral(EspNowCommunication::MessageType::DEV_ACK, acknowledgement);

            } else if (destinationIsLocal &&
                       received.message.header.messageType == EspNowCommunication::MessageType::FACTORY_RESET_COMMAND &&
                       received.message.header.payloadLength == sizeof(FactoryResetCommandPacket)) {

                FactoryResetCommandPacket command{};
                std::memcpy(&command, received.message.payload.data(), sizeof(command));

                if (command.confirmToken == FACTORY_RESET_CONFIRM_TOKEN) {
                    ESP_LOGW(TAG, "FACTORY_RESET_COMMAND commandId=%u accepted - resetting now",
                             static_cast<unsigned int>(command.commandId));
                    performSmartFactoryReset(); // never returns
                } else {
                    ESP_LOGW(TAG, "FACTORY_RESET_COMMAND commandId=%u rejected: confirm token mismatch",
                             static_cast<unsigned int>(command.commandId));
                    FactoryResetAckPacket acknowledgement{};
                    acknowledgement.commandId = command.commandId;
                    acknowledgement.success = 0U;
                    communication.sendToCentral(EspNowCommunication::MessageType::FACTORY_RESET_ACK, acknowledgement);
                }

            } else if (!destinationIsLocal) {
                const bool fromUpstream = communication.hasUpstreamNode() &&
                                           received.senderMacAddress == communication.getUpstreamNodeMacAddress();

                if (fromUpstream) {
                    /* A downward message (e.g. a relay command for a descendant Node) not addressed to us. */
                    communication.forwardMessageTo(received.message.header.destinationMacAddress, received.message);
                } else if (communication.hasUpstreamNode()) {
                    /* An upward message from a descendant Node, forwarded toward Central. */
                    communication.forwardMessageTo(communication.getUpstreamNodeMacAddress(), received.message);
                }
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if ((now - lastReportTick) >= pdMS_TO_TICKS(NODE_REPORT_PERIOD_MS)) {
            lastReportTick = now;
            ++reportSequenceId;

            const NodeReportPacket packet = buildNodeReportPacket(
                localMac, communication.getHopCountToCentral(), reportSequenceId);

            ESP_LOGI(TAG, "SMART_TX NODE_REPORT seq=%u node=%s hop=%u loads=%u",
                     static_cast<unsigned int>(reportSequenceId), packet.nodeName,
                     static_cast<unsigned int>(packet.hopCountToCentral), static_cast<unsigned int>(packet.numberOfLoads));

            for (std::size_t i = 0U; i < packet.numberOfLoads && i < MAX_LOADS_PER_NODE_PACKET; ++i) {
                const LoadReportPacket &loadPacket = packet.loads[i];
                ESP_LOGI(TAG, "LOAD_TX seq=%u pin=%u name=%s mode=%u confirmed=%u confirmedValid=%s "
                              "health=%u ratedV=%.3f ratedI=%.3f plannedP=%.3f",
                         static_cast<unsigned int>(reportSequenceId),
                         static_cast<unsigned int>(loadPacket.relayPin), loadPacket.name,
                         static_cast<unsigned int>(loadPacket.mode),
                         static_cast<unsigned int>(loadPacket.confirmedRelayState),
                         loadPacket.confirmedRelayStateValid != 0U ? "true" : "false",
                         static_cast<unsigned int>(loadPacket.availability),
                         static_cast<double>(loadPacket.nominalVoltageVolts),
                         static_cast<double>(loadPacket.nominalCurrentAmps),
                         static_cast<double>(loadPacket.nominalVoltageVolts * loadPacket.nominalCurrentAmps));
            }

            const bool sent = communication.sendToCentral(EspNowCommunication::MessageType::NODE_REPORT, packet);
            ESP_LOGI(TAG, "NODE_REPORT sequence=%u %s", static_cast<unsigned int>(reportSequenceId),
                     sent ? "SENT" : "FAILED (no upstream route yet)");

            if (sent) {
                awaitingReportAck = true;
                lastSentReportSequenceId = reportSequenceId;
                reportAckSentTick = now;
            } else {
                awaitingReportAck = false;
            }
        }
    }
}


/**
 * Watchdog/Diagnostics Task (Section 4.6.1, ~60s): reports link/relay
 * health (Node Link Loss handling — this Node holds its last commanded relay
 * state while a route to Central is unavailable). Discovery retry itself is owned entirely by
 * espNowCommunicationTask (Section "Smart Node Must Not Block On
 * Central"); this task only reports the link's current status.
 */
void watchdogTask(void *parameter)
{
    (void)parameter;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_PERIOD_MS));

        ESP_LOGI(TAG, "================ WATCHDOG ================");
        ESP_LOGI(TAG, "Upstream route: %s", communication.hasUpstreamNode() ? "OK" : "LOST (retried automatically)");
        relays.printDiagnosticReport();
        sendIdentityReport();
        ESP_LOGI(TAG, "===========================================");
    }
}


/**
 * One concise, authoritative boot-state block (Section "Clean Boot
 * Summary") - every value here is read live from the modules that own it,
 * never hardcoded text.
 */
void printSmartBootSummary(const EspNowCommunication::MacAddress& localMac)
{
    char macText[18] = {};
    formatMacAddressText(macText, sizeof(macText), localMac);

    const std::size_t loadCount = thisSmartNode != nullptr ? thisSmartNode->getNumberOfLoads() : 0U;

    ESP_LOGI(TAG, "========== KILOWATTS SMART NODE BOOT STATE ==========");
    ESP_LOGI(TAG, "Environment           : %s", toText(developmentSession.getEnvironment()));
    ESP_LOGI(TAG, "MAC                   : %s", macText);
    ESP_LOGI(TAG, "Firmware Version      : %s", KILOWATTS_FIRMWARE_VERSION);
    ESP_LOGI(TAG, "Commissioning State   : %s", toText(identityStore.getLifecycleState()));
    ESP_LOGI(TAG, "Node Name             : %s", communication.getLocalNodeName());
    ESP_LOGI(TAG, "Per-load sensors      : NONE (ratings only; battery INA219 is at Central)");
    ESP_LOGI(TAG, "Local Branches        : %u", static_cast<unsigned int>(loadCount));
    ESP_LOGI(TAG, "Local Loads           : %u", static_cast<unsigned int>(loadCount));
    ESP_LOGI(TAG, "Upstream Route        : %s", communication.hasUpstreamNode() ? "KNOWN" : "NONE YET");
    ESP_LOGI(TAG, "Development Session   : %s", developmentSession.isActive() ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "Verified Relay GPIOs  : %u (only these can actuate)",
             static_cast<unsigned int>(SmartNodeConfig::getVerifiedRelayPinCount()));
    ESP_LOGI(TAG, "======================================================");
}


/**
 * Minimal engineering test console (mirrors src/central/main.cpp's own
 * consoleTask() exactly — see its documentation for why this exists: the
 * only way to verify Development Session / factory reset behaviour on
 * real hardware when Wi-Fi/MQTT connectivity is unavailable. Calls exactly
 * the same local functions a received DEV_SESSION_COMMAND/
 * DEV_SENSOR_INPUT_COMMAND/FACTORY_RESET_COMMAND would call - never a
 * second, divergent code path. Not a production control surface.
 *
 * Commands: dev_start | dev_end | factory_reset
 */
void consoleTask(void *parameter)
{
    (void)parameter;

    char line[160];
    ESP_LOGI(TAG, "CONSOLE: engineering test console ready "
                  "(dev_start | dev_end | factory_reset)");

    while (true) {
        if (std::fgets(line, sizeof(line), stdin) == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(200U));
            continue;
        }

        std::size_t length = std::strlen(line);
        while (length > 0U && (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0U) {
            continue;
        }

        if (std::strcmp(line, "dev_start") == 0) {
            const DevCommandOutcome outcome = applyStartDevelopmentSession();
            ESP_LOGI(TAG, "CONSOLE: dev_start -> accepted=%s reason=%s", outcome.success ? "Yes" : "No", outcome.reason);

        } else if (std::strcmp(line, "dev_end") == 0) {
            const DevCommandOutcome outcome = applyEndDevelopmentSession();
            ESP_LOGI(TAG, "CONSOLE: dev_end -> accepted=%s reason=%s", outcome.success ? "Yes" : "No", outcome.reason);

        } else if (std::strcmp(line, "factory_reset") == 0) {
            ESP_LOGW(TAG, "CONSOLE: factory_reset requested - rebooting now");
            performSmartFactoryReset(); // never returns

        } else {
            ESP_LOGW(TAG, "CONSOLE: unrecognised command '%s'", line);
        }
    }
}

} // namespace


extern "C" void app_main()
{
    if (!communication.initialize()) {
        ESP_LOGE(TAG, "ESP-NOW initialization failed");
        return;
    }

    currentTimeProvider.initializeTimeSynchronization();
    currentTimeProvider.printDiagnosticReport();

    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();

    /*
     * Section "New Smart Node Boot": a Node with no valid persisted
     * commissioning record boots UNCOMMISSIONED (NodeIdentityStore's own
     * default) and advertises an automatically derived name; loadPersisted()
     * restores a previously commissioned identity/name instead when one
     * exists.
     */
    const bool identityRestored = identityStore.loadPersisted();
    ESP_LOGI(TAG, "NVS_RESTORE: identity=%s state=%s name=%s", identityRestored ? "RESTORED" : "ABSENT",
             toText(identityStore.getLifecycleState()),
             identityStore.getFriendlyName()[0] != '\0' ? identityStore.getFriendlyName() : "(none)");

    char automaticName[EspNowCommunication::NODE_NAME_LENGTH] = {};
    computeAutomaticNodeName(localMac, automaticName, sizeof(automaticName));

    if (identityStore.getLifecycleState() == NodeLifecycleState::COMMISSIONED ||
        identityStore.getLifecycleState() == NodeLifecycleState::OPERATIONAL) {
        communication.setLocalNodeName(identityStore.getFriendlyName());
    } else {
        communication.setLocalNodeName(automaticName);
    }

    ESP_LOGI(TAG, "This Smart Node name: %s state=%s", communication.getLocalNodeName(),
             toText(identityStore.getLifecycleState()));

    nodeMutex = xSemaphoreCreateMutex();
    identityMutex = xSemaphoreCreateMutex();
    relayCommandQueue = xQueueCreate(8U, sizeof(RelayCommandQueueItem));
    thisSmartNode = new Node(localMac);

    const bool hardwareConfigurationRestored = smartNodeConfigurationStore.loadPersisted();
    HardwareConfigurationFailureReason restoreFailure = HardwareConfigurationFailureReason::NONE;
    const bool canRestoreHardware =
        identityStore.getLifecycleState() == NodeLifecycleState::COMMISSIONED ||
        identityStore.getLifecycleState() == NodeLifecycleState::OPERATIONAL;
    if (hardwareConfigurationRestored && canRestoreHardware &&
        persistedRelayConfigurationMatchesBoardProfile()) {
        if (!smartNodeConfigurationStore.applyPersistedConfigurations(
                relays, *thisSmartNode, restoreFailure)) {
            ESP_LOGE(TAG, "NVS_RESTORE: Smart Node hardware configuration could not be applied (reason=%u)",
                     static_cast<unsigned int>(restoreFailure));
        }
    } else if (hardwareConfigurationRestored) {
        ESP_LOGW(TAG, "NVS_RESTORE: retaining hardware configuration without applying it while Node is uncommissioned or the board profile is not verified");
    }
    ESP_LOGI(TAG, "NVS_RESTORE: hardwareConfiguration=%s configuredLoads=%u",
             hardwareConfigurationRestored ? "RESTORED" : "ABSENT",
             static_cast<unsigned int>(smartNodeConfigurationStore.getNumberOfConfigurations()));
    ESP_LOGI(TAG, "LOAD_POWER_SOURCE: installer ratings only (no per-load INA219 on Smart Nodes)");
    sendIdentityReport();

    /*
     * Section "Smart Node Must Not Block On Central": local functionality
     * (relay control and watchdog) starts immediately and
     * unconditionally — it never waits for a successful upstream ESP-NOW
     * discovery first. Discovery itself is retried asynchronously inside
     * espNowCommunicationTask() once it starts running, without blocking
     * any other task; if Central stays unreachable, relay/
     * watchdog simply keep running on their own with the last safe relay
     * state held.
     */
    xTaskCreate(relayControlTask, "relay_ctrl", 4096U, nullptr, 6U, nullptr);
    xTaskCreate(espNowCommunicationTask, "espnow_app", 4096U, nullptr, 5U, nullptr);
    xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr);
    xTaskCreate(consoleTask, "console", 4096U, nullptr, 3U, nullptr);

    ESP_LOGI(TAG, "%s is ready: relay control and ESP-NOW tasks running", communication.getLocalNodeName());
    printSmartBootSummary(localMac);
}

#endif // DEVICE_ROLE_SMART
