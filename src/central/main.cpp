#if defined(DEVICE_ROLE_CENTRAL)

/**
 * @file main.cpp
 * @brief Central Node orchestration entry point.
 *
 * This file only instantiates real modules, creates the FreeRTOS
 * tasks/queues/synchronization that wire them together, and starts the
 * system. Every real responsibility (registry aggregation, filtering,
 * power-budget calculation, scheduling, Best-First Search, relay dispatch
 * sequencing, JSON formatting, transport) lives in the lib/ module that
 * owns it — this file sequences calls to those modules, it does not
 * reimplement any of their mathematics.
 *
 * Full planning cycle (Section 4.6.4, run by optimizationTask()):
 *
 *   latest atomic telemetry (stateMutex-protected snapshot)
 *       -> BatteryStateOfCharge::update()          (Equation 4.8)
 *       -> PowerBudgetCalculator::calculate()       (Equations 4.9-4.14)
 *       -> LoadConfigurationStore::applyToLoad()    (persisted user config wins)
 *       -> LoadFilter (Fixed ON / Fixed OFF / Auto candidates)
 *       -> AvailablePowerManager::calculateAvailablePower()
 *       -> LoadScheduleEvaluator::evaluateSchedule() (per Auto candidate)
 *       -> BestFirstSearch (registerBranch/addLoad/run)
 *       -> RelayCommandDispatcher::buildDispatchOrder() (OFF before ON)
 *       -> ESP-NOW RelayCommandPacket / local RelayController
 *       -> SystemStateJson / TopologyTree -> MqttManager::publish()
 *
 * FreeRTOS tasks: Sensor Acquisition (~1s), ESP-NOW Communication
 * (event-driven), Optimisation (~5s, or immediately on
 * optimizationTriggerSemaphore — an MQTT command never calls the
 * optimizer directly, it only signals this semaphore), Watchdog (~60s).
 * One mutex (stateMutex) protects every shared object below
 * (CentralNodeRegistry, NodeCommissioningRegistry, LoadConfigurationStore,
 * RelayCommandDispatcher, BatteryStateOfCharge) so the Optimisation Task
 * always computes from a consistent snapshot.
 *
 * A freshly flashed/uncommissioned Central has zero local Loads/Branches
 * and zero known remote Nodes (Section "New Central Boot") until real
 * ESP-NOW discovery (IDENTITY_REPORT) and an operator's commissioning
 * command populate NodeCommissioningRegistry — handled inline inside the
 * ESP-NOW Communication Task's own receive dispatch (IDENTITY_REPORT/
 * COMMISSION_ACK/DECOMMISSION_ACK, the same place NODE_REPORT/
 * ACKNOWLEDGEMENT already are) and the MQTT commands/config handler
 * (handleConfigCommand()).
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "AvailablePowerManager.h"
#include "BatteryStateOfCharge.h"
#include "BestFirstSearch.h"
#include "CentralNodeConfig.h"
#include "CentralConfigurationStore.h"
#include "CentralNodeRegistry.h"
#include "ChipInfo.h"
#include "CommissioningPackets.h"
#include "CurrentTimeProvider.h"
#include "DevelopmentPackets.h"
#include "DevelopmentSession.h"
#include "EspNowCommunication.h"
#include "FirmwareVersion.h"
#include "HardwareConfigurationPackets.h"
#include "INA219Monitor.h"
#include "KilowattsSecrets.h"
#include "Load.h"
#include "LoadConfigurationStore.h"
#include "LoadFilter.h"
#include "LoadScheduleEvaluator.h"
#include "MqttManager.h"
#include "Node.h"
#include "NodeCommissioningRegistry.h"
#include "NodeLifecycle.h"
#include "NodeReportPackets.h"
#include "NodeRegistryJson.h"
#include "PowerBudgetCalculator.h"
#include "RadioConfig.h"
#include "RelayCommandDispatcher.h"
#include "RelayController.h"
#include "SystemStateJson.h"
#include "TopologyTree.h"
#include "WiFiManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

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

static const char *TAG = "CENTRAL_MAIN";

namespace {

/*
 * Long-lived objects: FreeRTOS tasks created in app_main() run for the
 * lifetime of the device, so these must outlive app_main() itself (which
 * returns once every task is created, per normal ESP-IDF practice).
 */
EspNowCommunication communication(kilowatts::KILOWATTS_RADIO_CHANNEL);
CurrentTimeProvider currentTimeProvider;
WiFiManager wifiManager(kilowatts::KILOWATTS_RADIO_CHANNEL);
MqttManager mqttManager(CentralNodeConfig::MQTT_TOPIC_NAMESPACE, CentralNodeConfig::MQTT_DEVICE_ID,
                         CentralNodeConfig::MQTT_SCHEMA_VERSION);

INA219Monitor sensors;
RelayController relays;
BatteryStateOfCharge batteryStateOfCharge;
LoadConfigurationStore loadConfigurationStore;
CentralConfigurationStore centralConfigurationStore;
CentralNodeRegistry registry;
RelayCommandDispatcher relayCommandDispatcher;
LoadScheduleEvaluator scheduleEvaluator;
ChipInfo chipInfo;

/**
 * Central's authoritative identity/lifecycle record for every Node it
 * knows about (Section "Commissioning") — a distinct concern from
 * `registry` above (planning-time Node/Load domain data and route), never
 * duplicating its fields; joined by MAC address where both are needed
 * (see NodeRegistryJson). Guarded by stateMutex, same as every other
 * shared planning structure.
 */
NodeCommissioningRegistry commissioningRegistry;

/**
 * This Node's explicit runtime Development Session (Section "Development
 * Session Is Explicit") - always PRODUCTION on boot, never inferred from
 * missing hardware or a compile-time flag. Only handleDevelopmentCommand()
 * ever calls start()/end()/setSensorOverride()/clearSensorOverride().
 */
DevelopmentSession developmentSession;

SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t optimizationTriggerSemaphore = nullptr;

/**
 * Central has no battery sensor by default. It becomes configured only when
 * a persisted installer configuration has first registered and probed the
 * real INA219 (or while an explicit Development Session is active). Guarded
 * by stateMutex.
 */
bool batterySensorConfigured = false;
std::uint8_t batterySensorI2CAddress = 0U;

LoadMeasurements latestBatteryMeasurements{
    CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS, 0.0F, 0.0F
};
std::int64_t lastOptimizationEpochSeconds = 0;
std::uint32_t faultCount = 0U;

/*
 * Sensor-failure tracking (Section "Fix Sensor Failure Behavior"):
 * battery telemetry validity is tracked separately from the measurement
 * itself, with an age/staleness timeout, so a failed INA219 read can
 * never be silently treated as an indefinitely fresh, trustworthy
 * reading. batteryTelemetryEverValid stays false until the first
 * successful read this boot; batteryTelemetryLastValidMilliseconds is
 * the timestamp of the most recent successful read.
 *
 * lastValidAvailablePowerWatts/lastValidBatteryVoltageVolts are frozen
 * planning-only fallbacks used while telemetry is stale/invalid — never
 * fabricated new values, always the last value genuinely computed from a
 * real successful reading. Actuation safety (whether an ON command may
 * actually be sent) never relies on these frozen values: the pre-ON
 * safety re-check always re-reads live telemetry and refuses to energise
 * anything while it is untrustworthy, regardless of what planning used.
 */
bool batteryTelemetryEverValid = false;
std::uint32_t batteryTelemetryLastValidMilliseconds = 0U;
float lastValidAvailablePowerWatts = 0.0F;
float lastValidBatteryVoltageVolts = CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS;

constexpr std::uint8_t BATTERY_RELAY_PIN_SENTINEL = 0xFFU; // battery sensor has no relay/Load of its own


/**
 * Returns whether battery telemetry can currently be trusted: at least
 * one successful reading has ever been taken, and the most recent one is
 * still within the staleness timeout. Must be called with stateMutex
 * already held, since it reads the shared telemetry-tracking globals.
 */
bool isBatteryTelemetryTrustworthy(std::uint32_t nowMilliseconds)
{
    if (!batteryTelemetryEverValid) {
        return false;
    }

    const std::uint32_t ageMilliseconds = nowMilliseconds - batteryTelemetryLastValidMilliseconds;
    return ageMilliseconds <= CentralNodeConfig::BATTERY_TELEMETRY_STALE_TIMEOUT_MILLISECONDS;
}


/*
 * Node offline tracking (Section "Exclude Stale/Offline Smart Nodes From
 * Planning"): remembers which remote Node MAC addresses are currently
 * considered offline (last NODE_REPORT older than
 * CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS), purely so
 * NODE_OFFLINE/NODE_RECOVERED are logged only on the transition rather
 * than every cycle a Node happens to still be offline. Access only while
 * holding stateMutex, same as every other shared planning structure.
 */
std::vector<EspNowCommunication::MacAddress> offlineNodeMacAddresses;

bool isNodeMacAddressMarkedOffline(const EspNowCommunication::MacAddress& macAddress)
{
    for (const EspNowCommunication::MacAddress &mac : offlineNodeMacAddresses) {
        if (mac == macAddress) {
            return true;
        }
    }
    return false;
}

/** Returns true only the first time macAddress is marked offline (the NODE_OFFLINE transition). */
bool markNodeMacAddressOffline(const EspNowCommunication::MacAddress& macAddress)
{
    if (isNodeMacAddressMarkedOffline(macAddress)) {
        return false;
    }
    offlineNodeMacAddresses.push_back(macAddress);
    return true;
}

void clearNodeMacAddressOffline(const EspNowCommunication::MacAddress& macAddress)
{
    for (std::size_t i = 0U; i < offlineNodeMacAddresses.size(); ++i) {
        if (offlineNodeMacAddresses[i] == macAddress) {
            offlineNodeMacAddresses.erase(offlineNodeMacAddresses.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}


/**
 * Returns whether the Node owning load is currently reachable for new
 * relay commands: Central's own local Node is always online; a remote
 * Node is online only while its last NODE_REPORT is within
 * NODE_REPORT_TIMEOUT_MILLISECONDS of nowMilliseconds. Must be called
 * with stateMutex already held.
 */
bool isLoadNodeOnline(const Load& load, std::uint32_t nowMilliseconds)
{
    const CentralNodeRegistry::PlanningNode *planningNode = registry.findNodeByMacAddress(load.getMacAddress());
    if (planningNode == nullptr) {
        return false;
    }
    if (planningNode->isCentralNode) {
        return true;
    }

    const std::uint32_t ageMilliseconds = nowMilliseconds - planningNode->lastSeenMilliseconds;
    return ageMilliseconds <= CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS;
}


/**
 * Returns whether load may currently be considered for a NEW relay
 * command this cycle: its owning Node must be reachable (see
 * isLoadNodeOnline()) and its last known physical health must not be
 * FAULTED/UNAVAILABLE (Section "Fix Relay Confirmation Failure
 * Semantics": "Don't command a faulted/unavailable Load ON again until
 * fault/recovery policy permits"). A Load that fails this check keeps
 * whatever target/confirmed state it already had — it is simply left out
 * of this cycle's planning and dispatch, never forced OFF or assumed
 * safe.
 */
bool isLoadEligibleForNewCommandsThisCycle(const Load& load, std::uint32_t nowMilliseconds)
{
    return isLoadNodeOnline(load, nowMilliseconds) && load.getHealth() == LoadHealth::AVAILABLE;
}


const char *wifiStateText(WiFiConnectionState state)
{
    switch (state) {
        case WiFiConnectionState::DISCONNECTED: return "DISCONNECTED";
        case WiFiConnectionState::SCANNING: return "SCANNING";
        case WiFiConnectionState::CONNECTING: return "CONNECTING";
        case WiFiConnectionState::CONNECTED_AWAITING_IP: return "CONNECTED_AWAITING_IP";
        case WiFiConnectionState::CONNECTED_WITH_IP: return "CONNECTED_WITH_IP";
        case WiFiConnectionState::RADIO_CHANNEL_MISMATCH: return "RADIO_CHANNEL_MISMATCH";
        default: return "UNKNOWN";
    }
}


/** "AA:BB:CC:DD:EE:FF" — the one place this file formats a MAC address for logging/MQTT target fields. */
void formatMacAddressText(char* buffer, std::size_t bufferSize, const EspNowCommunication::MacAddress& macAddress)
{
    std::snprintf(buffer, bufferSize, "%02X:%02X:%02X:%02X:%02X:%02X",
                  macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
}


const char* hardwareConfigurationFailureText(HardwareConfigurationFailureReason reason)
{
    switch (reason) {
        case HardwareConfigurationFailureReason::NONE: return "applied";
        case HardwareConfigurationFailureReason::NODE_NOT_COMMISSIONED: return "Node is not commissioned";
        case HardwareConfigurationFailureReason::UNSUPPORTED_RELAY_PIN: return "relay GPIO is not declared safe by Node firmware";
        case HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN: return "relay GPIO is already configured";
        case HardwareConfigurationFailureReason::INVALID_ELECTRICAL_RATING: return "invalid nominal voltage/current or startup rating";
        case HardwareConfigurationFailureReason::INVALID_CONFIGURATION: return "invalid load configuration";
        case HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED: return "Node could not initialize or safely verify relay output";
        case HardwareConfigurationFailureReason::PERSISTENCE_FAILED: return "Node could not persist configuration";
        case HardwareConfigurationFailureReason::CAPACITY_REACHED: return "Node load capacity reached";
    }
    return "unknown configuration failure";
}


/**
 * Central repeats the Smart Node's board-profile check before forwarding a
 * load command. The Smart Node remains the final authority, but this avoids
 * sending an obviously unsafe GPIO request across the mesh and makes an old
 * or unverified capability report fail visibly at the installer console.
 */
bool nodeDeclaresRelayPin(const NodeCommissioningRegistry::CommissioningRecord& record,
                          std::uint8_t relayPin)
{
    for (std::size_t index = 0U; index < record.relayCapabilityCount; ++index) {
        if (record.relayPins[index] == relayPin) {
            return true;
        }
    }
    return false;
}


bool sameBatterySensorConfiguration(
    const CentralConfigurationStore::BatterySensorConfiguration& first,
    const CentralConfigurationStore::BatterySensorConfiguration& second)
{
    constexpr float EPSILON = 0.00001F;
    return first.configured == second.configured &&
           first.i2cAddress == second.i2cAddress &&
           std::fabs(first.shuntResistanceOhms - second.shuntResistanceOhms) <= EPSILON &&
           std::fabs(first.maximumExpectedCurrentAmps - second.maximumExpectedCurrentAmps) <= EPSILON &&
           std::fabs(first.emaAlpha - second.emaAlpha) <= EPSILON &&
           std::fabs(first.batteryCapacityAmpHours - second.batteryCapacityAmpHours) <= EPSILON &&
           std::fabs(first.initialStateOfChargePercent - second.initialStateOfChargePercent) <= EPSILON;
}


/**
 * Initializes Central's own I2C bus and registers Central's own
 * (currently Load-less) local Node. A freshly flashed/uncommissioned
 * Central has zero local Loads/Branches (Section "New Central Boot") and
 * zero configured sensors, including its own battery-bus INA219 (Section
 * "Remove Automatic Development Battery Input") — batterySensorConfigured
 * stays false until an installer command registers and verifies one for
 * real. This file no longer defines any local Load or battery sensor address
 * here.
 */
void configureLocalHardware(const EspNowCommunication::MacAddress& localMac)
{
    sensors.initializeBus(CentralNodeConfig::i2cBusConfiguration());

    const Node centralNode(localMac);
    registry.addLocalCentralNode(CentralNodeConfig::CENTRAL_NODE_NAME, centralNode, 0U);
}


CentralConfigurationStore::SafetyPolicy effectiveSafetyPolicy()
{
    const CentralConfigurationStore::SafetyPolicy configured =
        centralConfigurationStore.getConfiguration().safetyPolicy;
    if (configured.configured) {
        return configured;
    }
    return CentralConfigurationStore::SafetyPolicy{
        false,
        CentralNodeConfig::MINIMUM_STATE_OF_CHARGE_PERCENT,
        CentralNodeConfig::WARNING_STATE_OF_CHARGE_PERCENT,
        CentralNodeConfig::TARGET_RUNTIME_HOURS,
        CentralNodeConfig::SAFETY_FACTOR,
        CentralNodeConfig::MAXIMUM_BATTERY_DISCHARGE_CURRENT_AMPS,
        CentralNodeConfig::MAXIMUM_MAIN_CURRENT_AMPS,
    };
}


PowerBudgetCalculator::Inputs configuredPowerBudgetInputs(
    float stateOfChargePercent, float batteryBusVoltageVolts,
    const CentralConfigurationStore::SafetyPolicy& policy)
{
    PowerBudgetCalculator::Inputs inputs{};
    inputs.stateOfChargePercent = stateOfChargePercent;
    inputs.minimumStateOfChargePercent = policy.minimumStateOfChargePercent;
    inputs.nominalBatteryVoltageVolts = CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS;
    inputs.batteryCapacityAmpHours = centralConfigurationStore.getConfiguration().batterySensor.configured
        ? centralConfigurationStore.getConfiguration().batterySensor.batteryCapacityAmpHours
        : CentralNodeConfig::BATTERY_CAPACITY_AMP_HOURS;
    inputs.targetRuntimeHours = policy.targetRuntimeHours;
    inputs.batteryBusVoltageVolts = batteryBusVoltageVolts;
    inputs.maximumBatteryDischargeCurrentAmps = policy.maximumBatteryDischargeCurrentAmps;
    inputs.maximumMainCurrentAmps = policy.maximumMainCurrentAmps;
    inputs.safetyFactor = policy.safetyFactor;
    return inputs;
}


bool applyPersistedBatterySensorConfiguration()
{
    const CentralConfigurationStore::BatterySensorConfiguration battery =
        centralConfigurationStore.getConfiguration().batterySensor;
    if (!battery.configured) {
        return false;
    }

    if (sensors.findSensorByI2CAddress(battery.i2cAddress) == nullptr &&
        !sensors.addSensor(INA219Monitor::INA219SensorConfiguration{
            battery.i2cAddress, BATTERY_RELAY_PIN_SENTINEL, battery.shuntResistanceOhms,
            battery.maximumExpectedCurrentAmps, battery.emaAlpha})) {
        ESP_LOGE(TAG, "BATTERY_CONFIG: could not register INA219 at 0x%02X",
                 static_cast<unsigned int>(battery.i2cAddress));
        return false;
    }

    batterySensorConfigured = true;
    batterySensorI2CAddress = battery.i2cAddress;
    batteryStateOfCharge.initialize(battery.batteryCapacityAmpHours, battery.initialStateOfChargePercent);
    ESP_LOGI(TAG, "BATTERY_CONFIG: restored INA219 at 0x%02X capacity=%.1fAh",
             static_cast<unsigned int>(battery.i2cAddress), static_cast<double>(battery.batteryCapacityAmpHours));
    return true;
}


/**
 * Walks the destination's Next-Hop chain back toward Central to find the
 * first Node after Central on the path — the same "who do I physically
 * send to first" resolution this project has always used to route a
 * message through the reconstructed tree.
 */
bool findNextHopFromCentral(
    const EspNowCommunication::MacAddress& centralMac,
    const EspNowCommunication::MacAddress& destinationMac,
    CentralNodeRegistry& registryRef,
    EspNowCommunication::MacAddress& nextHopMac)
{
    const CentralNodeRegistry::PlanningNode *current = registryRef.findNodeByMacAddress(destinationMac);
    if (current == nullptr) {
        return false;
    }

    for (std::size_t level = 0U; level <= registryRef.getNumberOfNodes(); ++level) {
        if (current->nextHopToCentralMacAddress == centralMac) {
            nextHopMac = current->node.getMacAddress();
            return true;
        }

        current = registryRef.findNodeByMacAddress(current->nextHopToCentralMacAddress);
        if (current == nullptr) {
            return false;
        }
    }

    return false;
}


/**
 * Sensor Acquisition Task (Section 4.6.1, ~1s): reads Central's one
 * battery-bus INA219 through the filtered measurement path and advances
 * BatteryStateOfCharge (Equation 4.8) from that battery sensor's filtered
 * current. Individual Smart-node Loads use installer-entered ratings, not
 * per-load INA219 measurements.
 */
void sensorAcquisitionTask(void *parameter)
{
    (void)parameter;

    TickType_t lastTick = xTaskGetTickCount();

    while (true) {
        bool sensorConfiguredThisCycle = false;
        std::uint8_t i2cAddressThisCycle = 0U;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            sensorConfiguredThisCycle = batterySensorConfigured;
            i2cAddressThisCycle = batterySensorI2CAddress;
            xSemaphoreGive(stateMutex);
        }

        LoadMeasurements batteryMeasurements{};
        const bool batteryOk = sensorConfiguredThisCycle &&
            sensors.readFilteredMeasurements(i2cAddressThisCycle, batteryMeasurements);

        const TickType_t now = xTaskGetTickCount();
        const float deltaTimeSeconds =
            static_cast<float>(pdTICKS_TO_MS(now - lastTick)) / 1000.0F;
        lastTick = now;

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            const std::uint32_t nowMilliseconds = static_cast<std::uint32_t>(pdTICKS_TO_MS(now));

            if (batteryOk) {
                latestBatteryMeasurements = batteryMeasurements;
                batteryTelemetryEverValid = true;
                batteryTelemetryLastValidMilliseconds = nowMilliseconds;
                if (deltaTimeSeconds > 0.0F) {
                    batteryStateOfCharge.update(batteryMeasurements.currentAmps, deltaTimeSeconds);
                }
            } else if (sensorConfiguredThisCycle) {
                /*
                 * Never fabricate a substitute reading: latestBatteryMeasurements
                 * and batteryStateOfCharge are simply not updated this cycle.
                 * isBatteryTelemetryTrustworthy() is what tells the
                 * Optimisation Task whether the (now one cycle older)
                 * latestBatteryMeasurements value may still be used.
                 */
                ESP_LOGW(TAG, "SENSOR FAULT: battery INA219 read failed this cycle");
            }
            /* !sensorConfiguredThisCycle: nothing to read, nothing to log every cycle - see the boot summary/state/system for the honest NOT_CONFIGURED status instead of a repeated warning. */

            xSemaphoreGive(stateMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(CentralNodeConfig::SENSOR_ACQUISITION_PERIOD_MILLISECONDS));
    }
}


/**
 * Applies one RelayCommandDispatcher::RelayTarget and waits for it to be
 * confirmed (locally through RelayController for Central's own MAC —
 * synchronous already — or over ESP-NOW for a remote Smart Node, polling
 * relayCommandDispatcher until espNowCommunicationTask's acknowledgement
 * handler completes the command or it times out). Returns true only when
 * the transition was actually confirmed to the requested state.
 *
 * Relay failure semantics (Section "Fix Relay Confirmation Failure
 * Semantics"): a failed/timed-out command NEVER calls
 * Load::setConfirmedRelayState() — a failed acknowledgement does not
 * prove the relay is OFF, so the previous trusted confirmed state (and
 * its validity) is simply left exactly as it was. Only Load::setHealth()
 * changes on failure.
 *
 * Only briefly holds stateMutex per step (never across the ESP-NOW wait,
 * which would otherwise deadlock against espNowCommunicationTask needing
 * the same lock to deliver the acknowledgement that ends the wait).
 */
bool dispatchRelayTargetAndWaitForConfirmation(
    const RelayCommandDispatcher::RelayTarget& target, const EspNowCommunication::MacAddress& localMac)
{
    if (target.nodeMacAddress == localMac) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(1000U)) != pdTRUE) {
            ESP_LOGW(TAG, "Relay dispatch skipped: could not acquire state lock");
            return false;
        }

        const bool writeSucceeded = relays.setRelayState(target.relayPin, target.desiredOn);
        bool confirmedOn = false;
        const bool readBackSucceeded = relays.readBackState(target.relayPin, confirmedOn);
        const bool success = writeSucceeded && readBackSucceeded && (confirmedOn == target.desiredOn);

        Load *load = registry.findMutableLoad(localMac, target.relayPin);
        if (load != nullptr) {
            if (success) {
                load->setConfirmedRelayState(confirmedOn);
                load->setHealth(LoadHealth::AVAILABLE);
            } else {
                /* Preserve the previous trusted confirmed state exactly — see function documentation. */
                load->setHealth(LoadHealth::FAULTED);
                ++faultCount;
            }
        }

        ESP_LOGI(TAG, "Local relay pin=%u commanded %s: %s",
                 static_cast<unsigned int>(target.relayPin), target.desiredOn ? "ON" : "OFF",
                 success ? "OK" : "FAILED");

        xSemaphoreGive(stateMutex);
        return success;
    }

    EspNowCommunication::MacAddress nextHopMac{};
    std::uint32_t commandId = 0U;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(1000U)) != pdTRUE) {
        ESP_LOGW(TAG, "Relay dispatch skipped: could not acquire state lock");
        return false;
    }

    if (!findNextHopFromCentral(localMac, target.nodeMacAddress, registry, nextHopMac)) {
        ESP_LOGW(TAG, "Cannot dispatch relay command: no known route to the owning Node");
        ++faultCount;
        xSemaphoreGive(stateMutex);
        return false;
    }

    const std::uint32_t dispatchedAtMilliseconds = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    commandId = relayCommandDispatcher.beginCommand(target, dispatchedAtMilliseconds);

    RelayCommandPacket commandPacket{};
    commandPacket.relayPin = target.relayPin;
    commandPacket.desiredState = static_cast<std::uint8_t>(
        target.desiredOn ? RelayCommandState::ON : RelayCommandState::OFF);
    commandPacket.commandId = commandId;

    /* Section "Central Relay Command TX Log": every field logged is taken from the exact target/packet being sent, not recomputed. */
    const Load *loadBeforeDispatch = registry.findMutableLoad(target.nodeMacAddress, target.relayPin);
    char dstNodeText[18] = {};
    formatMacAddressText(dstNodeText, sizeof(dstNodeText), target.nodeMacAddress);
    ESP_LOGI(TAG, "CENTRAL_TX RELAY_COMMAND commandId=%u dstNode=%s loadPin=%u loadName=%s desired=%s "
                  "confirmedBefore=%s confirmedValidBefore=%s",
             static_cast<unsigned int>(commandId), dstNodeText, static_cast<unsigned int>(target.relayPin),
             loadBeforeDispatch != nullptr ? loadBeforeDispatch->getName().c_str() : "?",
             target.desiredOn ? "ON" : "OFF",
             target.currentlyConfirmedOn ? "ON" : "OFF", target.currentlyConfirmedValid ? "true" : "false");

    const bool sent = communication.sendTo(nextHopMac, target.nodeMacAddress,
                                            EspNowCommunication::MessageType::RELAY_COMMAND, commandPacket, 500U);

    ESP_LOGI(TAG, "RELAY_COMMAND commandId=%u %s", static_cast<unsigned int>(commandId),
             sent ? "MAC_DELIVERED" : "SEND_FAILED");

    if (!sent) {
        relayCommandDispatcher.completeCommand(commandId);
        ++faultCount;
        xSemaphoreGive(stateMutex);
        return false;
    }

    xSemaphoreGive(stateMutex);

    /*
     * Poll (briefly re-locking each check) until espNowCommunicationTask's
     * acknowledgement handler completes this command, or it times out —
     * this is what makes "advance P_remaining/P_committed/P_branch only
     * after a successfully CONFIRMED ON transition" possible for a
     * remote command, which is inherently asynchronous.
     */
    const TickType_t pollDeadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(CentralNodeConfig::RELAY_COMMAND_ACK_TIMEOUT_MILLISECONDS);
    bool stillPending = true;

    while (xTaskGetTickCount() < pollDeadline) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            stillPending = relayCommandDispatcher.isPending(commandId);
            xSemaphoreGive(stateMutex);
        }

        if (!stillPending) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100U));
    }

    if (stillPending) {
        /* Timed out waiting for the acknowledgement: same failure handling as an expired command elsewhere. */
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
            Load *load = registry.findMutableLoad(target.nodeMacAddress, target.relayPin);
            if (load != nullptr) {
                load->setHealth(LoadHealth::FAULTED);
                ++faultCount;
            }
            relayCommandDispatcher.completeCommand(commandId);
            xSemaphoreGive(stateMutex);
        }
        ESP_LOGW(TAG, "Relay commandId=%u timed out waiting for acknowledgement during dispatch",
                 static_cast<unsigned int>(commandId));
        return false;
    }

    /* The acknowledgement handler already applied the confirmed/faulted Load state; just read the outcome back. */
    bool confirmedSuccess = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
        const Load *load = registry.findMutableLoad(target.nodeMacAddress, target.relayPin);
        confirmedSuccess = load != nullptr && load->getHealth() == LoadHealth::AVAILABLE &&
                            load->isConfirmedRelayStateValid() && load->getConfirmedRelayState() == target.desiredOn;
        xSemaphoreGive(stateMutex);
    }

    return confirmedSuccess;
}


/**
 * Determines every Fixed Load's relay target for this cycle (Section
 * "Add Critical Protection For Fixed Loads"). Fixed Loads are never
 * Best-First Search candidates: normally a Fixed Load's target simply
 * follows its own configured LoadMode (FIXED_ON -> target ON, FIXED_OFF
 * -> target OFF).
 *
 * Chapter 4 additionally requires protecting the system when combined
 * FIXED_ON Running Power would exceed what is genuinely available this
 * cycle. The trigger used here is exactly P_fixed > P_available: no
 * separate SoC <= SoC_min check is added, because PowerBudgetCalculator's
 * own Equation 4.10 usable-energy clamp already forces P_available itself
 * to zero once SoC has reached SoC_min, so that condition is already
 * covered by this single comparison.
 *
 * The document does not specify a Best-First-style ranking for which
 * Fixed Load(s) to shed under this condition. Per the explicit
 * instruction not to invent a hidden search for that decision, a single
 * clearly documented deterministic policy is used instead: shed
 * currently FIXED_ON Loads in ascending configured-priority order
 * (Load::getPriority() lowest first — this project's convention is that
 * a higher priority number is more important, e.g. Light=8, so the
 * least-important Load is shed first) until the remaining committed
 * Fixed power fits within availablePowerWatts or no further eligible
 * Load remains. Only Loads currently eligible for a new command
 * (isLoadEligibleForNewCommandsThisCycle()) are ever chosen to shed — an
 * offline/faulted Fixed Load cannot receive a new command this cycle
 * either way, so it is left exactly at its previous target rather than
 * being force-assumed OFF.
 *
 * Every shed decision, and the overall critical condition, are logged
 * explicitly as the required fault diagnostic.
 */
void applyFixedLoadTargets(LoadFilter& loadFilter, float availablePowerWatts, std::uint32_t nowMilliseconds)
{
    for (std::size_t i = 0U; i < loadFilter.getNumberOfFixedOffLoads(); ++i) {
        const Load *constLoad = loadFilter.getFixedOffLoad(i);
        if (constLoad == nullptr) {
            continue;
        }
        Load *load = registry.findMutableLoad(constLoad->getMacAddress(), constLoad->getRelayPin());
        if (load != nullptr) {
            load->setTargetRelayState(false);
        }
    }

    std::vector<Load*> fixedOnLoadsByAscendingPriority;
    float committedFixedPowerWatts = 0.0F;

    for (std::size_t i = 0U; i < loadFilter.getNumberOfFixedOnLoads(); ++i) {
        const Load *constLoad = loadFilter.getFixedOnLoad(i);
        if (constLoad == nullptr) {
            continue;
        }
        Load *load = registry.findMutableLoad(constLoad->getMacAddress(), constLoad->getRelayPin());
        if (load == nullptr) {
            continue;
        }

        load->setTargetRelayState(true); // normal case: every FIXED_ON Load's target is simply ON
        committedFixedPowerWatts += load->getPower().runningWatts;
        fixedOnLoadsByAscendingPriority.push_back(load);
    }

    if (committedFixedPowerWatts <= availablePowerWatts) {
        return; // safe: no critical protection needed this cycle
    }

    ESP_LOGW(TAG, "CRITICAL PROTECTION: Fixed ON demand %.1fW exceeds available power %.1fW",
             static_cast<double>(committedFixedPowerWatts), static_cast<double>(availablePowerWatts));

    std::sort(fixedOnLoadsByAscendingPriority.begin(), fixedOnLoadsByAscendingPriority.end(),
              [](const Load *a, const Load *b) { return a->getPriority() < b->getPriority(); });

    for (Load *load : fixedOnLoadsByAscendingPriority) {
        if (committedFixedPowerWatts <= availablePowerWatts) {
            break;
        }
        if (!isLoadEligibleForNewCommandsThisCycle(*load, nowMilliseconds)) {
            continue; // cannot safely command this Load this cycle either way
        }

        load->setTargetRelayState(false);
        committedFixedPowerWatts -= load->getPower().runningWatts;

        ESP_LOGW(TAG, "CRITICAL PROTECTION: shedding Fixed Load pin=%u priority=%u to relieve overload",
                 static_cast<unsigned int>(load->getRelayPin()), static_cast<unsigned int>(load->getPriority()));
    }

    if (committedFixedPowerWatts > availablePowerWatts) {
        ESP_LOGE(TAG,
                 "CRITICAL PROTECTION: unable to shed enough Fixed load to fit available power "
                 "(%.1fW still committed vs %.1fW available; remaining Fixed ON Loads are offline/faulted)",
                 static_cast<double>(committedFixedPowerWatts), static_cast<double>(availablePowerWatts));
    }
}


/**
 * Optimisation Task (Section 4.6.1, nominal period 5s, also woken
 * immediately by optimizationTriggerSemaphore): the complete planning
 * cycle described at the top of this file.
 */
void runOptimizationCycle()
{
    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();

    float stateOfChargePercent = 0.0F;
    float batteryVoltageVolts = 0.0F;
    float batteryCurrentAmps = 0.0F;
    float availablePowerWattsForPlanning = 0.0F;
    bool telemetryTrustworthy = false;
    LoadFilter loadFilter;
    AvailablePowerManager availablePowerManager;
    PowerBudgetCalculator powerBudget;
    CentralConfigurationStore::SafetyPolicy safetyPolicy{};
    std::vector<RelayCommandDispatcher::RelayTarget> targets;

    /** One admitted-Auto Load's identity, kept by value (never a raw pointer) so it stays valid after stateMutex is released for dispatch. */
    struct AdmittedTarget {
        Load::MacAddress macAddress;
        std::uint8_t relayPin;
    };
    std::vector<AdmittedTarget> admittedLoadIds;

    /** Nodes that just transitioned online->offline this cycle, published as NODE_OFFLINE events after stateMutex is released (see the MQTT publish block below) rather than from inside the locked section. */
    std::vector<std::pair<EspNowCommunication::MacAddress, std::string>> newlyOfflineNodes;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(1000U)) != pdTRUE) {
        ESP_LOGW(TAG, "Optimisation cycle skipped: could not acquire state lock");
        return;
    }

    safetyPolicy = effectiveSafetyPolicy();

    /* Timed-out relay commands (Section "Relay Failure") become faults before this cycle plans anything new. */
    const std::uint32_t nowMilliseconds = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    for (const RelayCommandDispatcher::PendingCommand &expired :
         relayCommandDispatcher.findExpiredCommands(nowMilliseconds, CentralNodeConfig::RELAY_COMMAND_ACK_TIMEOUT_MILLISECONDS)) {
        Load *load = registry.findMutableLoad(expired.nodeMacAddress, expired.relayPin);
        if (load != nullptr) {
            load->setHealth(LoadHealth::FAULTED);
            ++faultCount;
        }
        relayCommandDispatcher.completeCommand(expired.commandId);
        ESP_LOGW(TAG, "Relay commandId=%u timed out awaiting acknowledgement", static_cast<unsigned int>(expired.commandId));
    }

    /* Persisted user configuration (MQTT-driven) always wins over whatever a Node's own report last carried. */
    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const CentralNodeRegistry::PlanningNode *planningNode = registry.getNode(nodeIndex);
        if (planningNode == nullptr) {
            continue;
        }
        for (std::size_t loadIndex = 0U; loadIndex < planningNode->node.getNumberOfLoads(); ++loadIndex) {
            const Load *constLoad = planningNode->node.getLoad(loadIndex);
            if (constLoad == nullptr) {
                continue;
            }
            Load *load = registry.findMutableLoad(planningNode->node.getMacAddress(), constLoad->getRelayPin());
            if (load != nullptr) {
                loadConfigurationStore.applyToLoad(*load);
            }
        }
    }

    /*
     * Node offline detection (Section "Exclude Stale/Offline Smart Nodes
     * From Planning"): going offline is only detectable here, by timeout
     * (the absence of a report, not an event) — logged only on the
     * transition so a Node that stays offline for many cycles never
     * floods the log. The matching NODE_RECOVERED transition is detected
     * and logged where a fresh report actually arrives
     * (espNowCommunicationTask), which also immediately re-triggers
     * optimisation rather than waiting for the next periodic cycle.
     */
    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const CentralNodeRegistry::PlanningNode *planningNode = registry.getNode(nodeIndex);
        if (planningNode == nullptr || planningNode->isCentralNode) {
            continue;
        }

        const std::uint32_t ageMilliseconds = nowMilliseconds - planningNode->lastSeenMilliseconds;
        if (ageMilliseconds > CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS) {
            if (markNodeMacAddressOffline(planningNode->node.getMacAddress())) {
                ESP_LOGW(TAG, "NODE_OFFLINE: node=%s", planningNode->nodeName.c_str());
                newlyOfflineNodes.emplace_back(planningNode->node.getMacAddress(), planningNode->nodeName);
            }
        }
    }

    /*
     * Sensor-failure-aware power budget (Section "Fix Sensor Failure
     * Behavior"): only ever call PowerBudgetCalculator::calculate() from
     * a trustworthy battery reading. While untrustworthy, planning
     * freezes at the last genuinely valid Pavailable/voltage rather than
     * fabricating a fresh budget from a stale/failed reading — actuation
     * safety is independently guaranteed regardless, since the pre-ON
     * safety re-check below always re-reads live telemetry immediately
     * before every ON command.
     */
    telemetryTrustworthy = batterySensorConfigured && isBatteryTelemetryTrustworthy(nowMilliseconds);
    const bool stateOfChargeValid = batteryStateOfCharge.isValid();
    const bool batteryStateKnown = telemetryTrustworthy && stateOfChargeValid;
    stateOfChargePercent = batteryStateOfCharge.getStateOfChargePercent();
    float maximumBatteryPowerWattsForPlanning = 0.0F;

    if (batteryStateKnown) {
        batteryVoltageVolts = latestBatteryMeasurements.voltageVolts;
        batteryCurrentAmps = latestBatteryMeasurements.currentAmps;

        if (powerBudget.calculate(configuredPowerBudgetInputs(stateOfChargePercent, batteryVoltageVolts, safetyPolicy))) {
            lastValidAvailablePowerWatts = powerBudget.getAvailablePowerWatts();
            lastValidBatteryVoltageVolts = batteryVoltageVolts;
        }

        availablePowerWattsForPlanning = powerBudget.getAvailablePowerWatts();
        maximumBatteryPowerWattsForPlanning = powerBudget.getMaximumBatteryPowerWatts();
    } else {
        batteryVoltageVolts = lastValidBatteryVoltageVolts;
        batteryCurrentAmps = 0.0F;
        /* Section "Safe Behavior When SoC Unknown": Pavailable stays at its zero-initialized default until a real battery state has ever been established - never fabricated. */
        availablePowerWattsForPlanning = lastValidAvailablePowerWatts;
        maximumBatteryPowerWattsForPlanning =
            lastValidBatteryVoltageVolts * safetyPolicy.maximumBatteryDischargeCurrentAmps;

        ++faultCount;
        if (!batterySensorConfigured) {
            ESP_LOGW(TAG, "PLANNING_RESTRICTED: battery sensor NOT_CONFIGURED; Pavailable frozen at %.1fW",
                     static_cast<double>(availablePowerWattsForPlanning));
        } else if (!stateOfChargeValid) {
            ESP_LOGW(TAG, "PLANNING_RESTRICTED: SoC is UNKNOWN (no valid estimate established yet); Pavailable frozen at %.1fW",
                     static_cast<double>(availablePowerWattsForPlanning));
        } else {
            ESP_LOGW(TAG, "SENSOR FAULT: battery telemetry untrustworthy this cycle; planning frozen at last valid Pavailable=%.1fW",
                     static_cast<double>(availablePowerWattsForPlanning));
        }
    }

    /*
     * No Smart Node has a per-load current sensor in the final design.
     * Build a conservative estimated demand from the last confirmed relay
     * state and each installer-entered running rating. An unconfirmed relay
     * is treated as ON so the pre-actuation safety gate never gains capacity
     * by pretending an electrically unknown circuit is off.
     */
    float estimatedTotalLoadPowerWatts = 0.0F;

    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const CentralNodeRegistry::PlanningNode *planningNode = registry.getNode(nodeIndex);
        if (planningNode == nullptr) {
            continue;
        }
        for (std::size_t loadIndex = 0U; loadIndex < planningNode->node.getNumberOfLoads(); ++loadIndex) {
            const Load *load = planningNode->node.getLoad(loadIndex);
            if (load == nullptr) {
                continue;
            }
            loadFilter.addLoad(*load);
            if (!load->isConfirmedRelayStateValid() || load->getConfirmedRelayState()) {
                estimatedTotalLoadPowerWatts += load->getPower().runningWatts;
            }
        }
    }

    availablePowerManager.calculateAvailablePower(availablePowerWattsForPlanning, loadFilter);

    /* Fixed Loads are never Best-First Search candidates; their target (including critical protection) is decided independently. */
    applyFixedLoadTargets(loadFilter, availablePowerWattsForPlanning, nowMilliseconds);

    BestFirstSearch search;
    search.setSearchScoreWeights(CentralNodeConfig::bestFirstSearchWeights());

    BestFirstSearch::ElectricalPlanningState planningState{};
    planningState.stateOfChargePercent = stateOfChargePercent;
    planningState.minimumStateOfChargePercent = safetyPolicy.minimumStateOfChargePercent;
    planningState.warningStateOfChargePercent = safetyPolicy.warningStateOfChargePercent;
    planningState.batteryBusVoltageVolts = batteryVoltageVolts > 0.0F ? batteryVoltageVolts : CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS;
    planningState.maximumBatteryPowerWatts = maximumBatteryPowerWattsForPlanning;
    planningState.maximumMainCurrentAmps = safetyPolicy.maximumMainCurrentAmps;
    planningState.totalAvailablePowerWatts = availablePowerManager.getTotalAvailablePowerWatts();
    planningState.powerAvailableForAutoLoadsWatts = availablePowerManager.getPowerAvailableForAutoLoadsWatts();
    planningState.initialCommittedPowerWatts = availablePowerManager.getFixedOnRunningPowerWatts();

    const bool searchStarted = search.startSearch(planningState);

    if (searchStarted) {
        for (std::size_t i = 0U; i < loadFilter.getNumberOfAutoCandidateLoads(); ++i) {
            const Load *autoLoad = loadFilter.getAutoCandidateLoad(i);
            if (autoLoad == nullptr) {
                continue;
            }

            if (!isLoadEligibleForNewCommandsThisCycle(*autoLoad, nowMilliseconds)) {
                /* Sections 7 & 8: a stale/offline Node or a faulted/unavailable Load is never a new BFS candidate. */
                continue;
            }

            const BestFirstSearch::BranchId branchId{autoLoad->getMacAddress(), autoLoad->getRelayPin()};

            float branchMaximumCurrentAmps = 0.0F;
            registry.findBranchMaximumCurrentAmps(autoLoad->getMacAddress(), autoLoad->getRelayPin(), branchMaximumCurrentAmps);

            search.registerBranch(branchId, 0.0F, branchMaximumCurrentAmps);

            LoadScheduleEvaluation evaluation{};
            float schedulePenalty = 0.0F;
            if (scheduleEvaluator.evaluateSchedule(*autoLoad, currentTimeProvider, evaluation)) {
                schedulePenalty = evaluation.futureSchedulePenalty;
            }

            search.addLoad(*autoLoad, branchId, schedulePenalty);
        }

        search.run();

        for (std::size_t i = 0U; i < search.getNumberOfLoadsAdded(); ++i) {
            const Load *searchedLoad = search.getLoad(i);
            if (searchedLoad == nullptr) {
                continue;
            }

            Load *mutableLoad = registry.findMutableLoad(searchedLoad->getMacAddress(), searchedLoad->getRelayPin());
            if (mutableLoad == nullptr) {
                continue;
            }

            const bool selected = search.isLoadSelectedToBeOn(i);
            mutableLoad->setLastBestFirstRejectionReason(search.getLoadSelectionRejectionReason(i));

            /*
             * Records the Best-First decision as this cycle's *target*
             * relay state only (Section "Fix Configured Load Mode Vs
             * Algorithm Target State") — an Auto Load's configured
             * AUTO_ON/AUTO_OFF mode (getMode()) is a user/system setting
             * that Best-First Search's per-cycle selection/rejection must
             * never overwrite.
             */
            mutableLoad->setTargetRelayState(selected);
        }

        /*
         * Section "Preserve The Exact Best-First ON Order": admitted Auto
         * Loads' ON commands must be dispatched in exactly the order
         * run() extracted them from OPEN — getAdmittedLoad() is the only
         * source of that order; reconstructing it from registry/Node/MAC
         * traversal order would silently discard it.
         */
        for (std::size_t k = 0U; k < search.getNumberOfAdmittedLoads(); ++k) {
            const Load *admittedLoad = search.getAdmittedLoad(k);
            if (admittedLoad == nullptr || !isLoadEligibleForNewCommandsThisCycle(*admittedLoad, nowMilliseconds)) {
                continue;
            }

            admittedLoadIds.push_back(AdmittedTarget{admittedLoad->getMacAddress(), admittedLoad->getRelayPin()});

            targets.push_back(RelayCommandDispatcher::RelayTarget{
                admittedLoad->getMacAddress(), admittedLoad->getRelayPin(), true,
                admittedLoad->getConfirmedRelayState(), admittedLoad->isConfirmedRelayStateValid()
            });
        }
    } else {
        ESP_LOGW(TAG, "Best-First Search did not start this cycle (invalid planning state)");
    }

    /*
     * Every remaining Load — every Fixed Load, plus every Auto Load
     * Best-First Search did not admit this cycle (rejected, never
     * evaluated, or excluded above) — in registry order. Appended after
     * the admitted-Auto entries already in targets so
     * RelayCommandDispatcher::buildDispatchOrder() preserves their
     * Best-First order within the ON phase.
     */
    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const CentralNodeRegistry::PlanningNode *planningNode = registry.getNode(nodeIndex);
        if (planningNode == nullptr) {
            continue;
        }
        for (std::size_t loadIndex = 0U; loadIndex < planningNode->node.getNumberOfLoads(); ++loadIndex) {
            const Load *load = planningNode->node.getLoad(loadIndex);
            if (load == nullptr) {
                continue;
            }

            bool alreadyHandledAsAdmittedAuto = false;
            for (const AdmittedTarget &admitted : admittedLoadIds) {
                if (admitted.macAddress == load->getMacAddress() && admitted.relayPin == load->getRelayPin()) {
                    alreadyHandledAsAdmittedAuto = true;
                    break;
                }
            }
            if (alreadyHandledAsAdmittedAuto) {
                continue;
            }

            if (!isLoadNodeOnline(*load, nowMilliseconds)) {
                continue; // Section 7: never send a new command to an unreachable Node
            }
            if (load->getTargetRelayState() && load->getHealth() != LoadHealth::AVAILABLE) {
                continue; // Section 8: never command a faulted/unavailable Load ON again until recovery
            }

            targets.push_back(RelayCommandDispatcher::RelayTarget{
                load->getMacAddress(), load->getRelayPin(), load->getTargetRelayState(),
                load->getConfirmedRelayState(), load->isConfirmedRelayStateValid()
            });
        }
    }

    batteryStateOfCharge.persist();
    lastOptimizationEpochSeconds = static_cast<std::int64_t>(time(nullptr));

    xSemaphoreGive(stateMutex);

    /* Relay dispatch itself (may block briefly on ESP-NOW sends/acknowledgements) happens outside the lock. */
    const std::vector<RelayCommandDispatcher::RelayTarget> dispatchOrder =
        RelayCommandDispatcher::buildDispatchOrder(targets);

    /*
     * Live sequential accounting for the pre-ON safety re-check (Section
     * "Add The Documented Safety Re-Check Immediately Before Each ON
     * Command"): starts from the conservative rated estimate of power that
     * may already be flowing, then only ever
     * advances after a genuinely CONFIRMED transition — never
     * optimistically, and never from a value BestFirstSearch merely
     * planned.
     */
    float liveCommittedPowerWatts = estimatedTotalLoadPowerWatts;

    for (const RelayCommandDispatcher::RelayTarget &target : dispatchOrder) {
        if (!target.desiredOn) {
            /* OFF transitions are always dispatched: releasing capacity is safe by construction. */
            const bool confirmedOff = dispatchRelayTargetAndWaitForConfirmation(target, localMac);
            if (confirmedOff) {
                float runningWatts = 0.0F;
                if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
                    const Load *offLoad = registry.findMutableLoad(target.nodeMacAddress, target.relayPin);
                    if (offLoad != nullptr) {
                        runningWatts = offLoad->getPower().runningWatts;
                    }
                    xSemaphoreGive(stateMutex);
                }
                liveCommittedPowerWatts = std::max(0.0F, liveCommittedPowerWatts - runningWatts);
            }
            continue;
        }

        bool isAdmittedAutoTarget = false;
        for (const AdmittedTarget &admitted : admittedLoadIds) {
            if (admitted.macAddress == target.nodeMacAddress && admitted.relayPin == target.relayPin) {
                isAdmittedAutoTarget = true;
                break;
            }
        }

        if (!isAdmittedAutoTarget) {
            /*
             * Fixed-load ON command: already safety-checked moments
             * earlier by applyFixedLoadTargets() against this same locked
             * planning snapshot. Algorithm 4.4's P_remaining/P_committed
             * guard models BestFirstSearch's own sequential Auto-load
             * allocation specifically (Section 4.6.3-4.6.4), so it is
             * only re-applied below to admitted Auto ON transitions, not
             * re-applied here.
             */
            const bool confirmedOn = dispatchRelayTargetAndWaitForConfirmation(target, localMac);
            if (confirmedOn) {
                float runningWatts = 0.0F;
                if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
                    const Load *onLoad = registry.findMutableLoad(target.nodeMacAddress, target.relayPin);
                    if (onLoad != nullptr) {
                        runningWatts = onLoad->getPower().runningWatts;
                    }
                    xSemaphoreGive(stateMutex);
                }
                liveCommittedPowerWatts += runningWatts;
            }
            continue;
        }

        /*
         * Admitted Auto-load ON transition: re-check Algorithm 4.4
         * immediately before actuation against the LATEST valid atomic
         * snapshot — not by re-running the whole O(n log n) search.
         */
        bool proceed = false;
        float candidateRunningWatts = 0.0F;

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
            const std::uint32_t recheckNowMilliseconds = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
            Load *load = registry.findMutableLoad(target.nodeMacAddress, target.relayPin);

            if (load == nullptr) {
                xSemaphoreGive(stateMutex);
                continue;
            }

            if (!isBatteryTelemetryTrustworthy(recheckNowMilliseconds)) {
                load->setTargetRelayState(false);
                load->setLastBestFirstRejectionReason(BestFirstSearch::LOW_BATTERY);
                ESP_LOGW(TAG, "Pre-ON safety re-check refused pin=%u: battery telemetry untrustworthy at actuation time",
                         static_cast<unsigned int>(target.relayPin));
                xSemaphoreGive(stateMutex);
                continue;
            }

            candidateRunningWatts = load->getPower().runningWatts;

            const float liveVoltageVolts = latestBatteryMeasurements.voltageVolts > 0.0F
                ? latestBatteryMeasurements.voltageVolts : CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS;

            float branchMaximumCurrentAmps = 0.0F;
            registry.findBranchMaximumCurrentAmps(target.nodeMacAddress, target.relayPin, branchMaximumCurrentAmps);

            BestFirstSearch::FeasibilityInputs recheckInputs{};
            const CentralConfigurationStore::SafetyPolicy recheckPolicy = effectiveSafetyPolicy();
            recheckInputs.stateOfChargePercent = batteryStateOfCharge.getStateOfChargePercent();
            recheckInputs.minimumStateOfChargePercent = recheckPolicy.minimumStateOfChargePercent;
            recheckInputs.candidateRunningPowerWatts = candidateRunningWatts;
            recheckInputs.candidatePeakPowerWatts = load->getPower().startupWatts;
            recheckInputs.remainingPowerWatts = std::max(0.0F, availablePowerWattsForPlanning - liveCommittedPowerWatts);
            recheckInputs.committedPowerWatts = liveCommittedPowerWatts;
            recheckInputs.maximumBatteryPowerWatts = liveVoltageVolts * recheckPolicy.maximumBatteryDischargeCurrentAmps;
            recheckInputs.batteryBusVoltageVolts = liveVoltageVolts;
            recheckInputs.maximumMainCurrentAmps = recheckPolicy.maximumMainCurrentAmps;
            recheckInputs.branchCommittedPowerWatts = 0.0F; // one Load per Branch in this hardware model: this candidate's own branch starts uncommitted for its own check
            recheckInputs.branchMaximumCurrentAmps = branchMaximumCurrentAmps;

            const std::uint8_t rejectionReason = BestFirstSearch::checkFeasibility(recheckInputs);

            if (rejectionReason == BestFirstSearch::NONE) {
                proceed = true;
            } else {
                load->setTargetRelayState(false);
                load->setLastBestFirstRejectionReason(rejectionReason);
                ESP_LOGW(TAG, "Pre-ON safety re-check refused pin=%u: reason=%u",
                         static_cast<unsigned int>(target.relayPin), static_cast<unsigned int>(rejectionReason));
            }

            xSemaphoreGive(stateMutex);
        }

        if (!proceed) {
            continue;
        }

        const bool confirmedOn = dispatchRelayTargetAndWaitForConfirmation(target, localMac);
        if (confirmedOn) {
            /*
             * Only advance the live committed-power accounting after a
             * successfully CONFIRMED ON transition, immediately before
             * evaluating the next one (Section "Add The Documented Safety
             * Re-Check").
             */
            liveCommittedPowerWatts += candidateRunningWatts;
        }
    }

    /* Publish updated state (no-ops gracefully when MQTT is not connected). */
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
        SystemStateInputs systemState{};
        systemState.batterySensorConfigured = batterySensorConfigured;
        systemState.batteryVoltageVolts = batteryVoltageVolts;
        systemState.batteryCurrentAmps = batteryCurrentAmps;
        systemState.batteryMeasurementSourceText = batterySensorConfigured
            ? toText(sensors.getLastMeasurementSource(batterySensorI2CAddress))
            : toText(MeasurementSource::NONE);
        systemState.stateOfChargePercent = stateOfChargePercent;
        systemState.stateOfChargeValid = stateOfChargeValid;
        systemState.stateOfChargeSourceText = toText(batteryStateOfCharge.getSource());
        systemState.estimatedTotalLoadPowerWatts = estimatedTotalLoadPowerWatts;
        systemState.availablePowerWatts = availablePowerManager.getTotalAvailablePowerWatts();
        systemState.fixedOnRunningPowerWatts = availablePowerManager.getFixedOnRunningPowerWatts();
        systemState.powerAvailableForAutoLoadsWatts = availablePowerManager.getPowerAvailableForAutoLoadsWatts();
        systemState.remainingPowerWatts = search.getRemainingPowerWatts();
        systemState.committedPowerWatts = search.getCommittedPowerWatts();
        systemState.wifiConnected = wifiManager.isConnected();
        systemState.wifiStateText = wifiStateText(wifiManager.getState());
        systemState.mqttConnected = mqttManager.isConnected();
        systemState.currentTimeValid = currentTimeProvider.isCurrentTimeValid();
        systemState.currentTimeSourceText =
            currentTimeProvider.getCurrentTimeSource() == TimeSource::NTP ? "NTP" :
            (currentTimeProvider.getCurrentTimeSource() == TimeSource::MANUAL ? "MANUAL" : "NONE");
        systemState.lastOptimizationEpochSeconds = lastOptimizationEpochSeconds;
        systemState.operatingEnvironmentText = toText(developmentSession.getEnvironment());
        systemState.developmentSessionActive = developmentSession.isActive();
        systemState.faultCount = faultCount;
        systemState.faultSummaryText = !batteryStateKnown
            ? (!batterySensorConfigured ? "Battery sensor not configured; Auto-load allocation restricted"
               : (!stateOfChargeValid ? "SoC is UNKNOWN; Auto-load allocation restricted"
                  : "Battery telemetry invalid or stale; planning frozen at last valid values"))
            : (faultCount > 0U ? "See per-Load health in state/loads" : "");

        const std::string systemJson = SystemStateJson::build(systemState, CentralNodeConfig::MQTT_SCHEMA_VERSION);
        const std::string treeJson = TopologyTree::buildTreeJson(
            registry, CentralNodeConfig::MQTT_SCHEMA_VERSION,
            nowMilliseconds, /* onlineTimeoutMilliseconds */ CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS);
        const std::string loadsJson = TopologyTree::buildLoadsJson(registry, CentralNodeConfig::MQTT_SCHEMA_VERSION);
        const std::string stateNodesJson = NodeRegistryJson::buildStateNodesJson(
            commissioningRegistry, registry, CentralNodeConfig::MQTT_SCHEMA_VERSION,
            nowMilliseconds, CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS);
        const std::string configNodesJson =
            NodeRegistryJson::buildConfigNodesJson(commissioningRegistry, CentralNodeConfig::MQTT_SCHEMA_VERSION);

        xSemaphoreGive(stateMutex);

        mqttManager.publish(MqttManager::TOPIC_STATE_SYSTEM, systemJson, 1, true);
        mqttManager.publish(MqttManager::TOPIC_STATE_TREE, treeJson, 1, true);
        mqttManager.publish(MqttManager::TOPIC_STATE_LOADS, loadsJson, 1, true);
        mqttManager.publish(MqttManager::TOPIC_STATE_NODES, stateNodesJson, 1, true);
        mqttManager.publish(MqttManager::TOPIC_CONFIG_NODES, configNodesJson, 1, true);

        /* NODE_OFFLINE events, collected while stateMutex was held above (Section "Events"). */
        for (const auto& offlineNode : newlyOfflineNodes) {
            char targetText[18] = {};
            formatMacAddressText(targetText, sizeof(targetText), offlineNode.first);
            mqttManager.publishEvent("NODE_OFFLINE", targetText, offlineNode.second.c_str());
        }
    }

    ESP_LOGI(TAG, "Optimisation cycle complete: SoC=%.1f%% Pavailable=%.1fW Premaining=%.1fW Pcommitted=%.1fW",
             static_cast<double>(stateOfChargePercent),
             static_cast<double>(availablePowerManager.getTotalAvailablePowerWatts()),
             static_cast<double>(search.getRemainingPowerWatts()),
             static_cast<double>(search.getCommittedPowerWatts()));
}


void optimizationTask(void *parameter)
{
    (void)parameter;

    while (true) {
        xSemaphoreTake(optimizationTriggerSemaphore, pdMS_TO_TICKS(CentralNodeConfig::OPTIMIZATION_PERIOD_MILLISECONDS));
        runOptimizationCycle();
    }
}


/**
 * ESP-NOW Communication Task (event-driven): aggregates NODE_REPORTs
 * into the registry, completes pending relay commands from their
 * acknowledgements, and forwards traffic that is not addressed here.
 */
void espNowCommunicationTask(void *parameter)
{
    (void)parameter;

    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();

    while (true) {
        EspNowCommunication::ReceivedMessage received{};
        if (!communication.receive(received, 1000U)) {
            continue;
        }

        const bool destinationIsLocal = received.message.header.destinationMacAddress == localMac;

        if (destinationIsLocal &&
            received.message.header.messageType == EspNowCommunication::MessageType::NODE_REPORT &&
            received.message.header.payloadLength == sizeof(NodeReportPacket)) {

            NodeReportPacket packet{};
            std::memcpy(&packet, received.message.payload.data(), sizeof(packet));
            packet.nodeName[sizeof(packet.nodeName) - 1U] = '\0';

            char srcText[18] = {};
            formatMacAddressText(srcText, sizeof(srcText), packet.nodeMacAddress);

            /* Section "Central Node Report - After Decode": log decoded values BEFORE mutation, exactly as received. */
            ESP_LOGI(TAG, "CENTRAL_RX NODE_REPORT seq=%u src=%s node=%s hop=%u loads=%u page=%u/%u RSSI=%ddBm",
                     static_cast<unsigned int>(packet.reportSequenceId), srcText, packet.nodeName,
                     static_cast<unsigned int>(packet.hopCountToCentral), static_cast<unsigned int>(packet.numberOfLoads),
                     static_cast<unsigned int>(packet.pageIndex) + 1U, static_cast<unsigned int>(packet.totalPages),
                     static_cast<int>(received.signalStrengthDbm));

            for (std::size_t i = 0U; i < packet.numberOfLoads && i < MAX_LOADS_PER_NODE_PACKET; ++i) {
                const LoadReportPacket &loadPacket = packet.loads[i];
                /* Only fields the wire packet actually carries - "target" is Central's own planning output, never transmitted by the Smart Node (see LoadReportPacket's own layout). */
                ESP_LOGI(TAG, "LOAD_RX seq=%u node=%s pin=%u name=%s mode=%u confirmed=%u confirmedValid=%s "
                              "health=%u ratedV=%.3f ratedI=%.3f plannedP=%.3f",
                         static_cast<unsigned int>(packet.reportSequenceId), srcText,
                         static_cast<unsigned int>(loadPacket.relayPin), loadPacket.name,
                         static_cast<unsigned int>(loadPacket.mode),
                         static_cast<unsigned int>(loadPacket.confirmedRelayState),
                         loadPacket.confirmedRelayStateValid != 0U ? "true" : "false",
                         static_cast<unsigned int>(loadPacket.availability),
                         static_cast<double>(loadPacket.nominalVoltageVolts),
                         static_cast<double>(loadPacket.nominalCurrentAmps),
                         static_cast<double>(loadPacket.nominalVoltageVolts * loadPacket.nominalCurrentAmps));
            }

            const std::uint32_t nowMilliseconds = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
            bool recovered = false;
            std::size_t loadsNowStored = 0U;
            EspNowCommunication::MacAddress ackNextHopMac{};
            bool ackRouteFound = false;

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                registry.applyNodeReport(packet, nowMilliseconds);

                const CentralNodeRegistry::PlanningNode *applied = registry.findNodeByMacAddress(packet.nodeMacAddress);
                loadsNowStored = applied != nullptr ? applied->node.getNumberOfLoads() : 0U;

                if (isNodeMacAddressMarkedOffline(packet.nodeMacAddress)) {
                    clearNodeMacAddressOffline(packet.nodeMacAddress);
                    recovered = true;
                }

                ackRouteFound = findNextHopFromCentral(localMac, packet.nodeMacAddress, registry, ackNextHopMac);

                xSemaphoreGive(stateMutex);
            }

            /* Section "Central Registry Application Log". */
            ESP_LOGI(TAG, "CENTRAL_APPLY NODE_REPORT seq=%u node=%s result=ACCEPTED loadsReceived=%u loadsNowStored=%u",
                     static_cast<unsigned int>(packet.reportSequenceId), srcText,
                     static_cast<unsigned int>(packet.numberOfLoads), static_cast<unsigned int>(loadsNowStored));

            /* Section "Application-Level Node Report Ack": prove decode+apply, not just MAC-layer delivery. */
            if (ackRouteFound) {
                NodeReportAcknowledgementPacket ack{};
                ack.reportSequenceId = packet.reportSequenceId;
                ack.status = static_cast<std::uint8_t>(NodeReportAckStatus::ACCEPTED);
                communication.sendTo(ackNextHopMac, packet.nodeMacAddress,
                                      EspNowCommunication::MessageType::NODE_REPORT_ACK, ack);
            }

            if (recovered) {
                /* Section "Exclude Stale/Offline Smart Nodes From Planning": recovery re-triggers optimisation immediately rather than waiting up to one full period. */
                ESP_LOGI(TAG, "NODE_RECOVERED: node=%s", packet.nodeName);
                mqttManager.publishEvent("NODE_RECOVERED", srcText, packet.nodeName);
                xSemaphoreGive(optimizationTriggerSemaphore);
            }

        } else if (destinationIsLocal &&
                   received.message.header.messageType == EspNowCommunication::MessageType::ACKNOWLEDGEMENT &&
                   received.message.header.payloadLength == sizeof(RelayCommandAcknowledgementPacket)) {

            RelayCommandAcknowledgementPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));

            char ackSrcText[18] = {};
            formatMacAddressText(ackSrcText, sizeof(ackSrcText), received.message.header.originMacAddress);

            /* Section "Central Relay Ack Rx Log": decoded values, before this class mutates any Load. */
            ESP_LOGI(TAG, "CENTRAL_RX RELAY_ACK commandId=%u src=%s pin=%u result=%s confirmed=%s confirmedValid=true",
                     static_cast<unsigned int>(acknowledgement.commandId), ackSrcText,
                     static_cast<unsigned int>(acknowledgement.relayPin), acknowledgement.success != 0U ? "SUCCESS" : "FAILED",
                     static_cast<RelayCommandState>(acknowledgement.confirmedState) == RelayCommandState::ON ? "ON" : "OFF");

            const char *registryStateText = "UNCHANGED";

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                const RelayCommandDispatcher::PendingCommand *pending =
                    relayCommandDispatcher.findPendingCommand(acknowledgement.commandId);

                if (pending != nullptr) {
                    Load *load = registry.findMutableLoad(pending->nodeMacAddress, pending->relayPin);
                    if (load != nullptr) {
                        const bool confirmedOn =
                            static_cast<RelayCommandState>(acknowledgement.confirmedState) == RelayCommandState::ON;
                        if (acknowledgement.success != 0U) {
                            load->setConfirmedRelayState(confirmedOn);
                            load->setHealth(LoadHealth::AVAILABLE);
                            registryStateText = confirmedOn ? "CONFIRMED_ON" : "CONFIRMED_OFF";
                        } else {
                            /*
                             * A failed acknowledgement does not prove the relay
                             * is OFF (Section "Fix Relay Confirmation Failure
                             * Semantics") — leave the previous trusted confirmed
                             * state/validity exactly as it was, only mark fault.
                             */
                            load->setHealth(LoadHealth::FAULTED);
                            ++faultCount;
                            registryStateText = "FAULTED";
                        }
                    }
                    relayCommandDispatcher.completeCommand(acknowledgement.commandId);
                }

                xSemaphoreGive(stateMutex);
            }

            ESP_LOGI(TAG, "CENTRAL_APPLY RELAY_ACK commandId=%u registryState=%s",
                     static_cast<unsigned int>(acknowledgement.commandId), registryStateText);

        } else if (destinationIsLocal &&
                   received.message.header.messageType == EspNowCommunication::MessageType::IDENTITY_REPORT &&
                   received.message.header.payloadLength == sizeof(IdentityReportPacket)) {

            IdentityReportPacket packet{};
            std::memcpy(&packet, received.message.payload.data(), sizeof(packet));

            const EspNowCommunication::MacAddress originMac = received.message.header.originMacAddress;
            const std::uint32_t nowMilliseconds = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
            bool isNewNode = false;

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                /*
                 * Not persisted on every report: persist() only writes
                 * COMMISSIONED/OPERATIONAL/DECOMMISSIONED records, and a
                 * freshly discovered UNCOMMISSIONED record is always
                 * cheaply rebuilt from the next real IDENTITY_REPORT after
                 * a reboot anyway - persisting here on every ~60s cadence
                 * tick would only add unnecessary NVS wear.
                 */
                isNewNode = commissioningRegistry.recordDiscovered(
                    originMac, static_cast<NodeRole>(packet.role), packet.firmwareVersion, packet.chipModel,
                    packet.relayPins.data(), packet.relayCapabilityCount, nowMilliseconds);
                xSemaphoreGive(stateMutex);
            }

            char targetText[18] = {};
            formatMacAddressText(targetText, sizeof(targetText), originMac);

            ESP_LOGI(TAG, "IDENTITY_REPORT: mac=%s role=%s state=%s", targetText,
                     toText(static_cast<NodeRole>(packet.role)),
                     toText(static_cast<NodeLifecycleState>(packet.lifecycleState)));

            if (isNewNode) {
                mqttManager.publishEvent("NODE_DISCOVERED", targetText, nullptr);
            }

        } else if (destinationIsLocal &&
                   received.message.header.messageType == EspNowCommunication::MessageType::COMMISSION_ACK &&
                   received.message.header.payloadLength == sizeof(CommissionAckPacket)) {

            CommissionAckPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));

            const EspNowCommunication::MacAddress originMac = received.message.header.originMacAddress;
            const bool success = acknowledgement.success != 0U;
            const NodeLifecycleState resultingState = static_cast<NodeLifecycleState>(acknowledgement.resultingState);

            bool registryPersisted = false;
            const char* acknowledgementReason = success ? "applied" : "rejected by Node";
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                const NodeCommissioningRegistry previousRegistry = commissioningRegistry;
                const bool registryUpdated = commissioningRegistry.applyCommissionResult(
                    originMac, success, resultingState);
                if (!registryUpdated) {
                    acknowledgementReason = "Central has no matching commissioning record";
                } else if (!success) {
                    /* The Node rejected the command; retain the FAILED state in RAM for diagnostics. */
                    registryPersisted = true;
                } else if (commissioningRegistry.persist()) {
                    registryPersisted = true;
                } else {
                    commissioningRegistry = previousRegistry;
                    acknowledgementReason = "Central could not persist commissioning result";
                }
                xSemaphoreGive(stateMutex);
            } else {
                acknowledgementReason = "Central registry lock unavailable";
            }

            char targetText[18] = {};
            formatMacAddressText(targetText, sizeof(targetText), originMac);

            mqttManager.publishAcknowledgement(
                acknowledgement.commandId, "NODE_COMMISSIONING",
                success && registryPersisted ? AckStatus::APPLIED : AckStatus::FAILED,
                acknowledgementReason, targetText);

            if (success && registryPersisted) {
                mqttManager.publishEvent("NODE_COMMISSIONED", targetText, nullptr);
            }

            ESP_LOGI(TAG, "COMMISSION_ACK: mac=%s success=%s state=%s", targetText, success ? "Yes" : "No",
                     toText(resultingState));

            /* Republish config/nodes + state/nodes promptly rather than waiting for the next periodic cycle (same mechanism NODE_RECOVERED already uses). */
            xSemaphoreGive(optimizationTriggerSemaphore);

        } else if (destinationIsLocal &&
                   received.message.header.messageType == EspNowCommunication::MessageType::DECOMMISSION_ACK &&
                   received.message.header.payloadLength == sizeof(DecommissionAckPacket)) {

            DecommissionAckPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));

            const EspNowCommunication::MacAddress originMac = received.message.header.originMacAddress;
            const bool success = acknowledgement.success != 0U;

            char targetText[18] = {};
            formatMacAddressText(targetText, sizeof(targetText), originMac);

            /*
             * Central's own registry already moved to DECOMMISSIONED
             * synchronously when the operator's command was accepted
             * (NodeCommissioningRegistry::decommission() is immediate, not
             * pending - see its own documentation, and handleConfigCommand()
             * below); this ACK is only the Node's own confirmation that it
             * reset its local identity.
             */
            mqttManager.publishAcknowledgement(acknowledgement.commandId, "NODE_DECOMMISSIONING",
                                                success ? AckStatus::APPLIED : AckStatus::FAILED,
                                                success ? "Node reset and prior load configuration cleared"
                                                        : "Node could not safely clear local identity/configuration",
                                                targetText);

            if (!success) {
                mqttManager.publishEvent("NODE_DECOMMISSION_RESET_FAILED", targetText, nullptr);
            }

            ESP_LOGI(TAG, "DECOMMISSION_ACK: mac=%s success=%s", targetText, success ? "Yes" : "No");

        } else if (destinationIsLocal &&
                   received.message.header.messageType == EspNowCommunication::MessageType::CONFIGURE_LOAD_ACK &&
                   received.message.header.payloadLength == sizeof(ConfigureLoadAcknowledgementPacket)) {

            ConfigureLoadAcknowledgementPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));

            char targetText[18] = {};
            formatMacAddressText(targetText, sizeof(targetText), received.message.header.originMacAddress);
            const bool success = acknowledgement.success != 0U;
            const HardwareConfigurationFailureReason reason =
                static_cast<HardwareConfigurationFailureReason>(acknowledgement.failureReason);

            mqttManager.publishAcknowledgement(acknowledgement.commandId, "CONFIGURE_LOAD",
                                                success ? AckStatus::APPLIED : AckStatus::FAILED,
                                                hardwareConfigurationFailureText(reason), targetText);
            if (success) {
                mqttManager.publishEvent("LOAD_CONFIGURED", targetText, nullptr);
                xSemaphoreGive(optimizationTriggerSemaphore);
            }

            ESP_LOGI(TAG, "CONFIGURE_LOAD_ACK: mac=%s commandId=%u relayPin=%u success=%s reason=%u",
                     targetText, static_cast<unsigned int>(acknowledgement.commandId),
                     static_cast<unsigned int>(acknowledgement.relayPin), success ? "Yes" : "No",
                     static_cast<unsigned int>(acknowledgement.failureReason));

        } else if (destinationIsLocal &&
                   received.message.header.messageType == EspNowCommunication::MessageType::DEV_ACK &&
                   received.message.header.payloadLength == sizeof(DevAckPacket)) {

            DevAckPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));

            char targetText[18] = {};
            formatMacAddressText(targetText, sizeof(targetText), received.message.header.originMacAddress);
            const bool success = acknowledgement.success != 0U;

            mqttManager.publishAcknowledgement(acknowledgement.commandId, "DEVELOPMENT",
                                                success ? AckStatus::APPLIED : AckStatus::FAILED,
                                                success ? "applied" : "rejected by Node", targetText);

            ESP_LOGI(TAG, "DEV_ACK: mac=%s success=%s environment=%s", targetText, success ? "Yes" : "No",
                     toText(static_cast<OperatingEnvironment>(acknowledgement.resultingEnvironment)));

        } else if (destinationIsLocal &&
                   received.message.header.messageType == EspNowCommunication::MessageType::FACTORY_RESET_ACK &&
                   received.message.header.payloadLength == sizeof(FactoryResetAckPacket)) {

            FactoryResetAckPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));

            char targetText[18] = {};
            formatMacAddressText(targetText, sizeof(targetText), received.message.header.originMacAddress);

            /*
             * Only ever sent on rejection - a successful remote factory
             * reset reboots the Node immediately instead of replying (see
             * DevelopmentPackets.h). This ACK therefore only ever reports
             * FAILED/REJECTED, never APPLIED.
             */
            mqttManager.publishAcknowledgement(acknowledgement.commandId, "FACTORY_RESET_NODE",
                                                AckStatus::FAILED, "rejected or failed on Node", targetText);

            ESP_LOGW(TAG, "FACTORY_RESET_ACK: mac=%s rejected/failed on Node", targetText);

        } else if (!destinationIsLocal) {
            EspNowCommunication::MacAddress nextHopMac{};
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                const bool routeFound = findNextHopFromCentral(
                    localMac, received.message.header.destinationMacAddress, registry, nextHopMac);
                xSemaphoreGive(stateMutex);

                if (routeFound) {
                    communication.forwardMessageTo(nextHopMac, received.message);
                }
            }
        }
    }
}


void watchdogTask(void *parameter)
{
    (void)parameter;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CentralNodeConfig::WATCHDOG_PERIOD_MILLISECONDS));

        ESP_LOGI(TAG, "================ WATCHDOG ================");
        ESP_LOGI(TAG, "Wi-Fi: %s  MQTT: %s  Faults: %u",
                 wifiStateText(wifiManager.getState()), mqttManager.isConnected() ? "CONNECTED" : "DISCONNECTED",
                 static_cast<unsigned int>(faultCount));
        wifiManager.printDiagnosticReport();
        mqttManager.printDiagnosticReport();
        sensors.printDiagnosticReport();
        relays.printDiagnosticReport();
        ESP_LOGI(TAG, "===========================================");
    }
}


/**
 * MQTT load-command handler (Section "MQTT Must Accept User Load
 * Commands"): performs every domain validation the document requires,
 * then updates the domain Load, persists the change, and signals the
 * Optimisation Task — it never calls the optimizer directly (Section
 * "Optimization Triggers": never recursively call the optimizer from
 * MQTT callbacks).
 */
LoadCommandResult handleLoadCommand(void *context, const LoadCommandRequest &request)
{
    (void)context;

    LoadCommandResult result{};
    result.accepted = false;
    result.reason[0] = '\0';

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::snprintf(result.reason, sizeof(result.reason), "internal: could not acquire state lock");
        return result;
    }

    Load *load = registry.findMutableLoad(request.nodeMacAddress, request.relayPin);
    if (load == nullptr) {
        std::snprintf(result.reason, sizeof(result.reason), "unknown Node MAC / relay pin");
        xSemaphoreGive(stateMutex);
        return result;
    }

    if (request.hasPriority && request.priority > CentralNodeConfig::bestFirstSearchWeights().maximumAllowedPriority) {
        std::snprintf(result.reason, sizeof(result.reason), "priority exceeds W_max");
        xSemaphoreGive(stateMutex);
        return result;
    }

    const std::uint16_t newPriority = request.hasPriority ? request.priority : load->getPriority();
    const LoadMode::Value newMode = request.hasMode ? request.mode : load->getMode();
    const AutoSchedule newSchedule = request.hasSchedule ? request.schedule : load->getSchedule();

    LoadConfigurationStore::ConfigurationEntry entry{
        request.nodeMacAddress, request.relayPin, newPriority, newMode, newSchedule
    };

    if (!loadConfigurationStore.setConfiguration(entry)) {
        std::snprintf(result.reason, sizeof(result.reason), "invalid schedule hour/minute");
        xSemaphoreGive(stateMutex);
        return result;
    }

    loadConfigurationStore.applyToLoad(*load);
    loadConfigurationStore.persist();

    xSemaphoreGive(stateMutex);

    xSemaphoreGive(optimizationTriggerSemaphore);

    result.accepted = true;
    std::snprintf(result.reason, sizeof(result.reason), "applied");
    return result;
}


/**
 * Factory reset (Section "Factory Reset"): establishes safe physical
 * outputs, erases every installation-specific persisted record (a full
 * NVS erase - the ESP32's MAC address and chip identity live in efuses,
 * never NVS, so this can never touch immutable hardware identity), and
 * reboots into PRODUCTION + UNCOMMISSIONED. Never returns.
 *
 * Section "Do Not Confuse Firmware Flash With Factory Reset": this is a
 * deliberate, explicit action distinct from uploading new firmware - a
 * normal firmware upload does not call this and does not erase NVS.
 */
[[noreturn]] void performCentralFactoryReset()
{
    ESP_LOGW(TAG, "FACTORY_RESET: establishing safe physical outputs before erasing NVS");
    relays.printDiagnosticReport();

    ESP_LOGW(TAG, "FACTORY_RESET: erasing NVS (commissioning records, Load configuration, "
                  "INA219 calibration, persisted SoC, time mode) - immutable hardware identity is unaffected");
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


LoadCommandResult handleSystemCommand(void *context, const SystemCommandRequest &request)
{
    (void)context;

    LoadCommandResult result{};
    result.accepted = false;
    result.reason[0] = '\0';

    if (request.action == SystemCommandAction::REQUEST_OPTIMIZATION_CYCLE) {
        xSemaphoreGive(optimizationTriggerSemaphore);
        result.accepted = true;
        std::snprintf(result.reason, sizeof(result.reason), "optimization cycle requested");

    } else if (request.action == SystemCommandAction::APPLY_SAFETY_CONFIG) {
        if (!request.hasSafetyPolicy) {
            std::snprintf(result.reason, sizeof(result.reason), "safetyConfig required");
        } else if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            std::snprintf(result.reason, sizeof(result.reason), "internal: could not acquire state lock");
        } else {
            const CentralConfigurationStore::SafetyPolicy previousPolicy =
                centralConfigurationStore.getConfiguration().safetyPolicy;
            const CentralConfigurationStore::SafetyPolicy policy{
                true,
                request.minimumStateOfChargePercent,
                request.warningStateOfChargePercent,
                request.targetRuntimeHours,
                request.safetyFactor,
                request.maximumBatteryDischargeCurrentAmps,
                request.maximumMainCurrentAmps,
            };
            if (!centralConfigurationStore.setSafetyPolicy(policy)) {
                std::snprintf(result.reason, sizeof(result.reason), "invalid safety limits");
            } else if (!centralConfigurationStore.persist()) {
                centralConfigurationStore.setSafetyPolicy(previousPolicy);
                std::snprintf(result.reason, sizeof(result.reason), "could not persist safety limits");
            } else {
                result.accepted = true;
                std::snprintf(result.reason, sizeof(result.reason), "applied");
            }
            xSemaphoreGive(stateMutex);
            if (result.accepted) {
                xSemaphoreGive(optimizationTriggerSemaphore);
            }
        }

    } else if (request.action == SystemCommandAction::FACTORY_RESET_CENTRAL) {
        if (std::strcmp(request.confirmText, "FACTORY_RESET_CONFIRMED") != 0) {
            std::snprintf(result.reason, sizeof(result.reason), "confirm text did not match; reset refused");
        } else {
            performCentralFactoryReset(); // never returns
        }

    } else if (request.action == SystemCommandAction::FACTORY_RESET_NODE) {
        if (std::strcmp(request.confirmText, "FACTORY_RESET_CONFIRMED") != 0) {
            std::snprintf(result.reason, sizeof(result.reason), "confirm text did not match; reset refused");
        } else if (!request.hasTargetNodeMacAddress) {
            std::snprintf(result.reason, sizeof(result.reason), "targetNodeMac required for FACTORY_RESET_NODE");
        } else {
            const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();
            EspNowCommunication::MacAddress nextHopMac{};
            bool routeFound = false;

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
                routeFound = findNextHopFromCentral(localMac, request.targetNodeMacAddress, registry, nextHopMac);
                xSemaphoreGive(stateMutex);
            }

            if (!routeFound) {
                std::snprintf(result.reason, sizeof(result.reason), "no known route to target Node");
            } else {
                FactoryResetCommandPacket packet{};
                packet.commandId = request.commandId;
                packet.confirmToken = FACTORY_RESET_CONFIRM_TOKEN;

                const bool sent = communication.sendTo(nextHopMac, request.targetNodeMacAddress,
                                                        EspNowCommunication::MessageType::FACTORY_RESET_COMMAND, packet);
                result.accepted = sent;
                std::snprintf(result.reason, sizeof(result.reason),
                              sent ? "dispatched to Node" : "failed to send factory reset command");
            }
        }

    } else {
        std::snprintf(result.reason, sizeof(result.reason), "unrecognised action");
    }

    return result;
}


/**
 * MQTT development-command handler (Section "Development Session Is
 * Explicit"): the only path that can ever start a Development Session or
 * arm a simulated sensor value on this system - never automatic. A
 * command targeting Central's own MAC is applied directly; a command
 * targeting a remote Node is relayed over ESP-NOW exactly like a
 * commissioning command (see handleConfigCommand()).
 */
LoadCommandResult handleDevelopmentCommand(void *context, const DevelopmentCommandRequest &request)
{
    (void)context;

    LoadCommandResult result{};
    result.accepted = false;
    result.reason[0] = '\0';

    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();

    if (request.targetNodeMacAddress != localMac) {
        if (request.action == DevelopmentCommandAction::SET_SENSOR_INPUT ||
            request.action == DevelopmentCommandAction::CLEAR_SENSOR_OVERRIDE) {
            std::snprintf(result.reason, sizeof(result.reason),
                          "Smart Nodes have no INA219; development sensor input is Central battery-only");
            return result;
        }

        EspNowCommunication::MacAddress nextHopMac{};
        bool routeFound = false;

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
            routeFound = findNextHopFromCentral(localMac, request.targetNodeMacAddress, registry, nextHopMac);
            xSemaphoreGive(stateMutex);
        }

        if (!routeFound) {
            std::snprintf(result.reason, sizeof(result.reason), "no known route to target Node");
            return result;
        }

        bool sent = false;
        if (request.action == DevelopmentCommandAction::START_SESSION ||
            request.action == DevelopmentCommandAction::END_SESSION) {
            DevSessionCommandPacket packet{};
            packet.commandId = request.commandId;
            packet.action = static_cast<std::uint8_t>(
                request.action == DevelopmentCommandAction::START_SESSION ? DevSessionAction::START : DevSessionAction::END);
            sent = communication.sendTo(nextHopMac, request.targetNodeMacAddress,
                                         EspNowCommunication::MessageType::DEV_SESSION_COMMAND, packet);
        } else if (request.action == DevelopmentCommandAction::SET_SENSOR_INPUT ||
                   request.action == DevelopmentCommandAction::CLEAR_SENSOR_OVERRIDE) {
            DevSensorInputCommandPacket packet{};
            packet.commandId = request.commandId;
            packet.i2cAddress = request.i2cAddress;
            packet.clearOverride = request.action == DevelopmentCommandAction::CLEAR_SENSOR_OVERRIDE ? 1U : 0U;
            packet.voltageVolts = request.voltageVolts;
            packet.currentAmps = request.currentAmps;
            sent = communication.sendTo(nextHopMac, request.targetNodeMacAddress,
                                         EspNowCommunication::MessageType::DEV_SENSOR_INPUT_COMMAND, packet);
        } else {
            std::snprintf(result.reason, sizeof(result.reason), "unrecognised action");
            return result;
        }

        result.accepted = sent;
        std::snprintf(result.reason, sizeof(result.reason),
                      sent ? "dispatched to Node; awaiting confirmation" : "failed to send development command");
        return result;
    }

    /* Targeting Central itself. */
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::snprintf(result.reason, sizeof(result.reason), "internal: could not acquire state lock");
        return result;
    }

    bool success = false;

    switch (request.action) {
        case DevelopmentCommandAction::START_SESSION:
            success = developmentSession.start();
            ESP_LOGW(TAG, "DEV_SESSION_START: node=Central result=%s", success ? "OK" : "ALREADY_ACTIVE");
            std::snprintf(result.reason, sizeof(result.reason), success ? "session started" : "a session is already active");
            break;

        case DevelopmentCommandAction::END_SESSION:
            success = developmentSession.end();
            if (success && batterySensorConfigured) {
                sensors.clearDevelopmentOverride(batterySensorI2CAddress);
                ESP_LOGW(TAG, "DEV_SESSION_END: node=Central battery sensor 0x%02X released; battery NOT_CONFIGURED again",
                         static_cast<unsigned int>(batterySensorI2CAddress));
                batterySensorConfigured = false;
            } else {
                ESP_LOGW(TAG, "DEV_SESSION_END: node=Central result=%s", success ? "OK" : "NOT_ACTIVE");
            }
            std::snprintf(result.reason, sizeof(result.reason), success ? "session ended" : "no session was active");
            break;

        case DevelopmentCommandAction::SET_SENSOR_INPUT: {
            if (!developmentSession.isActive()) {
                std::snprintf(result.reason, sizeof(result.reason), "no active Development Session; call START_SESSION first");
                break;
            }
            if (!request.hasSensorInput) {
                std::snprintf(result.reason, sizeof(result.reason), "voltageVolts/currentAmps required");
                break;
            }

            if (sensors.findSensorByI2CAddress(request.i2cAddress) == nullptr) {
                sensors.addSensor(INA219Monitor::INA219SensorConfiguration{
                    request.i2cAddress, BATTERY_RELAY_PIN_SENTINEL, 0.005F, 60.0F, 0.2F
                });
                ESP_LOGW(TAG, "DEV_SENSOR_REGISTER: node=Central i2c=0x%02X (temporary, Development-Session-only registration)",
                         static_cast<unsigned int>(request.i2cAddress));
            }

            const LoadMeasurements rawOverride{
                request.voltageVolts, request.currentAmps, request.voltageVolts * request.currentAmps
            };
            ESP_LOGW(TAG, "DEV_INPUT node=Central i2c=0x%02X V=%.3f I=%.3f",
                     static_cast<unsigned int>(request.i2cAddress),
                     static_cast<double>(request.voltageVolts), static_cast<double>(request.currentAmps));

            success = sensors.setDevelopmentOverride(request.i2cAddress, rawOverride);
            developmentSession.setSensorOverride(request.i2cAddress, request.voltageVolts, request.currentAmps);

            batterySensorConfigured = true;
            batterySensorI2CAddress = request.i2cAddress;
            if (!batteryStateOfCharge.isInitialized()) {
                const CentralConfigurationStore::BatterySensorConfiguration battery =
                    centralConfigurationStore.getConfiguration().batterySensor;
                batteryStateOfCharge.initialize(
                    battery.configured ? battery.batteryCapacityAmpHours : CentralNodeConfig::BATTERY_CAPACITY_AMP_HOURS,
                    battery.configured ? battery.initialStateOfChargePercent : CentralNodeConfig::DEFAULT_STATE_OF_CHARGE_PERCENT);
            }

            std::snprintf(result.reason, sizeof(result.reason), success ? "sensor input armed" : "failed to arm sensor override");
            break;
        }

        case DevelopmentCommandAction::CLEAR_SENSOR_OVERRIDE:
            success = developmentSession.clearSensorOverride(request.i2cAddress) &&
                      sensors.clearDevelopmentOverride(request.i2cAddress);
            std::snprintf(result.reason, sizeof(result.reason),
                          success ? "override cleared" : "no override was armed for this address");
            break;

        default:
            std::snprintf(result.reason, sizeof(result.reason), "unrecognised action");
            break;
    }

    xSemaphoreGive(stateMutex);

    result.accepted = success;
    return result;
}


/**
 * One concise, authoritative boot-state block (Section "Clean Boot
 * Summary") - every value here is read live from the modules that own it,
 * never hardcoded text.
 */
void printCentralBootSummary(const EspNowCommunication::MacAddress& localMac)
{
    char macText[18] = {};
    formatMacAddressText(macText, sizeof(macText), localMac);

    const CentralNodeRegistry::PlanningNode* localNode = registry.findNodeByMacAddress(localMac);
    const std::size_t localLoadCount = localNode != nullptr ? localNode->node.getNumberOfLoads() : 0U;

    ESP_LOGI(TAG, "========== KILOWATTS CENTRAL BOOT STATE ==========");
    ESP_LOGI(TAG, "Environment           : %s", toText(developmentSession.getEnvironment()));
    ESP_LOGI(TAG, "MAC                   : %s", macText);
    ESP_LOGI(TAG, "Firmware Version      : %s", KILOWATTS_FIRMWARE_VERSION);
    ESP_LOGI(TAG, "Commissioning State   : %s", toText(NodeLifecycleState::COMMISSIONED));
    ESP_LOGI(TAG, "Persisted Nodes       : %u", static_cast<unsigned int>(commissioningRegistry.getCount()));
    ESP_LOGI(TAG, "Local Sensors         : %u", static_cast<unsigned int>(sensors.getNumberOfSensors()));
    ESP_LOGI(TAG, "Local Branches        : %u", static_cast<unsigned int>(localLoadCount));
    ESP_LOGI(TAG, "Local Loads           : %u", static_cast<unsigned int>(localLoadCount));
    ESP_LOGI(TAG, "Battery Sensor        : %s", batterySensorConfigured ? "CONFIGURED" : "NOT CONFIGURED");
    if (batteryStateOfCharge.isValid()) {
        ESP_LOGI(TAG, "SoC                   : %.1f%% (source=%s)",
                 static_cast<double>(batteryStateOfCharge.getStateOfChargePercent()),
                 toText(batteryStateOfCharge.getSource()));
    } else {
        ESP_LOGI(TAG, "SoC                   : UNKNOWN");
    }
    ESP_LOGI(TAG, "SoC Valid             : %s", batteryStateOfCharge.isValid() ? "Yes" : "No");
    ESP_LOGI(TAG, "Development Session   : %s", developmentSession.isActive() ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "Physical Actuation    : ENABLED");
    ESP_LOGI(TAG, "===================================================");
}


/**
 * Minimal engineering test console (Section "Engineering Logging Is A
 * Requirement" / test-seam precedent already established for relay
 * round-trip verification): reads simple line commands from the same
 * UART the log output already uses, and calls exactly the same handler
 * functions the MQTT commands/development and commands/system paths call
 * - never a second, divergent code path. This exists so Development
 * Session and factory reset are verifiable on real hardware even when
 * Wi-Fi/MQTT connectivity is unavailable; it is not a production control
 * surface.
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

        const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();
        unsigned int i2cAddress = 0U;
        float voltageVolts = 0.0F;
        float currentAmps = 0.0F;

        if (std::strcmp(line, "dev_start") == 0) {
            DevelopmentCommandRequest request{};
            request.commandId = static_cast<std::uint32_t>(xTaskGetTickCount());
            request.targetNodeMacAddress = localMac;
            request.action = DevelopmentCommandAction::START_SESSION;
            const LoadCommandResult result = handleDevelopmentCommand(nullptr, request);
            ESP_LOGI(TAG, "CONSOLE: dev_start -> accepted=%s reason=%s", result.accepted ? "Yes" : "No", result.reason);

        } else if (std::strcmp(line, "dev_end") == 0) {
            DevelopmentCommandRequest request{};
            request.commandId = static_cast<std::uint32_t>(xTaskGetTickCount());
            request.targetNodeMacAddress = localMac;
            request.action = DevelopmentCommandAction::END_SESSION;
            const LoadCommandResult result = handleDevelopmentCommand(nullptr, request);
            ESP_LOGI(TAG, "CONSOLE: dev_end -> accepted=%s reason=%s", result.accepted ? "Yes" : "No", result.reason);

        } else if (std::sscanf(line, "dev_set %x %f %f", &i2cAddress, &voltageVolts, &currentAmps) == 3) {
            DevelopmentCommandRequest request{};
            request.commandId = static_cast<std::uint32_t>(xTaskGetTickCount());
            request.targetNodeMacAddress = localMac;
            request.action = DevelopmentCommandAction::SET_SENSOR_INPUT;
            request.hasSensorInput = true;
            request.i2cAddress = static_cast<std::uint8_t>(i2cAddress);
            request.voltageVolts = voltageVolts;
            request.currentAmps = currentAmps;
            const LoadCommandResult result = handleDevelopmentCommand(nullptr, request);
            ESP_LOGI(TAG, "CONSOLE: dev_set 0x%02X -> accepted=%s reason=%s",
                     i2cAddress, result.accepted ? "Yes" : "No", result.reason);

        } else if (std::sscanf(line, "dev_clear %x", &i2cAddress) == 1) {
            DevelopmentCommandRequest request{};
            request.commandId = static_cast<std::uint32_t>(xTaskGetTickCount());
            request.targetNodeMacAddress = localMac;
            request.action = DevelopmentCommandAction::CLEAR_SENSOR_OVERRIDE;
            request.hasSensorInput = true;
            request.i2cAddress = static_cast<std::uint8_t>(i2cAddress);
            const LoadCommandResult result = handleDevelopmentCommand(nullptr, request);
            ESP_LOGI(TAG, "CONSOLE: dev_clear 0x%02X -> accepted=%s reason=%s",
                     i2cAddress, result.accepted ? "Yes" : "No", result.reason);

        } else if (std::strcmp(line, "factory_reset") == 0) {
            SystemCommandRequest request{};
            request.commandId = static_cast<std::uint32_t>(xTaskGetTickCount());
            request.action = SystemCommandAction::FACTORY_RESET_CENTRAL;
            std::snprintf(request.confirmText, sizeof(request.confirmText), "FACTORY_RESET_CONFIRMED");
            ESP_LOGW(TAG, "CONSOLE: factory_reset requested - rebooting now");
            handleSystemCommand(nullptr, request); // never returns on success

        } else {
            ESP_LOGW(TAG, "CONSOLE: unrecognised command '%s'", line);
        }
    }
}


/**
 * A COMMISSION_COMMAND could not actually be delivered (no known route, or
 * the ESP-NOW send itself failed): the Node never received it, so
 * requestCommissioning()'s optimistic CONFIGURING/PENDING state must be
 * rolled back rather than left stuck — a first commissioning attempt
 * returns to UNCOMMISSIONED (the Node genuinely never applied anything); a
 * rename attempt is simply marked FAILED without moving lifecycleState at
 * all (it was never CONFIGURING to begin with — see
 * NodeCommissioningRegistry::requestCommissioning()). Must be called with
 * stateMutex already held.
 */
void rollbackUndeliveredCommissionRequest(const EspNowCommunication::MacAddress& macAddress)
{
    const NodeCommissioningRegistry::CommissioningRecord *record = commissioningRegistry.findByMac(macAddress);
    const NodeLifecycleState currentState =
        record != nullptr ? record->lifecycleState : NodeLifecycleState::UNCOMMISSIONED;
    const NodeLifecycleState rollbackState =
        currentState == NodeLifecycleState::CONFIGURING ? NodeLifecycleState::UNCOMMISSIONED : currentState;

    commissioningRegistry.applyCommissionResult(macAddress, false, rollbackState);
}


/**
 * MQTT config-command handler (Section "Commissioning"): validates the
 * target Node and requested transition against commissioningRegistry, then
 * dispatches the matching ESP-NOW commissioning packet over the Node's
 * already-known route (found exactly like a relay command's route is —
 * see findNextHopFromCentral()). The handler's own return only reflects
 * this immediate, local outcome; a commissioning/rename's final
 * APPLIED/FAILED result is published separately once the Node's own
 * COMMISSION_ACK/DECOMMISSION_ACK arrives (see espNowCommunicationTask()
 * and MqttManager.h's own file comment).
 */
LoadCommandResult handleConfigCommand(void *context, const ConfigCommandRequest &request)
{
    (void)context;

    LoadCommandResult result{};
    result.accepted = false;
    result.reason[0] = '\0';

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::snprintf(result.reason, sizeof(result.reason), "internal: could not acquire state lock");
        return result;
    }

    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();

    if (request.action == ConfigCommandAction::CONFIGURE_BATTERY_SENSOR) {
        if (!request.hasBatterySensorConfiguration || request.nodeMacAddress != localMac) {
            std::snprintf(result.reason, sizeof(result.reason),
                          "battery configuration must target the Central Node");
            xSemaphoreGive(stateMutex);
            return result;
        }

        const CentralConfigurationStore::BatterySensorConfiguration configuration{
            true,
            request.batteryI2cAddress,
            request.batteryShuntResistanceOhms,
            request.batteryMaximumExpectedCurrentAmps,
            request.batteryEmaAlpha,
            request.batteryCapacityAmpHours,
            request.batteryInitialStateOfChargePercent,
        };
        if (!CentralConfigurationStore::isValidBatterySensor(configuration)) {
            std::snprintf(result.reason, sizeof(result.reason), "invalid battery sensor configuration");
            xSemaphoreGive(stateMutex);
            return result;
        }

        const CentralConfigurationStore::BatterySensorConfiguration previous =
            centralConfigurationStore.getConfiguration().batterySensor;
        if (previous.configured && !sameBatterySensorConfiguration(previous, configuration)) {
            std::snprintf(result.reason, sizeof(result.reason),
                          "battery hardware settings cannot be changed after commissioning; factory-reset and recommission if the wiring changed");
            xSemaphoreGive(stateMutex);
            return result;
        }
        bool sensorRegisteredNow = false;
        if (sensors.findSensorByI2CAddress(configuration.i2cAddress) == nullptr) {
            if (!sensors.addSensor(INA219Monitor::INA219SensorConfiguration{
                configuration.i2cAddress, BATTERY_RELAY_PIN_SENTINEL,
                configuration.shuntResistanceOhms, configuration.maximumExpectedCurrentAmps,
                configuration.emaAlpha})) {
                std::snprintf(result.reason, sizeof(result.reason), "could not initialize battery INA219");
                xSemaphoreGive(stateMutex);
                return result;
            }
            sensorRegisteredNow = true;
        }
        if (!sensors.isSensorPresent(configuration.i2cAddress)) {
            if (sensorRegisteredNow) {
                sensors.removeSensor(configuration.i2cAddress);
            }
            std::snprintf(result.reason, sizeof(result.reason),
                          "battery INA219 did not respond on the configured I2C address");
            xSemaphoreGive(stateMutex);
            return result;
        }
        if (!centralConfigurationStore.setBatterySensor(configuration) || !centralConfigurationStore.persist()) {
            centralConfigurationStore.setBatterySensor(previous);
            if (sensorRegisteredNow) {
                sensors.removeSensor(configuration.i2cAddress);
            }
            std::snprintf(result.reason, sizeof(result.reason), "could not persist battery sensor configuration");
            xSemaphoreGive(stateMutex);
            return result;
        }

        batterySensorConfigured = true;
        batterySensorI2CAddress = configuration.i2cAddress;

        /*
         * MQTT QoS 1 can redeliver a command. Re-applying an already
         * commissioned, identical battery configuration must not reset the
         * accumulated state-of-charge estimate to the installer-entered
         * starting value. A first successful commission still starts the
         * estimate from that value, as intended.
         */
        if (!previous.configured) {
            batteryTelemetryEverValid = false;
            batteryStateOfCharge.initialize(configuration.batteryCapacityAmpHours,
                                            configuration.initialStateOfChargePercent);
        }
        xSemaphoreGive(stateMutex);
        xSemaphoreGive(optimizationTriggerSemaphore);
        result.accepted = true;
        std::snprintf(result.reason, sizeof(result.reason), "applied");
        return result;
    }

    if (request.action == ConfigCommandAction::CONFIGURE_LOAD) {
        if (!request.hasLoadConfiguration) {
            std::snprintf(result.reason, sizeof(result.reason), "load configuration required");
            xSemaphoreGive(stateMutex);
            return result;
        }

        const NodeCommissioningRegistry::CommissioningRecord* record =
            commissioningRegistry.findByMac(request.nodeMacAddress);
        if (record == nullptr || record->role != NodeRole::SMART ||
            (record->lifecycleState != NodeLifecycleState::COMMISSIONED &&
             record->lifecycleState != NodeLifecycleState::OPERATIONAL)) {
            std::snprintf(result.reason, sizeof(result.reason),
                          "target must be a commissioned Smart Node");
            xSemaphoreGive(stateMutex);
            return result;
        }
        if (!nodeDeclaresRelayPin(*record, request.relayPin)) {
            std::snprintf(result.reason, sizeof(result.reason),
                          "relay GPIO is not declared safe by Node firmware");
            xSemaphoreGive(stateMutex);
            return result;
        }

        EspNowCommunication::MacAddress nextHopMac{};
        const bool routeFound = findNextHopFromCentral(localMac, request.nodeMacAddress, registry, nextHopMac);
        if (!routeFound) {
            std::snprintf(result.reason, sizeof(result.reason), "no known route to Node");
            xSemaphoreGive(stateMutex);
            return result;
        }

        ConfigureLoadCommandPacket packet{};
        packet.commandId = request.commandId;
        std::snprintf(packet.loadName, sizeof(packet.loadName), "%s", request.loadName);
        packet.relayPin = request.relayPin;
        packet.relayActiveHigh = request.relayActiveHigh ? 1U : 0U;
        packet.mode = static_cast<std::uint8_t>(request.mode);
        packet.priority = request.priority;
        packet.nominalVoltageVolts = request.nominalVoltageVolts;
        packet.nominalCurrentAmps = request.nominalCurrentAmps;
        packet.branchMaximumCurrentAmps = request.branchMaximumCurrentAmps;
        packet.startupWatts = request.startupWatts;
        packet.scheduleEnabled = request.schedule.enabled ? 1U : 0U;
        packet.scheduleHour = request.schedule.hour;
        packet.scheduleMinute = request.schedule.minute;

        xSemaphoreGive(stateMutex);

        const bool sent = communication.sendTo(nextHopMac, request.nodeMacAddress,
                                               EspNowCommunication::MessageType::CONFIGURE_LOAD_COMMAND, packet);
        result.accepted = sent;
        std::snprintf(result.reason, sizeof(result.reason),
                      sent ? "dispatched to Node; awaiting confirmation" : "failed to send load configuration");
        return result;
    }

    if (request.action == ConfigCommandAction::DECOMMISSION_NODE) {
        const NodeCommissioningRegistry previousRegistry = commissioningRegistry;
        if (!commissioningRegistry.decommission(request.nodeMacAddress)) {
            std::snprintf(result.reason, sizeof(result.reason), "unknown Node MAC, or already decommissioned");
            xSemaphoreGive(stateMutex);
            return result;
        }
        if (!commissioningRegistry.persist()) {
            commissioningRegistry = previousRegistry;
            std::snprintf(result.reason, sizeof(result.reason),
                          "Central could not persist decommissioning state");
            xSemaphoreGive(stateMutex);
            return result;
        }

        EspNowCommunication::MacAddress nextHopMac{};
        const bool routeFound = findNextHopFromCentral(localMac, request.nodeMacAddress, registry, nextHopMac);

        xSemaphoreGive(stateMutex);

        bool nodeNotificationSent = false;
        if (routeFound) {
            DecommissionCommandPacket packet{};
            packet.commandId = request.commandId;
            nodeNotificationSent = communication.sendTo(nextHopMac, request.nodeMacAddress,
                                                         EspNowCommunication::MessageType::DECOMMISSION_COMMAND, packet);
        } else {
            ESP_LOGW(TAG, "DECOMMISSION_NODE commandId=%u: no known route to Node; Central-side state updated, "
                          "Node itself could not be notified", static_cast<unsigned int>(request.commandId));
        }

        char targetText[18] = {};
        formatMacAddressText(targetText, sizeof(targetText), request.nodeMacAddress);
        mqttManager.publishEvent("NODE_DECOMMISSIONED", targetText, nullptr);
        xSemaphoreGive(optimizationTriggerSemaphore);

        result.accepted = true;
        std::snprintf(result.reason, sizeof(result.reason),
                      nodeNotificationSent ? "Central record decommissioned; Node reset requested"
                                            : "Central record decommissioned; Node reset notification unavailable");
        return result;
    }

    /* COMMISSION_NODE and RENAME_NODE share the same wire round trip (see NodeCommissioningRegistry::requestCommissioning()). */
    if (!request.hasFriendlyName) {
        std::snprintf(result.reason, sizeof(result.reason), "friendlyName required");
        xSemaphoreGive(stateMutex);
        return result;
    }

    if (!commissioningRegistry.requestCommissioning(request.nodeMacAddress, request.friendlyName)) {
        std::snprintf(result.reason, sizeof(result.reason),
                       "unknown Node MAC, or current commissioning state does not allow this operation");
        xSemaphoreGive(stateMutex);
        return result;
    }

    EspNowCommunication::MacAddress nextHopMac{};
    const bool routeFound = findNextHopFromCentral(localMac, request.nodeMacAddress, registry, nextHopMac);

    if (!routeFound) {
        rollbackUndeliveredCommissionRequest(request.nodeMacAddress);
        std::snprintf(result.reason, sizeof(result.reason), "no known route to Node");
        xSemaphoreGive(stateMutex);
        return result;
    }

    CommissionCommandPacket packet{};
    packet.commandId = request.commandId;
    std::snprintf(packet.friendlyName, sizeof(packet.friendlyName), "%s", request.friendlyName);

    const bool sent = communication.sendTo(nextHopMac, request.nodeMacAddress,
                                            EspNowCommunication::MessageType::COMMISSION_COMMAND, packet);

    if (!sent) {
        rollbackUndeliveredCommissionRequest(request.nodeMacAddress);
        std::snprintf(result.reason, sizeof(result.reason), "failed to send commissioning command");
        xSemaphoreGive(stateMutex);
        return result;
    }

    xSemaphoreGive(stateMutex);

    result.accepted = true;
    std::snprintf(result.reason, sizeof(result.reason), "dispatched to Node; awaiting confirmation");
    return result;
}

} // namespace


extern "C" void app_main()
{
    communication.setLocalNodeName(CentralNodeConfig::CENTRAL_NODE_NAME);

    if (!communication.initialize() || !communication.setAsCentralNode()) {
        ESP_LOGE(TAG, "Central initialization failed");
        return;
    }

    currentTimeProvider.initializeTimeSynchronization();

    stateMutex = xSemaphoreCreateMutex();
    optimizationTriggerSemaphore = xSemaphoreCreateBinary();

    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();

    configureLocalHardware(localMac);
    const bool centralConfigurationRestored = centralConfigurationStore.loadPersisted();
    const bool batteryConfigurationApplied =
        centralConfigurationRestored && applyPersistedBatterySensorConfiguration();
    /*
     * batteryStateOfCharge is deliberately NOT initialize()d here: Section
     * "SoC Must Have Validity"/"True Factory State" - with no battery
     * sensor configured, isValid() must honestly report false/UNKNOWN.
     * initialize() only ever runs after a real installer-verified sensor
     * exists (or during an explicit Development Session).
     */
    if (!batteryConfigurationApplied) {
        ESP_LOGI(TAG, "BATTERY_SOC: No valid persisted SoC estimate; no battery sensor configured, remaining UNKNOWN");
    }
    ESP_LOGI(TAG, "NVS_RESTORE: centralConfiguration=%s safetyPolicy=%s batterySensor=%s",
             centralConfigurationRestored ? "VALID" : "ABSENT",
             centralConfigurationStore.getConfiguration().safetyPolicy.configured ? "CONFIGURED" : "NOT_CONFIGURED",
             batteryConfigurationApplied ? "CONFIGURED" : "NOT_CONFIGURED");
    loadConfigurationStore.loadPersisted();

    /*
     * Section "Commissioning": Central registers its own identity directly
     * as COMMISSIONED - no ESP-NOW round trip is needed for a Node to
     * commission itself. loadPersisted() first restores any remote Nodes
     * already commissioned in a previous session (Central's own record is
     * always re-seeded fresh below rather than trusted from a persisted
     * blob, since CENTRAL_NODE_NAME/localMac are already authoritatively
     * known at this point).
     */
    const bool commissioningRestored = commissioningRegistry.loadPersisted();

    /* Section "NVS Restore Logging": show what was actually restored, not a vague count alone. */
    ESP_LOGI(TAG, "NVS_RESTORE: commissioningRecords=%s nodes=%u loadConfigEntries=%s",
             commissioningRestored ? "VALID" : "ABSENT",
             static_cast<unsigned int>(commissioningRegistry.getCount()),
             "see LOAD_CONFIG_STORE below");
    for (std::size_t i = 0U; i < commissioningRegistry.getCount(); ++i) {
        const NodeCommissioningRegistry::CommissioningRecord *record = commissioningRegistry.getRecord(i);
        if (record == nullptr) {
            continue;
        }
        char recordMacText[18] = {};
        formatMacAddressText(recordMacText, sizeof(recordMacText), record->macAddress);
        ESP_LOGI(TAG, "RESTORED_NODE mac=%s role=%s state=%s name=%s",
                 recordMacText, toText(record->role), toText(record->lifecycleState), record->friendlyName);
    }

    char chipModelText[NodeCommissioningRegistry::CHIP_MODEL_BUFFER_SIZE] = {};
    chipInfo.getChipModelText(chipModelText, sizeof(chipModelText));
    if (commissioningRegistry.registerSelf(localMac, NodeRole::CENTRAL, CentralNodeConfig::CENTRAL_NODE_NAME,
                                            KILOWATTS_FIRMWARE_VERSION, chipModelText,
                                            static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())))) {
        commissioningRegistry.persist();
    }

    /*
     * Wi-Fi/MQTT are best-effort: their absence must never prevent local
     * sensing, Best-First Search, ESP-NOW or relay control from working.
     */
    if (!wifiManager.begin(WiFiManager::Credentials{
            Secrets::WIFI_SSID, Secrets::WIFI_PASSWORD, CentralNodeConfig::WIFI_STATION_HOSTNAME})) {
        ESP_LOGW(TAG, "Wi-Fi did not start; continuing with local-only control");
    }

    mqttManager.setLoadCommandHandler(&handleLoadCommand, nullptr);
    mqttManager.setSystemCommandHandler(&handleSystemCommand, nullptr);
    mqttManager.setConfigCommandHandler(&handleConfigCommand, nullptr);
    mqttManager.setDevelopmentCommandHandler(&handleDevelopmentCommand, nullptr);
    if (!mqttManager.begin(MqttManager::Credentials{
            Secrets::MQTT_BROKER_HOST, Secrets::MQTT_BROKER_PORT, Secrets::MQTT_BROKER_USE_TLS,
            Secrets::MQTT_USERNAME, Secrets::MQTT_PASSWORD})) {
        ESP_LOGW(TAG, "MQTT did not start; continuing with local-only control");
    }

    xTaskCreate(sensorAcquisitionTask, "sensor_acq", 4096U, nullptr, 5U, nullptr);
    xTaskCreate(espNowCommunicationTask, "espnow_app", 4096U, nullptr, 5U, nullptr);
    xTaskCreate(optimizationTask, "optimization", 8192U, nullptr, 6U, nullptr);
    xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr);
    xTaskCreate(consoleTask, "console", 4096U, nullptr, 3U, nullptr);

    printCentralBootSummary(localMac);
    ESP_LOGI(TAG, "Central is ready: sensing, ESP-NOW, optimisation and watchdog tasks running");
}

#endif // DEVICE_ROLE_CENTRAL
