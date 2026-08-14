#if defined(DEVICE_ROLE_SMART)

/**
 * @file main.cpp
 * @brief Smart Node orchestration entry point.
 *
 * This file only instantiates real modules, creates the FreeRTOS
 * tasks/queues/synchronization that wire them together, and starts the
 * system — every real responsibility (sensing, filtering, relay
 * actuation, ESP-NOW transport, schedule evaluation, time) lives in the
 * lib/ module that owns it.
 *
 * Pipeline implemented here (Section 4.6, "Smart Node" role):
 *
 *   Sensor Acquisition Task (~1s)
 *       INA219Monitor::readFilteredMeasurements() -> Load::setMeasurements()
 *   ESP-NOW Communication Task (event-driven + periodic report)
 *       builds/sends NodeReportPacket, learns/forwards other traffic,
 *       hands RELAY_COMMAND messages to the Relay Control Task's queue
 *   Relay Control Task (event-driven)
 *       RelayController::setRelayState() -> readBackState() ->
 *       RelayCommandAcknowledgementPacket back to Central
 *   Watchdog/Diagnostics Task (~60s)
 *       link/sensor/relay health, re-discovers Central if the route was
 *       lost, sends a periodic IDENTITY_REPORT
 *
 * A freshly flashed/uncommissioned Node has zero Loads/Branches/sensors
 * (Section "New Smart Node Boot") until a COMMISSION_COMMAND from Central
 * assigns it a friendly name (see lib/NodeIdentityStore,
 * lib/CommissioningPackets) — handled inline inside the ESP-NOW
 * Communication Task's own receive dispatch, the same place RELAY_COMMAND
 * already is.
 *
 * nodeMutex protects thisSmartNode (the Node/Load objects), since the
 * Sensor Acquisition Task, the Relay Control Task and the ESP-NOW
 * Communication Task's report builder all read or write Load state
 * concurrently. identityMutex separately protects identityStore, written
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
#include "INA219Monitor.h"
#include "Load.h"
#include "Node.h"
#include "NodeIdentityStore.h"
#include "NodeLifecycle.h"
#include "NodeReportPackets.h"
#include "RadioConfig.h"
#include "RelayController.h"
#include "SmartNodeConfig.h"

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

static const char *TAG = "SMART_MAIN";

namespace {

constexpr std::uint32_t SENSOR_ACQUISITION_PERIOD_MS = 1000U;
constexpr std::uint32_t NODE_REPORT_PERIOD_MS = 2000U;
constexpr std::uint32_t WATCHDOG_PERIOD_MS = 60000U;

/** Section "Application-Level Node Report Ack": how long to wait for Central's NODE_REPORT_ACK before logging CENTRAL_ACK_TIMEOUT. */
constexpr std::uint32_t NODE_REPORT_ACK_TIMEOUT_MS = 3000U;

/** A Development Session sensor override may target an I2C address with no owning Load yet - see applySetSensorInput(). */
constexpr std::uint8_t DEV_SENSOR_RELAY_PIN_SENTINEL = 0xFFU;

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
INA219Monitor sensors;
RelayController relays;
ChipInfo chipInfo;
NodeIdentityStore identityStore;

/**
 * This Node's explicit runtime Development Session (Section "Development
 * Session Is Explicit") - always PRODUCTION on boot, never inferred from
 * missing hardware or a compile-time flag. Only a received
 * DEV_SESSION_COMMAND/DEV_SENSOR_INPUT_COMMAND (or the local engineering
 * console) ever calls start()/end()/setSensorOverride()/
 * clearSensorOverride(). Guarded by identityMutex, the same mutex already
 * used for this Node's other session-level state (identityStore).
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
 * Initializes this Node's I2C bus - a genuine per-board hardware fact
 * (SmartNodeConfig::i2cBusConfiguration()), not installation
 * configuration. A freshly flashed/uncommissioned Node registers zero
 * Loads, zero sensors and zero relays (Section "New Smart Node Boot"):
 * Branches/Loads/sensors become a later phase's runtime-commissioned,
 * NVS-persisted concern, not something this file defines.
 */
void initializeHardwareBus()
{
    sensors.initializeBus(SmartNodeConfig::i2cBusConfiguration());
}


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
 * Sensor Acquisition Task (Section 4.6.1, ~1s): reads every configured
 * Load's INA219 through the filtered measurement path (real hardware or
 * an active Development Session override, both through the exact same
 * INA219Monitor interface — see lib/INA219Monitor, lib/DevelopmentSession)
 * and stores the result on that Load's own object, guarded by nodeMutex.
 */
void sensorAcquisitionTask(void *parameter)
{
    (void)parameter;

    while (true) {
        if (xSemaphoreTake(nodeMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            for (std::size_t i = 0U; i < thisSmartNode->getNumberOfLoads(); ++i) {
                Load *load = thisSmartNode->getLoad(i);
                if (load == nullptr) {
                    continue;
                }

                LoadMeasurements filtered{};
                if (sensors.readFilteredMeasurementsForRelayPin(load->getRelayPin(), filtered)) {
                    load->setMeasurements(filtered);
                }
            }
            xSemaphoreGive(nodeMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_ACQUISITION_PERIOD_MS));
    }
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
            loadPacket.runningWatts = load->getPower().runningWatts;
            loadPacket.startupWatts = load->getPower().startupWatts;

            /*
             * Branch configuration (I_branch,max) is a later phase's
             * runtime-commissioned concern (GPIO/Branch CRUD) - there is
             * currently no way for a Load to exist at all without it, so
             * this loop never actually runs yet, but the field is kept at
             * a safe 0.0F rather than removed, ready for that phase to
             * populate it from real Branch configuration.
             */
            loadPacket.branchMaximumCurrentAmps = 0.0F;

            const LoadMeasurements measurements = load->getMeasurements();
            loadPacket.measuredVoltageVolts = measurements.voltageVolts;
            loadPacket.measuredCurrentAmps = measurements.currentAmps;
            loadPacket.measuredPowerWatts = measurements.powerWatts;

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
 * "Development Session Is Explicit") — the only functions in this file
 * that ever call DevelopmentSession::start()/end()/setSensorOverride()/
 * clearSensorOverride() or INA219Monitor::setDevelopmentOverride()/
 * clearDevelopmentOverride(). Called from both the ESP-NOW receive
 * dispatch (a DEV_SESSION_COMMAND/DEV_SENSOR_INPUT_COMMAND Central relayed
 * here) and consoleTask() (the local engineering test console) — never a
 * second, divergent code path. Mirrors src/central/main.cpp's own
 * handleDevelopmentCommand() local-target branch.
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


DevCommandOutcome applySetSensorInput(std::uint8_t i2cAddress, float voltageVolts, float currentAmps)
{
    DevCommandOutcome outcome{};

    if (xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) != pdTRUE) {
        std::snprintf(outcome.reason, sizeof(outcome.reason), "internal: could not acquire state lock");
        return outcome;
    }

    if (!developmentSession.isActive()) {
        std::snprintf(outcome.reason, sizeof(outcome.reason), "no active Development Session; call START_SESSION first");
        xSemaphoreGive(identityMutex);
        return outcome;
    }

    if (sensors.findSensorByI2CAddress(i2cAddress) == nullptr) {
        sensors.addSensor(INA219Monitor::INA219SensorConfiguration{
            i2cAddress, DEV_SENSOR_RELAY_PIN_SENTINEL, 0.005F, 60.0F, 0.2F
        });
        ESP_LOGW(TAG, "DEV_SENSOR_REGISTER: node=%s i2c=0x%02X (temporary, Development-Session-only registration)",
                 communication.getLocalNodeName(), static_cast<unsigned int>(i2cAddress));
    }

    ESP_LOGW(TAG, "DEV_INPUT node=%s i2c=0x%02X V=%.3f I=%.3f", communication.getLocalNodeName(),
             static_cast<unsigned int>(i2cAddress), static_cast<double>(voltageVolts), static_cast<double>(currentAmps));

    const LoadMeasurements rawOverride{voltageVolts, currentAmps, voltageVolts * currentAmps};
    outcome.success = sensors.setDevelopmentOverride(i2cAddress, rawOverride);
    developmentSession.setSensorOverride(i2cAddress, voltageVolts, currentAmps);

    xSemaphoreGive(identityMutex);

    std::snprintf(outcome.reason, sizeof(outcome.reason), outcome.success ? "sensor input armed" : "failed to arm sensor override");
    return outcome;
}


DevCommandOutcome applyClearSensorOverride(std::uint8_t i2cAddress)
{
    DevCommandOutcome outcome{};
    if (xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
        outcome.success = developmentSession.clearSensorOverride(i2cAddress) && sensors.clearDevelopmentOverride(i2cAddress);
        xSemaphoreGive(identityMutex);
    }
    std::snprintf(outcome.reason, sizeof(outcome.reason), outcome.success ? "override cleared" : "no override was armed for this address");
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

    ESP_LOGW(TAG, "FACTORY_RESET: erasing NVS (commissioning identity/name, INA219 calibration) - "
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
                    accepted = identityStore.applyCommission(command.friendlyName);
                    identityStore.persist();
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

                if (xSemaphoreTake(identityMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                    identityStore.applyDecommission();
                    identityStore.persist();
                    xSemaphoreGive(identityMutex);
                }

                char automaticName[EspNowCommunication::NODE_NAME_LENGTH] = {};
                computeAutomaticNodeName(localMac, automaticName, sizeof(automaticName));
                communication.setLocalNodeName(automaticName);

                DecommissionAckPacket acknowledgement{};
                acknowledgement.commandId = command.commandId;
                acknowledgement.success = 1U;
                communication.sendToCentral(EspNowCommunication::MessageType::DECOMMISSION_ACK, acknowledgement);

                ESP_LOGI(TAG, "Decommission command commandId=%u applied; name reset to '%s'",
                         static_cast<unsigned int>(command.commandId), automaticName);

                sendIdentityReport();
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

                const DevCommandOutcome outcome = command.clearOverride != 0U
                    ? applyClearSensorOverride(command.i2cAddress)
                    : applySetSensorInput(command.i2cAddress, command.voltageVolts, command.currentAmps);

                DevAckPacket acknowledgement{};
                acknowledgement.commandId = command.commandId;
                acknowledgement.success = outcome.success ? 1U : 0U;
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
                              "health=%u V=%.3f I=%.3f P=%.3f",
                         static_cast<unsigned int>(reportSequenceId),
                         static_cast<unsigned int>(loadPacket.relayPin), loadPacket.name,
                         static_cast<unsigned int>(loadPacket.mode),
                         static_cast<unsigned int>(loadPacket.confirmedRelayState),
                         loadPacket.confirmedRelayStateValid != 0U ? "true" : "false",
                         static_cast<unsigned int>(loadPacket.availability),
                         static_cast<double>(loadPacket.measuredVoltageVolts),
                         static_cast<double>(loadPacket.measuredCurrentAmps),
                         static_cast<double>(loadPacket.measuredPowerWatts));
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
 * Watchdog/Diagnostics Task (Section 4.6.1, ~60s): reports link/sensor/
 * relay health (Node Link Loss handling — this Node holds its last
 * commanded relay state and keeps sensing locally the whole time a route
 * to Central is unavailable). Discovery retry itself is owned entirely by
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
        sensors.printDiagnosticReport();
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
    ESP_LOGI(TAG, "Local Sensors         : %u", static_cast<unsigned int>(sensors.getNumberOfSensors()));
    ESP_LOGI(TAG, "Local Branches        : %u", static_cast<unsigned int>(loadCount));
    ESP_LOGI(TAG, "Local Loads           : %u", static_cast<unsigned int>(loadCount));
    ESP_LOGI(TAG, "Upstream Route        : %s", communication.hasUpstreamNode() ? "KNOWN" : "NONE YET");
    ESP_LOGI(TAG, "Development Session   : %s", developmentSession.isActive() ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "Physical Actuation    : ENABLED");
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
 * Commands: dev_start | dev_set <hexI2cAddress> <voltage> <current> |
 * dev_clear <hexI2cAddress> | dev_end | factory_reset
 */
void consoleTask(void *parameter)
{
    (void)parameter;

    char line[160];
    ESP_LOGI(TAG, "CONSOLE: engineering test console ready "
                  "(dev_start | dev_set <hexI2c> <V> <I> | dev_clear <hexI2c> | dev_end | factory_reset)");

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

        unsigned int i2cAddress = 0U;
        float voltageVolts = 0.0F;
        float currentAmps = 0.0F;

        if (std::strcmp(line, "dev_start") == 0) {
            const DevCommandOutcome outcome = applyStartDevelopmentSession();
            ESP_LOGI(TAG, "CONSOLE: dev_start -> accepted=%s reason=%s", outcome.success ? "Yes" : "No", outcome.reason);

        } else if (std::strcmp(line, "dev_end") == 0) {
            const DevCommandOutcome outcome = applyEndDevelopmentSession();
            ESP_LOGI(TAG, "CONSOLE: dev_end -> accepted=%s reason=%s", outcome.success ? "Yes" : "No", outcome.reason);

        } else if (std::sscanf(line, "dev_set %x %f %f", &i2cAddress, &voltageVolts, &currentAmps) == 3) {
            const DevCommandOutcome outcome =
                applySetSensorInput(static_cast<std::uint8_t>(i2cAddress), voltageVolts, currentAmps);
            ESP_LOGI(TAG, "CONSOLE: dev_set 0x%02X -> accepted=%s reason=%s",
                     i2cAddress, outcome.success ? "Yes" : "No", outcome.reason);

        } else if (std::sscanf(line, "dev_clear %x", &i2cAddress) == 1) {
            const DevCommandOutcome outcome = applyClearSensorOverride(static_cast<std::uint8_t>(i2cAddress));
            ESP_LOGI(TAG, "CONSOLE: dev_clear 0x%02X -> accepted=%s reason=%s",
                     i2cAddress, outcome.success ? "Yes" : "No", outcome.reason);

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

    /*
     * Section "Uncommissioned Node Reports NONE": a freshly flashed/reset
     * Node has zero registered sensors, so its honest sensor input source
     * is NONE - never DEVELOPMENT, never fabricated. Development Session
     * per-sensor MeasurementSource (see INA219Monitor::getLastMeasurementSource())
     * only becomes meaningful once a sensor is actually registered, which
     * only ever happens through commissioning or an explicit
     * DEV_SENSOR_INPUT_COMMAND.
     */
    ESP_LOGI(TAG, "SENSOR_INPUT_SOURCE: %s (sensors configured=%u)",
             sensors.getNumberOfSensors() == 0U ? "NONE" : "SEE PER-SENSOR MEASUREMENT SOURCE",
             static_cast<unsigned int>(sensors.getNumberOfSensors()));

    nodeMutex = xSemaphoreCreateMutex();
    identityMutex = xSemaphoreCreateMutex();
    relayCommandQueue = xQueueCreate(8U, sizeof(RelayCommandQueueItem));
    thisSmartNode = new Node(localMac);

    initializeHardwareBus();
    sendIdentityReport();

    /*
     * Section "Smart Node Must Not Block On Central": local functionality
     * (sensing, relay control, watchdog) starts immediately and
     * unconditionally — it never waits for a successful upstream ESP-NOW
     * discovery first. Discovery itself is retried asynchronously inside
     * espNowCommunicationTask() once it starts running, without blocking
     * any other task; if Central stays unreachable, sensing/relay/
     * watchdog simply keep running on their own with the last safe relay
     * state held.
     */
    xTaskCreate(sensorAcquisitionTask, "sensor_acq", 4096U, nullptr, 5U, nullptr);
    xTaskCreate(relayControlTask, "relay_ctrl", 4096U, nullptr, 6U, nullptr);
    xTaskCreate(espNowCommunicationTask, "espnow_app", 4096U, nullptr, 5U, nullptr);
    xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr);
    xTaskCreate(consoleTask, "console", 4096U, nullptr, 3U, nullptr);

    ESP_LOGI(TAG, "%s is ready: sensing, relay control and ESP-NOW tasks running", communication.getLocalNodeName());
    printSmartBootSummary(localMac);
}

#endif // DEVICE_ROLE_SMART
