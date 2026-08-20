#ifndef KILOWATTS_CENTRAL_RUNTIME_H
#define KILOWATTS_CENTRAL_RUNTIME_H

#include "BestFirstSearch.h"
#include "CentralConfigurationStore.h"
#include "CentralNodeConfig.h"
#include "CentralNodeRegistry.h"
#include "ChipInfo.h"
#include "CommissioningPackets.h"
#include "CurrentTimeProvider.h"
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
#include "NodeLoadHardwareStore.h"
#include "NodeRegistryJson.h"
#include "NodeReportPackets.h"
#include "PowerManager.h"
#include "RadioConfig.h"
#include "RelayCommandDispatcher.h"
#include "RelayController.h"
#include "SystemStateJson.h"
#include "TopologyTree.h"
#include "WiFiCredentialsStore.h"
#include "WiFiManager.h"
#include "WiFiProvisioningPortal.h"

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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

using namespace kilowatts;

static const char* TAG = "CENTRAL";

namespace {

EspNowCommunication communication(kilowatts::KILOWATTS_RADIO_CHANNEL);
CurrentTimeProvider currentTimeProvider;
WiFiManager wifiManager(kilowatts::KILOWATTS_RADIO_CHANNEL);
WiFiCredentialsStore wifiCredentialsStore;
WiFiProvisioningPortal wifiProvisioningPortal;
MqttManager mqttManager(
    CentralNodeConfig::MQTT_TOPIC_NAMESPACE,
    CentralNodeConfig::MQTT_DEVICE_ID,
    CentralNodeConfig::MQTT_SCHEMA_VERSION);

INA219Monitor sensors;
RelayController relays;
BatteryStateOfCharge batteryStateOfCharge;
CentralConfigurationStore centralConfigurationStore;
LoadConfigurationStore loadConfigurationStore;
NodeLoadHardwareStore centralLoadHardwareStore;
CentralNodeRegistry registry;
NodeCommissioningRegistry commissioningRegistry;
RelayCommandDispatcher relayCommandDispatcher;
LoadScheduleEvaluator scheduleEvaluator;
ChipInfo chipInfo;
Node centralNode(EspNowCommunication::MacAddress{});

SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t optimizationTriggerSemaphore = nullptr;

bool batterySensorConfigured = false;
LoadMeasurements latestBatteryMeasurements{
    CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS,
    0.0F,
    0.0F};
bool batteryReadingValid = false;
std::uint32_t batteryReadingMilliseconds = 0U;
std::int64_t lastOptimizationEpochSeconds = 0;
std::uint32_t pinCommandErrorCount = 0U;
bool mqttStarted = false;

struct BatterySummary {
    float nominalVoltageVolts = 0.0F;
    float capacityAmpHours = 0.0F;
    float ratedEnergyWattHours = 0.0F;
    float storedEnergyWattHours = 0.0F;
    float usableEnergyWattHours = 0.0F;
    float targetRuntimeHours = 0.0F;
    float estimatedRuntimeHours = 0.0F;
    bool estimatedRuntimeValid = false;
    float runtimePowerLimitWatts = 0.0F;
    float maximumBatteryCurrentAmps = 0.0F;
    float maximumBatteryPowerWatts = 0.0F;
    float maximumMainCurrentAmps = 0.0F;
    float maximumMainPowerWatts = 0.0F;
    float safetyCeilingWatts = 0.0F;
};

struct PowerFlowSummary {
    float measuredSourcePowerWatts = 0.0F;
    float powerBeforeFixedLoadsWatts = 0.0F;
    float fixedOnLoadPowerWatts = 0.0F;
    float powerPassedToBestFirstWatts = 0.0F;
    float selectedAutoLoadPowerWatts = 0.0F;
    float finalRemainingPowerWatts = 0.0F;
};

struct PendingHardwareCommand {
    std::uint32_t commandId;
    Load::MacAddress nodeMacAddress;
    std::uint8_t relayPin;
    bool remove;
};

std::vector<PendingHardwareCommand> pendingHardwareCommands;
std::vector<Load::MacAddress> offlineNodeMacAddresses;

void formatMacAddressText(
    char* buffer,
    std::size_t bufferSize,
    const Load::MacAddress& mac)
{
    std::snprintf(buffer, bufferSize, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char* wifiStateText(WiFiConnectionState state)
{
    switch (state) {
        case WiFiConnectionState::DISCONNECTED: return "DISCONNECTED";
        case WiFiConnectionState::SCANNING: return "SCANNING";
        case WiFiConnectionState::CONNECTING: return "CONNECTING";
        case WiFiConnectionState::CONNECTED_AWAITING_IP: return "CONNECTED_AWAITING_IP";
        case WiFiConnectionState::CONNECTED_WITH_IP: return "CONNECTED_WITH_IP";
        case WiFiConnectionState::RADIO_CHANNEL_MISMATCH: return "RADIO_CHANNEL_MISMATCH";
    }
    return "UNKNOWN";
}

const char* hardwareConfigurationFailureText(HardwareConfigurationFailureReason reason)
{
    switch (reason) {
        case HardwareConfigurationFailureReason::NONE: return "applied";
        case HardwareConfigurationFailureReason::NODE_NOT_COMMISSIONED: return "Node is not commissioned";
        case HardwareConfigurationFailureReason::UNSUPPORTED_RELAY_PIN: return "GPIO pin is not supported";
        case HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN: return "GPIO pin is already in use";
        case HardwareConfigurationFailureReason::INVALID_ELECTRICAL_RATING: return "invalid load power rating";
        case HardwareConfigurationFailureReason::INVALID_CONFIGURATION: return "invalid load configuration";
        case HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED: return "GPIO pin could not be configured";
        case HardwareConfigurationFailureReason::PERSISTENCE_FAILED: return "configuration could not be saved";
        case HardwareConfigurationFailureReason::CAPACITY_REACHED: return "Node load capacity reached";
    }
    return "unknown error";
}

const char* loadControlText(const Load& load)
{
    return load.isFixed() ? "FIXED" : "AUTO";
}

const char* configuredStateText(const Load& load)
{
    if (load.isFixed()) return load.isOn() ? "ON" : "OFF";
    return load.getTargetRelayState() ? "ON" : "OFF";
}

bool nodeDeclaresRelayPin(
    const NodeCommissioningRegistry::CommissioningRecord& record,
    std::uint8_t relayPin)
{
    for (std::size_t i = 0U; i < record.relayCapabilityCount; ++i) {
        if (record.relayPins[i] == relayPin) return true;
    }
    return false;
}

CentralConfigurationStore::SafetyPolicy effectiveSafetyPolicy()
{
    const auto configured = centralConfigurationStore.getConfiguration().safetyPolicy;
    if (configured.configured) return configured;

    return CentralConfigurationStore::SafetyPolicy{
        false,
        CentralNodeConfig::MINIMUM_STATE_OF_CHARGE_PERCENT,
        CentralNodeConfig::WARNING_STATE_OF_CHARGE_PERCENT,
        CentralNodeConfig::TARGET_RUNTIME_HOURS,
        CentralNodeConfig::SAFETY_FACTOR,
        CentralNodeConfig::MAXIMUM_BATTERY_DISCHARGE_CURRENT_AMPS,
        CentralNodeConfig::MAXIMUM_MAIN_CURRENT_AMPS};
}

float configuredNominalBatteryVoltageVolts()
{
    const auto& battery = centralConfigurationStore.getConfiguration().batterySensor;
    return battery.configured
        ? battery.nominalVoltageVolts
        : CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS;
}

float configuredBatteryCapacityAmpHours()
{
    const auto& battery = centralConfigurationStore.getConfiguration().batterySensor;
    return battery.configured
        ? battery.batteryCapacityAmpHours
        : CentralNodeConfig::BATTERY_CAPACITY_AMP_HOURS;
}

SafePowerLimitCalculator::Inputs makeSafePowerInputs(
    float stateOfChargePercent,
    float batteryVoltageVolts,
    const CentralConfigurationStore::SafetyPolicy& policy)
{
    SafePowerLimitCalculator::Inputs input{};
    input.stateOfChargePercent = stateOfChargePercent;
    input.minimumStateOfChargePercent = policy.minimumStateOfChargePercent;
    input.nominalBatteryVoltageVolts = configuredNominalBatteryVoltageVolts();
    input.batteryCapacityAmpHours = configuredBatteryCapacityAmpHours();
    input.targetRuntimeHours = policy.targetRuntimeHours;
    input.batteryBusVoltageVolts = batteryVoltageVolts;
    input.maximumBatteryDischargeCurrentAmps = policy.maximumBatteryDischargeCurrentAmps;
    input.maximumMainCurrentAmps = policy.maximumMainCurrentAmps;
    input.safetyFactor = policy.safetyFactor;
    return input;
}

bool batteryTelemetryIsFresh(std::uint32_t nowMilliseconds)
{
    return batteryReadingValid &&
           (nowMilliseconds - batteryReadingMilliseconds) <=
               CentralNodeConfig::BATTERY_TELEMETRY_STALE_TIMEOUT_MILLISECONDS;
}

bool isNodeOnline(const CentralNodeRegistry::PlanningNode& node, std::uint32_t nowMilliseconds)
{
    if (node.isCentralNode) return true;
    return (nowMilliseconds - node.lastSeenMilliseconds) <=
           CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS;
}

bool isLoadAvailableForPlanning(const Load& load, std::uint32_t nowMilliseconds)
{
    const auto* node = registry.findNodeByMacAddress(load.getMacAddress());
    return node != nullptr && isNodeOnline(*node, nowMilliseconds);
}

bool isMacMarkedOffline(const Load::MacAddress& mac)
{
    return std::find(offlineNodeMacAddresses.begin(), offlineNodeMacAddresses.end(), mac) !=
           offlineNodeMacAddresses.end();
}

void updateOfflineTransitions(
    std::uint32_t nowMilliseconds,
    std::vector<std::pair<Load::MacAddress, std::string>>& newlyOffline,
    std::vector<std::pair<Load::MacAddress, std::string>>& recovered)
{
    for (std::size_t i = 0U; i < registry.getNumberOfNodes(); ++i) {
        const auto* node = registry.getNode(i);
        if (node == nullptr || node->isCentralNode) continue;

        const bool online = isNodeOnline(*node, nowMilliseconds);
        const bool wasOffline = isMacMarkedOffline(node->node.getMacAddress());

        if (!online && !wasOffline) {
            offlineNodeMacAddresses.push_back(node->node.getMacAddress());
            newlyOffline.push_back({node->node.getMacAddress(), node->nodeName});
        } else if (online && wasOffline) {
            offlineNodeMacAddresses.erase(
                std::remove(offlineNodeMacAddresses.begin(), offlineNodeMacAddresses.end(), node->node.getMacAddress()),
                offlineNodeMacAddresses.end());
            recovered.push_back({node->node.getMacAddress(), node->nodeName});
        }
    }
}

bool findNextHopFromCentral(
    const Load::MacAddress& centralMac,
    const Load::MacAddress& destinationMac,
    Load::MacAddress& nextHop)
{
    const auto* current = registry.findNodeByMacAddress(destinationMac);
    if (current == nullptr) return false;

    for (std::size_t depth = 0U; depth <= registry.getNumberOfNodes(); ++depth) {
        if (current->nextHopToCentralMacAddress == centralMac) {
            nextHop = current->node.getMacAddress();
            return true;
        }
        current = registry.findNodeByMacAddress(current->nextHopToCentralMacAddress);
        if (current == nullptr) return false;
    }
    return false;
}

void configureLocalHardware(const Load::MacAddress& localMac)
{
    sensors.initializeBus(CentralNodeConfig::i2cBusConfiguration());
    centralNode = Node(localMac);

    if (centralLoadHardwareStore.loadPersisted()) {
        bool pinsValid = true;
        for (std::size_t i = 0U; i < centralLoadHardwareStore.getNumberOfConfigurations(); ++i) {
            const auto* configuration = centralLoadHardwareStore.getConfiguration(i);
            if (configuration != nullptr && !CentralNodeConfig::isVerifiedRelayPin(configuration->relayPin)) {
                pinsValid = false;
                break;
            }
        }

        if (pinsValid) {
            HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
            centralLoadHardwareStore.applyPersistedConfigurations(relays, centralNode, reason);
        }
    }

    registry.addLocalCentralNode(CentralNodeConfig::CENTRAL_NODE_NAME, centralNode, 0U);
}

bool applyPersistedBatterySensorConfiguration()
{
    const auto& battery = centralConfigurationStore.getConfiguration().batterySensor;
    if (!battery.configured) return false;

    if (!sensors.configureSensor(INA219Monitor::SensorConfiguration{
            battery.shuntResistanceOhms,
            battery.maximumExpectedCurrentAmps,
            battery.emaAlpha}) ||
        !sensors.isSensorPresent()) {
        return false;
    }

    if (!batteryStateOfCharge.initialize(
            battery.batteryCapacityAmpHours,
            battery.initialStateOfChargePercent)) {
        return false;
    }

    batterySensorConfigured = true;
    return true;
}

void sensorAcquisitionTask(void* parameter)
{
    (void)parameter;
    TickType_t lastTick = xTaskGetTickCount();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CentralNodeConfig::SENSOR_ACQUISITION_PERIOD_MILLISECONDS));

        const TickType_t nowTick = xTaskGetTickCount();
        const float seconds = static_cast<float>(nowTick - lastTick) * portTICK_PERIOD_MS / 1000.0F;
        lastTick = nowTick;

        LoadMeasurements measurement{};
        const bool ok = batterySensorConfigured && sensors.readFilteredMeasurements(measurement);

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            if (ok) {
                latestBatteryMeasurements = measurement;
                batteryReadingValid = true;
                batteryReadingMilliseconds = static_cast<std::uint32_t>(pdTICKS_TO_MS(nowTick));
                batteryStateOfCharge.update(measurement.currentAmps, seconds);
            }
            xSemaphoreGive(stateMutex);
        }
    }
}

void applyStoredLoadSettings()
{
    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const auto* branch = registry.getNode(nodeIndex);
        if (branch == nullptr) continue;

        for (std::size_t loadIndex = 0U; loadIndex < branch->node.getNumberOfLoads(); ++loadIndex) {
            const Load* load = branch->node.getLoad(loadIndex);
            if (load == nullptr) continue;

            Load* mutableLoad = registry.findMutableLoad(load->getMacAddress(), load->getRelayPin());
            if (mutableLoad != nullptr) loadConfigurationStore.applyToLoad(*mutableLoad);
        }
    }
}

float applyFixedLoadStates(LoadFilter& filter)
{
    for (std::size_t i = 0U; i < filter.getNumberOfFixedOffLoads(); ++i) {
        const Load* fixed = filter.getFixedOffLoad(i);
        if (fixed == nullptr) continue;
        Load* load = registry.findMutableLoad(fixed->getMacAddress(), fixed->getRelayPin());
        if (load != nullptr) load->setTargetRelayState(false);
    }

    float fixedOnLoadPowerWatts = 0.0F;
    for (std::size_t i = 0U; i < filter.getNumberOfFixedOnLoads(); ++i) {
        const Load* fixed = filter.getFixedOnLoad(i);
        if (fixed == nullptr) continue;
        Load* load = registry.findMutableLoad(fixed->getMacAddress(), fixed->getRelayPin());
        if (load == nullptr) continue;

        load->setTargetRelayState(true);
        fixedOnLoadPowerWatts += load->getPower().runningWatts;
    }
    return fixedOnLoadPowerWatts;
}

bool dispatchRelayTargetAndWait(
    const RelayCommandDispatcher::RelayTarget& target,
    const Load::MacAddress& localMac)
{
    const std::uint32_t nowMilliseconds =
        static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));

    if (target.nodeMacAddress == localMac) {
        const bool writeOk = relays.setRelayState(target.relayPin, target.desiredOn);
        bool pinOn = false;
        const bool readOk = relays.readBackState(target.relayPin, pinOn);
        const bool success = writeOk && readOk && pinOn == target.desiredOn;

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            Load* load = registry.findMutableLoad(target.nodeMacAddress, target.relayPin);
            if (load != nullptr && success) load->setConfirmedRelayState(pinOn);
            if (!success) ++pinCommandErrorCount;
            xSemaphoreGive(stateMutex);
        }
        return success;
    }

    Load::MacAddress nextHop{};
    std::uint32_t commandId = 0U;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) return false;

    if (!findNextHopFromCentral(localMac, target.nodeMacAddress, nextHop)) {
        xSemaphoreGive(stateMutex);
        return false;
    }

    commandId = relayCommandDispatcher.beginCommand(target, nowMilliseconds);
    xSemaphoreGive(stateMutex);

    RelayCommandPacket packet{};
    packet.relayPin = target.relayPin;
    packet.desiredState = static_cast<std::uint8_t>(
        target.desiredOn ? RelayCommandState::ON : RelayCommandState::OFF);
    packet.commandId = commandId;

    if (!communication.sendTo(
            nextHop,
            target.nodeMacAddress,
            EspNowCommunication::MessageType::RELAY_COMMAND,
            packet,
            500U)) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            relayCommandDispatcher.completeCommand(commandId);
            ++pinCommandErrorCount;
            xSemaphoreGive(stateMutex);
        }
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(CentralNodeConfig::RELAY_COMMAND_ACK_TIMEOUT_MILLISECONDS);

    while (xTaskGetTickCount() < deadline) {
        bool pending = true;
        bool success = false;

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            pending = relayCommandDispatcher.isPending(commandId);
            if (!pending) {
                const Load* load = registry.findMutableLoad(target.nodeMacAddress, target.relayPin);
                success = load != nullptr &&
                          load->isConfirmedRelayStateValid() &&
                          load->getConfirmedRelayState() == target.desiredOn;
            }
            xSemaphoreGive(stateMutex);
        }

        if (!pending) return success;
        vTaskDelay(pdMS_TO_TICKS(50U));
    }

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
        relayCommandDispatcher.completeCommand(commandId);
        ++pinCommandErrorCount;
        xSemaphoreGive(stateMutex);
    }
    return false;
}

void printOptimizationCycle(
    const BatterySummary& battery,
    const PowerFlowSummary& power,
    const LoadFilter& filter,
    const std::vector<Load::Id>& selectedAutoLoadIds,
    std::uint32_t nowMilliseconds)
{
    std::size_t onlineNodeCount = 0U;
    for (std::size_t i = 0U; i < registry.getNumberOfNodes(); ++i) {
        const auto* node = registry.getNode(i);
        if (node != nullptr && isNodeOnline(*node, nowMilliseconds)) ++onlineNodeCount;
    }

    std::printf("\n================ KILOWATTS ================\n");
    std::printf("NODES\n");
    std::printf("Total Nodes              : %u\n", static_cast<unsigned int>(registry.getNumberOfNodes()));
    std::printf("Online Nodes             : %u\n", static_cast<unsigned int>(onlineNodeCount));

    for (std::size_t i = 0U; i < registry.getNumberOfNodes(); ++i) {
        const auto* node = registry.getNode(i);
        if (node == nullptr) continue;

        char mac[18]{};
        formatMacAddressText(mac, sizeof(mac), node->node.getMacAddress());
        std::printf("  %s | %s | %s | %s | Loads: %u\n",
                    node->isCentralNode ? "CENTRAL" : "SMART",
                    node->nodeName.c_str(),
                    mac,
                    isNodeOnline(*node, nowMilliseconds) ? "ONLINE" : "OFFLINE",
                    static_cast<unsigned int>(node->node.getNumberOfLoads()));

        for (std::size_t j = 0U; j < node->node.getNumberOfLoads(); ++j) {
            const Load* load = node->node.getLoad(j);
            if (load == nullptr) continue;
            const char* pinState = load->isConfirmedRelayStateValid()
                ? (load->getConfirmedRelayState() ? "ON" : "OFF")
                : "UNKNOWN";
            std::printf("    %s | Pin %u | %s | %s | Pin: %s | %.3f W\n",
                        load->getName().c_str(),
                        static_cast<unsigned int>(load->getRelayPin()),
                        loadControlText(*load),
                        configuredStateText(*load),
                        pinState,
                        static_cast<double>(load->getPower().runningWatts));
        }
    }

    std::printf("\nSOURCE READING\n");
    std::printf("Voltage                  : %.2f V\n", static_cast<double>(latestBatteryMeasurements.voltageVolts));
    std::printf("Current                  : %.2f A\n", static_cast<double>(latestBatteryMeasurements.currentAmps));
    std::printf("Measured Source Power    : %.3f W\n", static_cast<double>(power.measuredSourcePowerWatts));
    std::printf("Measurement Source       : %s\n", toText(sensors.getLastMeasurementSource()));

    std::printf("\nBATTERY\n");
    std::printf("Nominal Voltage          : %.2f V\n", static_cast<double>(battery.nominalVoltageVolts));
    std::printf("Capacity                 : %.2f Ah\n", static_cast<double>(battery.capacityAmpHours));
    std::printf("Rated Energy             : %.2f Wh\n", static_cast<double>(battery.ratedEnergyWattHours));
    std::printf("State of Charge          : %.1f %%\n", static_cast<double>(batteryStateOfCharge.getStateOfChargePercent()));
    std::printf("Stored Energy            : %.2f Wh\n", static_cast<double>(battery.storedEnergyWattHours));
    std::printf("Usable Energy            : %.2f Wh\n", static_cast<double>(battery.usableEnergyWattHours));
    std::printf("Target Runtime           : %.2f h\n", static_cast<double>(battery.targetRuntimeHours));
    if (battery.estimatedRuntimeValid) {
        std::printf("Estimated Runtime        : %.2f h\n", static_cast<double>(battery.estimatedRuntimeHours));
    } else {
        std::printf("Estimated Runtime        : N/A\n");
    }
    std::printf("Runtime Power Limit      : %.2f W\n", static_cast<double>(battery.runtimePowerLimitWatts));
    std::printf("Battery Current Limit    : %.2f A (%.2f W)\n",
                static_cast<double>(battery.maximumBatteryCurrentAmps),
                static_cast<double>(battery.maximumBatteryPowerWatts));
    std::printf("Main Current Limit       : %.2f A (%.2f W)\n",
                static_cast<double>(battery.maximumMainCurrentAmps),
                static_cast<double>(battery.maximumMainPowerWatts));
    std::printf("Safety Ceiling           : %.2f W\n", static_cast<double>(battery.safetyCeilingWatts));

    std::printf("\nLOADS\n");
    std::printf("Fixed ON Loads           : %u\n", static_cast<unsigned int>(filter.getNumberOfFixedOnLoads()));
    std::printf("Fixed OFF Loads          : %u\n", static_cast<unsigned int>(filter.getNumberOfFixedOffLoads()));
    std::printf("Auto Loads               : %u\n", static_cast<unsigned int>(filter.getNumberOfAutoCandidateLoads()));
    std::printf("Fixed ON Power           : %.3f W\n", static_cast<double>(power.fixedOnLoadPowerWatts));

    std::printf("\nBEST-FIRST SEARCH\n");
    std::printf("Power before Fixed Loads : %.3f W\n", static_cast<double>(power.powerBeforeFixedLoadsWatts));
    std::printf("Fixed ON Power           : %.3f W\n", static_cast<double>(power.fixedOnLoadPowerWatts));
    std::printf("Power passed to BFS      : %.3f W\n", static_cast<double>(power.powerPassedToBestFirstWatts));
    std::printf("Selected Auto Loads:\n");

    if (selectedAutoLoadIds.empty()) {
        std::printf("  None\n");
    } else {
        for (const Load::Id& id : selectedAutoLoadIds) {
            const Load* load = registry.findMutableLoad(id.macAddress, id.relayPin);
            if (load == nullptr) continue;
            const auto* node = registry.findNodeByMacAddress(id.macAddress);
            std::printf("  %s | %s | Pin %u | %.3f W\n",
                        node != nullptr ? node->nodeName.c_str() : "Node",
                        load->getName().c_str(),
                        static_cast<unsigned int>(load->getRelayPin()),
                        static_cast<double>(load->getPower().runningWatts));
        }
    }

    std::printf("Selected Auto Load Power : %.3f W\n", static_cast<double>(power.selectedAutoLoadPowerWatts));
    std::printf("Final Remaining Power    : %.3f W\n", static_cast<double>(power.finalRemainingPowerWatts));
    std::printf("===========================================\n");
}

void publishCurrentState(
    const BatterySummary& battery,
    const PowerFlowSummary& power,
    bool batteryStateKnown,
    std::uint32_t nowMilliseconds)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) return;

    SystemStateInputs state{};
    state.batterySensorConfigured = batterySensorConfigured;
    state.batteryNominalVoltageVolts = battery.nominalVoltageVolts;
    state.batteryCapacityAmpHours = battery.capacityAmpHours;
    state.batteryRatedEnergyWattHours = battery.ratedEnergyWattHours;
    state.batteryStoredEnergyWattHours = battery.storedEnergyWattHours;
    state.batteryUsableEnergyWattHours = battery.usableEnergyWattHours;
    state.batteryVoltageVolts = batterySensorConfigured ? latestBatteryMeasurements.voltageVolts : 0.0F;
    state.batteryCurrentAmps = batterySensorConfigured ? latestBatteryMeasurements.currentAmps : 0.0F;
    state.measuredSourcePowerWatts = power.measuredSourcePowerWatts;
    state.batteryMeasurementSourceText = toText(sensors.getLastMeasurementSource());
    state.stateOfChargePercent = batteryStateOfCharge.getStateOfChargePercent();
    state.stateOfChargeValid = batteryStateOfCharge.isValid();
    state.stateOfChargeSourceText = toText(batteryStateOfCharge.getSource());
    state.targetRuntimeHours = battery.targetRuntimeHours;
    state.estimatedRuntimeHours = battery.estimatedRuntimeHours;
    state.runtimeEstimateValid = battery.estimatedRuntimeValid;
    state.runtimePowerLimitWatts = battery.runtimePowerLimitWatts;
    state.maximumBatteryCurrentAmps = battery.maximumBatteryCurrentAmps;
    state.maximumBatteryPowerWatts = battery.maximumBatteryPowerWatts;
    state.maximumMainCurrentAmps = battery.maximumMainCurrentAmps;
    state.maximumMainPowerWatts = battery.maximumMainPowerWatts;
    state.safetyCeilingWatts = battery.safetyCeilingWatts;
    state.powerBeforeFixedLoadsWatts = power.powerBeforeFixedLoadsWatts;
    state.fixedOnLoadPowerWatts = power.fixedOnLoadPowerWatts;
    state.powerPassedToBestFirstWatts = power.powerPassedToBestFirstWatts;
    state.selectedAutoLoadPowerWatts = power.selectedAutoLoadPowerWatts;
    state.finalRemainingPowerWatts = power.finalRemainingPowerWatts;
    state.wifiConnected = wifiManager.isConnected();
    state.wifiStateText = wifiStateText(wifiManager.getState());
    state.mqttConnected = mqttManager.isConnected();
    state.currentTimeValid = currentTimeProvider.isCurrentTimeValid();
    state.currentTimeSourceText =
        currentTimeProvider.getCurrentTimeSource() == TimeSource::NTP ? "NTP" :
        (currentTimeProvider.getCurrentTimeSource() == TimeSource::MANUAL ? "MANUAL" : "NONE");
    state.lastOptimizationEpochSeconds = lastOptimizationEpochSeconds;
    state.pinCommandErrorCount = pinCommandErrorCount;

    const std::string systemJson = SystemStateJson::build(
        state, CentralNodeConfig::MQTT_SCHEMA_VERSION);
    const std::string treeJson = TopologyTree::buildTreeJson(
        registry,
        commissioningRegistry,
        CentralNodeConfig::MQTT_SCHEMA_VERSION,
        nowMilliseconds,
        CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS);
    const std::string loadsJson = TopologyTree::buildLoadsJson(
        registry, CentralNodeConfig::MQTT_SCHEMA_VERSION);
    const std::string nodesJson = NodeRegistryJson::buildStateNodesJson(
        commissioningRegistry,
        registry,
        CentralNodeConfig::MQTT_SCHEMA_VERSION,
        nowMilliseconds,
        CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS);
    const std::string configNodesJson = NodeRegistryJson::buildConfigNodesJson(
        commissioningRegistry,
        registry,
        CentralNodeConfig::MQTT_SCHEMA_VERSION);

    xSemaphoreGive(stateMutex);

    mqttManager.publish(MqttManager::TOPIC_STATE_SYSTEM, systemJson, 1, true);
    mqttManager.publish(MqttManager::TOPIC_STATE_TREE, treeJson, 1, true);
    mqttManager.publish(MqttManager::TOPIC_STATE_LOADS, loadsJson, 1, true);
    mqttManager.publish(MqttManager::TOPIC_STATE_NODES, nodesJson, 1, true);
    mqttManager.publish(MqttManager::TOPIC_CONFIG_NODES, configNodesJson, 1, true);

    (void)batteryStateKnown;
}

void runOptimizationCycle()
{
    const Load::MacAddress localMac = communication.getLocalMacAddress();
    const std::uint32_t nowMilliseconds =
        static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));

    std::vector<RelayCommandDispatcher::RelayTarget> targets;
    std::vector<std::pair<Load::MacAddress, std::string>> newlyOffline;
    std::vector<std::pair<Load::MacAddress, std::string>> recovered;
    std::vector<Load::Id> selectedAutoLoadIds;

    BatterySummary battery{};
    PowerFlowSummary power{};
    bool batteryStateKnown = false;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(1000U)) != pdTRUE) return;

    updateOfflineTransitions(nowMilliseconds, newlyOffline, recovered);
    applyStoredLoadSettings();

    const auto policy = effectiveSafetyPolicy();
    const bool batteryTelemetryKnown = batterySensorConfigured && batteryTelemetryIsFresh(nowMilliseconds);
    const bool socKnown = batteryStateOfCharge.isValid();
    batteryStateKnown = batteryTelemetryKnown && socKnown;

    battery.nominalVoltageVolts = configuredNominalBatteryVoltageVolts();
    battery.capacityAmpHours = configuredBatteryCapacityAmpHours();
    battery.targetRuntimeHours = policy.targetRuntimeHours;
    battery.maximumBatteryCurrentAmps = policy.maximumBatteryDischargeCurrentAmps;
    battery.maximumMainCurrentAmps = policy.maximumMainCurrentAmps;

    const float batteryVoltageVolts = latestBatteryMeasurements.voltageVolts;
    power.measuredSourcePowerWatts = batteryStateKnown
        ? std::max(0.0F, latestBatteryMeasurements.powerWatts)
        : 0.0F;

    if (batteryStateKnown) {
        SafePowerLimitCalculator calculator;
        if (calculator.calculate(makeSafePowerInputs(
                batteryStateOfCharge.getStateOfChargePercent(),
                batteryVoltageVolts,
                policy))) {
            battery.ratedEnergyWattHours = calculator.getRatedEnergyWattHours();
            battery.usableEnergyWattHours = calculator.getUsableEnergyWattHours();
            battery.runtimePowerLimitWatts = calculator.getRuntimePowerWatts();
            battery.maximumBatteryPowerWatts = calculator.getMaximumBatteryPowerWatts();
            battery.maximumMainPowerWatts = calculator.getMaximumMainPowerWatts();
            battery.safetyCeilingWatts = calculator.getAvailablePowerWatts();
            battery.storedEnergyWattHours =
                battery.ratedEnergyWattHours * batteryStateOfCharge.getStateOfChargePercent() / 100.0F;
            power.powerBeforeFixedLoadsWatts = std::min(
                power.measuredSourcePowerWatts,
                battery.safetyCeilingWatts);
        }
    }

    LoadFilter filter;
    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const auto* branch = registry.getNode(nodeIndex);
        if (branch == nullptr) continue;

        for (std::size_t loadIndex = 0U; loadIndex < branch->node.getNumberOfLoads(); ++loadIndex) {
            const Load* load = branch->node.getLoad(loadIndex);
            if (load != nullptr) filter.addLoad(*load);
        }
    }

    power.fixedOnLoadPowerWatts = applyFixedLoadStates(filter);
    power.powerPassedToBestFirstWatts = std::max(
        0.0F,
        power.powerBeforeFixedLoadsWatts - power.fixedOnLoadPowerWatts);
    power.finalRemainingPowerWatts = power.powerPassedToBestFirstWatts;

    for (std::size_t i = 0U; i < filter.getNumberOfAutoCandidateLoads(); ++i) {
        const Load* autoLoad = filter.getAutoCandidateLoad(i);
        if (autoLoad == nullptr) continue;
        Load* mutableLoad = registry.findMutableLoad(autoLoad->getMacAddress(), autoLoad->getRelayPin());
        if (mutableLoad != nullptr) {
            mutableLoad->setTargetRelayState(false);
            mutableLoad->setLastBestFirstRejectionReason(BestFirstSearch::NONE);
        }
    }

    if (batteryStateKnown && filter.getNumberOfAutoCandidateLoads() > 0U) {
        BestFirstSearch search;
        search.setSearchScoreWeights(CentralNodeConfig::bestFirstSearchWeights());

        BestFirstSearch::ElectricalPlanningState planning{};
        planning.stateOfChargePercent = batteryStateOfCharge.getStateOfChargePercent();
        planning.minimumStateOfChargePercent = policy.minimumStateOfChargePercent;
        planning.warningStateOfChargePercent = policy.warningStateOfChargePercent;
        planning.batteryBusVoltageVolts = batteryVoltageVolts;
        planning.maximumBatteryPowerWatts = battery.maximumBatteryPowerWatts;
        planning.maximumMainCurrentAmps = policy.maximumMainCurrentAmps;
        planning.totalAvailablePowerWatts = power.powerBeforeFixedLoadsWatts;
        planning.initialBestFirstPowerWatts = power.powerPassedToBestFirstWatts;
        planning.powerAlreadyUsedWatts = power.fixedOnLoadPowerWatts;

        if (search.startSearch(planning)) {
            for (std::size_t i = 0U; i < filter.getNumberOfAutoCandidateLoads(); ++i) {
                const Load* load = filter.getAutoCandidateLoad(i);
                if (load == nullptr || !isLoadAvailableForPlanning(*load, nowMilliseconds)) continue;

                LoadScheduleEvaluation schedule{};
                float schedulePenalty = 0.0F;
                if (scheduleEvaluator.evaluateSchedule(*load, currentTimeProvider, schedule)) {
                    schedulePenalty = schedule.futureSchedulePenalty;
                }
                search.addLoad(*load, schedulePenalty);
            }

            if (search.run()) {
                for (std::size_t i = 0U; i < search.getNumberOfLoadsAdded(); ++i) {
                    const Load* load = search.getLoad(i);
                    if (load == nullptr) continue;

                    Load* mutableLoad = registry.findMutableLoad(load->getMacAddress(), load->getRelayPin());
                    if (mutableLoad == nullptr) continue;

                    const bool selected = search.isLoadSelectedToBeOn(i);
                    mutableLoad->setTargetRelayState(selected);
                    mutableLoad->setLastBestFirstRejectionReason(
                        search.getLoadSelectionRejectionReason(i));
                    if (selected) selectedAutoLoadIds.push_back(load->getId());
                }

                power.selectedAutoLoadPowerWatts = search.getSelectedAutoLoadPowerWatts();
                power.finalRemainingPowerWatts = search.getFinalRemainingPowerWatts();
            }
        }
    }

    const float plannedLoadPowerWatts =
        power.fixedOnLoadPowerWatts + power.selectedAutoLoadPowerWatts;
    if (batteryStateKnown && plannedLoadPowerWatts > 0.0F) {
        battery.estimatedRuntimeHours =
            battery.usableEnergyWattHours / plannedLoadPowerWatts;
        battery.estimatedRuntimeValid = std::isfinite(battery.estimatedRuntimeHours);
    }

    printOptimizationCycle(
        battery,
        power,
        filter,
        selectedAutoLoadIds,
        nowMilliseconds);

    for (const Load::Id& id : selectedAutoLoadIds) {
        const Load* load = registry.findMutableLoad(id.macAddress, id.relayPin);
        if (load == nullptr) continue;
        targets.push_back(RelayCommandDispatcher::RelayTarget{
            id.macAddress,
            id.relayPin,
            true,
            load->getConfirmedRelayState(),
            load->isConfirmedRelayStateValid()});
    }

    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const auto* branch = registry.getNode(nodeIndex);
        if (branch == nullptr || !isNodeOnline(*branch, nowMilliseconds)) continue;

        for (std::size_t loadIndex = 0U; loadIndex < branch->node.getNumberOfLoads(); ++loadIndex) {
            const Load* load = branch->node.getLoad(loadIndex);
            if (load == nullptr) continue;

            const bool alreadyAdded = std::find_if(
                selectedAutoLoadIds.begin(),
                selectedAutoLoadIds.end(),
                [load](const Load::Id& id) {
                    return id.macAddress == load->getMacAddress() && id.relayPin == load->getRelayPin();
                }) != selectedAutoLoadIds.end();
            if (alreadyAdded) continue;

            targets.push_back(RelayCommandDispatcher::RelayTarget{
                load->getMacAddress(),
                load->getRelayPin(),
                load->getTargetRelayState(),
                load->getConfirmedRelayState(),
                load->isConfirmedRelayStateValid()});
        }
    }

    batteryStateOfCharge.persist();
    lastOptimizationEpochSeconds = static_cast<std::int64_t>(std::time(nullptr));
    xSemaphoreGive(stateMutex);

    const auto dispatchOrder = RelayCommandDispatcher::buildDispatchOrder(targets);

    float powerAlreadyUsedWatts = power.fixedOnLoadPowerWatts;
    float liveRemainingBestFirstPowerWatts = power.powerPassedToBestFirstWatts;

    for (const auto& target : dispatchOrder) {
        if (!target.desiredOn) {
            dispatchRelayTargetAndWait(target, localMac);
            continue;
        }

        bool selectedAuto = false;
        for (const Load::Id& id : selectedAutoLoadIds) {
            if (id.macAddress == target.nodeMacAddress && id.relayPin == target.relayPin) {
                selectedAuto = true;
                break;
            }
        }

        if (selectedAuto) {
            bool feasible = false;
            float runningWatts = 0.0F;

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300U)) == pdTRUE) {
                Load* load = registry.findMutableLoad(target.nodeMacAddress, target.relayPin);
                const std::uint32_t recheckNow =
                    static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));

                if (load != nullptr && batteryTelemetryIsFresh(recheckNow)) {
                    runningWatts = load->getPower().runningWatts;
                    BestFirstSearch::FeasibilityInputs check{};
                    check.stateOfChargePercent = batteryStateOfCharge.getStateOfChargePercent();
                    check.minimumStateOfChargePercent = policy.minimumStateOfChargePercent;
                    check.candidateRunningPowerWatts = runningWatts;
                    check.candidatePeakPowerWatts = load->getPower().startupWatts;
                    check.remainingPowerWatts = liveRemainingBestFirstPowerWatts;
                    check.powerAlreadyUsedWatts = powerAlreadyUsedWatts;
                    check.maximumBatteryPowerWatts =
                        latestBatteryMeasurements.voltageVolts * policy.maximumBatteryDischargeCurrentAmps;
                    check.batteryBusVoltageVolts = latestBatteryMeasurements.voltageVolts;
                    check.maximumMainCurrentAmps = policy.maximumMainCurrentAmps;

                    const std::uint8_t reason = BestFirstSearch::checkFeasibility(check);
                    feasible = reason == BestFirstSearch::NONE;
                    if (!feasible) {
                        load->setTargetRelayState(false);
                        load->setLastBestFirstRejectionReason(reason);
                    }
                }
                xSemaphoreGive(stateMutex);
            }

            if (!feasible) continue;

            if (dispatchRelayTargetAndWait(target, localMac)) {
                powerAlreadyUsedWatts += runningWatts;
                liveRemainingBestFirstPowerWatts = std::max(
                    0.0F,
                    liveRemainingBestFirstPowerWatts - runningWatts);
            }
        } else {
            dispatchRelayTargetAndWait(target, localMac);
        }
    }

    publishCurrentState(
        battery,
        power,
        batteryStateKnown,
        nowMilliseconds);

    for (const auto& item : newlyOffline) {
        char mac[18]{};
        formatMacAddressText(mac, sizeof(mac), item.first);
        mqttManager.publishEvent("NODE_OFFLINE", mac, item.second.c_str());
    }
    for (const auto& item : recovered) {
        char mac[18]{};
        formatMacAddressText(mac, sizeof(mac), item.first);
        mqttManager.publishEvent("NODE_RECOVERED", mac, item.second.c_str());
    }
}

void checkMqttStartTrigger()
{
    if (mqttStarted || !wifiManager.isConnected()) return;

    mqttStarted = true;
    mqttManager.begin(MqttManager::Credentials{
        Secrets::MQTT_BROKER_HOST,
        Secrets::MQTT_BROKER_PORT,
        Secrets::MQTT_BROKER_USE_TLS,
        Secrets::MQTT_USERNAME,
        Secrets::MQTT_PASSWORD});
}

void optimizationTask(void* parameter)
{
    (void)parameter;
    while (true) {
        xSemaphoreTake(
            optimizationTriggerSemaphore,
            pdMS_TO_TICKS(CentralNodeConfig::OPTIMIZATION_PERIOD_MILLISECONDS));
        checkMqttStartTrigger();
        runOptimizationCycle();
    }
}

PendingHardwareCommand* findPendingHardwareCommand(std::uint32_t commandId)
{
    for (auto& pending : pendingHardwareCommands) {
        if (pending.commandId == commandId) return &pending;
    }
    return nullptr;
}

void removePendingHardwareCommand(std::uint32_t commandId)
{
    pendingHardwareCommands.erase(
        std::remove_if(
            pendingHardwareCommands.begin(),
            pendingHardwareCommands.end(),
            [commandId](const PendingHardwareCommand& item) { return item.commandId == commandId; }),
        pendingHardwareCommands.end());
}

void handleRelayAcknowledgement(
    const Load::MacAddress& originMac,
    const RelayCommandAcknowledgementPacket& ack)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) != pdTRUE) return;

    const auto* pending = relayCommandDispatcher.findPendingCommand(ack.commandId);
    if (pending != nullptr &&
        pending->nodeMacAddress == originMac &&
        pending->relayPin == ack.relayPin) {
        Load* load = registry.findMutableLoad(originMac, ack.relayPin);
        if (load != nullptr && ack.success != 0U) {
            load->setConfirmedRelayState(
                static_cast<RelayCommandState>(ack.confirmedState) == RelayCommandState::ON);
        }
        if (ack.success == 0U) ++pinCommandErrorCount;
        relayCommandDispatcher.completeCommand(ack.commandId);
    }

    xSemaphoreGive(stateMutex);
}

void handleHardwareConfigurationAcknowledgement(
    const Load::MacAddress& originMac,
    const ConfigureLoadAcknowledgementPacket& ack)
{
    bool remove = false;
    bool known = false;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
        PendingHardwareCommand* pending = findPendingHardwareCommand(ack.commandId);
        if (pending != nullptr && pending->nodeMacAddress == originMac && pending->relayPin == ack.relayPin) {
            remove = pending->remove;
            known = true;
            if (ack.success != 0U && remove) registry.removeLoad(originMac, ack.relayPin);
            removePendingHardwareCommand(ack.commandId);
        }
        xSemaphoreGive(stateMutex);
    }

    if (!known) return;

    char target[18]{};
    formatMacAddressText(target, sizeof(target), originMac);
    const bool success = ack.success != 0U;
    mqttManager.publishAcknowledgement(
        ack.commandId,
        remove ? "REMOVE_LOAD" : "CONFIGURE_LOAD",
        success ? AckStatus::APPLIED : AckStatus::FAILED,
        success ? "applied" : hardwareConfigurationFailureText(
            static_cast<HardwareConfigurationFailureReason>(ack.failureReason)),
        target);

    if (success) {
        mqttManager.publishEvent(remove ? "LOAD_REMOVED" : "LOAD_CONFIGURED", target, nullptr);
        xSemaphoreGive(optimizationTriggerSemaphore);
    }
}

void espNowCommunicationTask(void* parameter)
{
    (void)parameter;
    const Load::MacAddress localMac = communication.getLocalMacAddress();

    while (true) {
        EspNowCommunication::ReceivedMessage received{};
        if (!communication.receive(received, 500U)) continue;

        const bool localDestination =
            received.message.header.destinationMacAddress == localMac;
        const auto type = received.message.header.messageType;
        const Load::MacAddress originMac = received.message.header.originMacAddress;

        if (!localDestination) {
            Load::MacAddress nextHop{};
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                const bool found = findNextHopFromCentral(
                    localMac,
                    received.message.header.destinationMacAddress,
                    nextHop);
                xSemaphoreGive(stateMutex);
                if (found) communication.forwardMessageTo(nextHop, received.message);
            }
            continue;
        }

        if (type == EspNowCommunication::MessageType::NODE_REPORT &&
            received.message.header.payloadLength == sizeof(NodeReportPacket)) {
            NodeReportPacket report{};
            std::memcpy(&report, received.message.payload.data(), sizeof(report));

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                registry.applyNodeReport(
                    report,
                    static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
                xSemaphoreGive(stateMutex);
            }

            NodeReportAcknowledgementPacket ack{};
            ack.reportSequenceId = report.reportSequenceId;
            ack.status = static_cast<std::uint8_t>(NodeReportAckStatus::ACCEPTED);
            communication.sendTo(
                received.senderMacAddress,
                originMac,
                EspNowCommunication::MessageType::NODE_REPORT_ACK,
                ack);
            xSemaphoreGive(optimizationTriggerSemaphore);

        } else if (type == EspNowCommunication::MessageType::IDENTITY_REPORT &&
                   received.message.header.payloadLength == sizeof(IdentityReportPacket)) {
            IdentityReportPacket report{};
            std::memcpy(&report, received.message.payload.data(), sizeof(report));

            bool isNew = false;
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                isNew = commissioningRegistry.recordDiscovered(
                    originMac,
                    static_cast<NodeRole>(report.role),
                    report.firmwareVersion,
                    report.chipModel,
                    report.relayPins.data(),
                    report.relayCapabilityCount,
                    static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));

                NodeCommissioningRegistry::Diagnostics diagnostics{};
                diagnostics.freeHeapBytes = report.freeHeapBytes;
                diagnostics.minFreeHeapBytes = report.minFreeHeapBytes;
                diagnostics.flashSizeBytes = report.flashSizeBytes;
                diagnostics.psramSizeBytes = report.psramSizeBytes;
                diagnostics.siliconRevision = report.siliconRevision;
                diagnostics.cpuCores = report.cpuCores;
                diagnostics.cpuFrequencyMhz = report.cpuFrequencyMhz;
                diagnostics.temperatureAvailable = report.temperatureAvailable != 0U;
                diagnostics.temperatureCelsius = report.temperatureCelsius;
                std::snprintf(diagnostics.resetReason, sizeof(diagnostics.resetReason), "%s", report.resetReason);
                commissioningRegistry.updateDiagnostics(originMac, diagnostics);
                xSemaphoreGive(stateMutex);
            }

            if (isNew) {
                char target[18]{};
                formatMacAddressText(target, sizeof(target), originMac);
                mqttManager.publishEvent("NODE_DISCOVERED", target, nullptr);
            }

        } else if (type == EspNowCommunication::MessageType::COMMISSION_ACK &&
                   received.message.header.payloadLength == sizeof(CommissionAckPacket)) {
            CommissionAckPacket ack{};
            std::memcpy(&ack, received.message.payload.data(), sizeof(ack));
            bool persisted = false;

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                commissioningRegistry.applyCommissionResult(
                    originMac,
                    ack.success != 0U,
                    static_cast<NodeLifecycleState>(ack.resultingState));
                persisted = commissioningRegistry.persist();
                xSemaphoreGive(stateMutex);
            }

            char target[18]{};
            formatMacAddressText(target, sizeof(target), originMac);
            mqttManager.publishAcknowledgement(
                ack.commandId,
                "NODE_COMMISSIONING",
                ack.success != 0U && persisted ? AckStatus::APPLIED : AckStatus::FAILED,
                ack.success != 0U && persisted ? "applied" : "Node rejected or persistence failed",
                target);
            xSemaphoreGive(optimizationTriggerSemaphore);

        } else if (type == EspNowCommunication::MessageType::DECOMMISSION_ACK &&
                   received.message.header.payloadLength == sizeof(DecommissionAckPacket)) {
            DecommissionAckPacket ack{};
            std::memcpy(&ack, received.message.payload.data(), sizeof(ack));
            char target[18]{};
            formatMacAddressText(target, sizeof(target), originMac);
            mqttManager.publishAcknowledgement(
                ack.commandId,
                "DECOMMISSION_NODE",
                ack.success != 0U ? AckStatus::APPLIED : AckStatus::FAILED,
                ack.success != 0U ? "Node cleared" : "Node could not clear itself",
                target);

        } else if (type == EspNowCommunication::MessageType::CONFIGURE_LOAD_ACK &&
                   received.message.header.payloadLength == sizeof(ConfigureLoadAcknowledgementPacket)) {
            ConfigureLoadAcknowledgementPacket ack{};
            std::memcpy(&ack, received.message.payload.data(), sizeof(ack));
            handleHardwareConfigurationAcknowledgement(originMac, ack);

        } else if (type == EspNowCommunication::MessageType::ACKNOWLEDGEMENT &&
                   received.message.header.payloadLength == sizeof(RelayCommandAcknowledgementPacket)) {
            RelayCommandAcknowledgementPacket ack{};
            std::memcpy(&ack, received.message.payload.data(), sizeof(ack));
            handleRelayAcknowledgement(originMac, ack);
        }
    }
}

void checkWiFiProvisioningTrigger()
{
    constexpr std::uint32_t RETRIES_BEFORE_PORTAL = 3U;
    const bool failedRepeatedly =
        wifiManager.getState() == WiFiConnectionState::DISCONNECTED &&
        wifiManager.getReconnectAttemptCount() >= RETRIES_BEFORE_PORTAL;

    if (failedRepeatedly && !wifiProvisioningPortal.isActive()) {
        wifiProvisioningPortal.begin(kilowatts::KILOWATTS_RADIO_CHANNEL);
    } else if (wifiManager.isConnected() && wifiProvisioningPortal.isActive()) {
        wifiProvisioningPortal.end();
    }
}

void watchdogTask(void* parameter)
{
    (void)parameter;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CentralNodeConfig::WATCHDOG_PERIOD_MILLISECONDS));
        checkWiFiProvisioningTrigger();
    }
}

[[noreturn]] void performCentralFactoryReset()
{
    nvs_flash_erase();
    nvs_flash_init();
    vTaskDelay(pdMS_TO_TICKS(200U));
    esp_restart();
    while (true) {}
}

LoadCommandResult handleLoadCommand(void* context, const LoadCommandRequest& request)
{
    (void)context;
    LoadCommandResult result{};

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::snprintf(result.reason, sizeof(result.reason), "state is busy");
        return result;
    }

    Load* load = registry.findMutableLoad(request.nodeMacAddress, request.relayPin);
    if (load == nullptr) {
        std::snprintf(result.reason, sizeof(result.reason), "load not found");
        xSemaphoreGive(stateMutex);
        return result;
    }

    const std::uint16_t priority = request.hasPriority ? request.priority : load->getPriority();
    const LoadMode::Value mode = request.hasMode ? request.mode : load->getMode();
    AutoSchedule schedule = request.hasSchedule ? request.schedule : load->getSchedule();

    if (priority > CentralNodeConfig::bestFirstSearchWeights().maximumAllowedPriority) {
        std::snprintf(result.reason, sizeof(result.reason), "priority out of range");
        xSemaphoreGive(stateMutex);
        return result;
    }

    if (mode == LoadMode::Fixed::ON || mode == LoadMode::Fixed::OFF) {
        schedule = AutoSchedule{false, 0U, 0U};
    }

    const LoadConfigurationStore::ConfigurationEntry entry{
        request.nodeMacAddress,
        request.relayPin,
        priority,
        mode,
        schedule};

    if (!loadConfigurationStore.setConfiguration(entry) ||
        !loadConfigurationStore.applyToLoad(*load) ||
        !loadConfigurationStore.persist()) {
        std::snprintf(result.reason, sizeof(result.reason), "could not save load settings");
        xSemaphoreGive(stateMutex);
        return result;
    }

    xSemaphoreGive(stateMutex);
    xSemaphoreGive(optimizationTriggerSemaphore);
    result.accepted = true;
    result.completed = true;
    std::snprintf(result.reason, sizeof(result.reason), "applied");
    return result;
}

LoadCommandResult handleSystemCommand(void* context, const SystemCommandRequest& request)
{
    (void)context;
    LoadCommandResult result{};

    if (request.action == SystemCommandAction::REQUEST_OPTIMIZATION_CYCLE) {
        xSemaphoreGive(optimizationTriggerSemaphore);
        result.accepted = true;
        result.completed = true;
        std::snprintf(result.reason, sizeof(result.reason), "optimization requested");
        return result;
    }

    if (request.action == SystemCommandAction::APPLY_SAFETY_CONFIG) {
        if (!request.hasSafetyPolicy) {
            std::snprintf(result.reason, sizeof(result.reason), "safetyConfig required");
            return result;
        }

        const CentralConfigurationStore::SafetyPolicy policy{
            true,
            request.minimumStateOfChargePercent,
            request.warningStateOfChargePercent,
            request.targetRuntimeHours,
            request.safetyFactor,
            request.maximumBatteryDischargeCurrentAmps,
            request.maximumMainCurrentAmps};

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
            result.accepted = centralConfigurationStore.setSafetyPolicy(policy) &&
                              centralConfigurationStore.persist();
            xSemaphoreGive(stateMutex);
        }

        result.completed = result.accepted;
        std::snprintf(result.reason, sizeof(result.reason), result.accepted ? "applied" : "invalid safetyConfig");
        if (result.accepted) xSemaphoreGive(optimizationTriggerSemaphore);
        return result;
    }

    if (request.action == SystemCommandAction::FACTORY_RESET_CENTRAL) {
        if (std::strcmp(request.confirmText, "FACTORY_RESET_CONFIRMED") != 0) {
            std::snprintf(result.reason, sizeof(result.reason), "confirmation required");
            return result;
        }
        performCentralFactoryReset();
    }

    if (request.action == SystemCommandAction::FACTORY_RESET_NODE) {
        if (!request.hasTargetNodeMacAddress ||
            std::strcmp(request.confirmText, "FACTORY_RESET_CONFIRMED") != 0) {
            std::snprintf(result.reason, sizeof(result.reason), "target and confirmation required");
            return result;
        }

        Load::MacAddress nextHop{};
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
            const bool found = findNextHopFromCentral(
                communication.getLocalMacAddress(),
                request.targetNodeMacAddress,
                nextHop);
            xSemaphoreGive(stateMutex);
            if (found) {
                FactoryResetCommandPacket packet{};
                packet.commandId = request.commandId;
                packet.confirmToken = FACTORY_RESET_CONFIRM_TOKEN;
                result.accepted = communication.sendTo(
                    nextHop,
                    request.targetNodeMacAddress,
                    EspNowCommunication::MessageType::FACTORY_RESET_COMMAND,
                    packet);
            }
        }

        result.completed = false;
        std::snprintf(result.reason, sizeof(result.reason), result.accepted ? "sent to Node" : "Node offline or unreachable");
        return result;
    }

    std::snprintf(result.reason, sizeof(result.reason), "unknown action");
    return result;
}

LoadCommandResult handleConfigCommand(void* context, const ConfigCommandRequest& request)
{
    (void)context;
    LoadCommandResult result{};
    const Load::MacAddress localMac = communication.getLocalMacAddress();

    if (request.action == ConfigCommandAction::CONFIGURE_BATTERY_SENSOR) {
        if (request.nodeMacAddress != localMac || !request.hasBatterySensorConfiguration) {
            std::snprintf(result.reason, sizeof(result.reason), "battery sensor belongs to Central");
            return result;
        }

        const CentralConfigurationStore::BatterySensorConfiguration configuration{
            true,
            request.batteryShuntResistanceOhms,
            request.batteryMaximumExpectedCurrentAmps,
            request.batteryEmaAlpha,
            request.batteryCapacityAmpHours,
            request.batteryInitialStateOfChargePercent,
            request.batteryNominalVoltageVolts};

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            std::snprintf(result.reason, sizeof(result.reason), "state is busy");
            return result;
        }

        const bool firstConfiguration = !centralConfigurationStore.getConfiguration().batterySensor.configured;
        const bool applied =
            centralConfigurationStore.setBatterySensor(configuration) &&
            sensors.configureSensor(INA219Monitor::SensorConfiguration{
                configuration.shuntResistanceOhms,
                configuration.maximumExpectedCurrentAmps,
                configuration.emaAlpha}) &&
            sensors.isSensorPresent() &&
            centralConfigurationStore.persist();

        if (applied) {
            batterySensorConfigured = true;
            batteryReadingValid = false;
            if (firstConfiguration || !batteryStateOfCharge.isInitialized()) {
                batteryStateOfCharge.initialize(
                    configuration.batteryCapacityAmpHours,
                    configuration.initialStateOfChargePercent);
            }
        }

        xSemaphoreGive(stateMutex);
        result.accepted = applied;
        result.completed = applied;
        std::snprintf(result.reason, sizeof(result.reason), applied ? "applied" : "invalid battery configuration");
        if (applied) xSemaphoreGive(optimizationTriggerSemaphore);
        return result;
    }

    if (request.action == ConfigCommandAction::CONFIGURE_LOAD) {
        if (!request.hasLoadConfiguration || !request.hasRelayPin) {
            std::snprintf(result.reason, sizeof(result.reason), "load configuration required");
            return result;
        }

        if (request.nodeMacAddress == localMac) {
            if (!CentralNodeConfig::isVerifiedRelayPin(request.relayPin)) {
                std::snprintf(result.reason, sizeof(result.reason), "GPIO pin is not supported by Central");
                return result;
            }

            NodeLoadHardwareStore::LoadConfiguration configuration{};
            std::snprintf(configuration.name, sizeof(configuration.name), "%s", request.loadName);
            configuration.relayPin = request.relayPin;
            configuration.relayActiveHigh = request.relayActiveHigh;
            configuration.mode = request.mode;
            configuration.priority = request.priority;
            configuration.nominalVoltageVolts = request.nominalVoltageVolts;
            configuration.nominalCurrentAmps = request.nominalCurrentAmps;
            configuration.startupWatts = request.startupWatts;
            configuration.schedule = request.schedule;

            HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
            bool applied = false;
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
                applied = centralLoadHardwareStore.configureNewLoad(
                    configuration, relays, centralNode, reason);
                if (applied) {
                    registry.addLocalCentralNode(
                        CentralNodeConfig::CENTRAL_NODE_NAME,
                        centralNode,
                        static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
                }
                xSemaphoreGive(stateMutex);
            }

            result.accepted = applied;
            result.completed = applied;
            std::snprintf(result.reason, sizeof(result.reason),
                          applied ? "applied" : hardwareConfigurationFailureText(reason));
            if (applied) xSemaphoreGive(optimizationTriggerSemaphore);
            return result;
        }

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            std::snprintf(result.reason, sizeof(result.reason), "state is busy");
            return result;
        }

        const auto* record = commissioningRegistry.findByMac(request.nodeMacAddress);
        const bool validNode = record != nullptr && record->role == NodeRole::SMART &&
            (record->lifecycleState == NodeLifecycleState::COMMISSIONED ||
             record->lifecycleState == NodeLifecycleState::OPERATIONAL) &&
            nodeDeclaresRelayPin(*record, request.relayPin);

        Load::MacAddress nextHop{};
        const bool routeFound = validNode && findNextHopFromCentral(localMac, request.nodeMacAddress, nextHop);
        xSemaphoreGive(stateMutex);

        if (!routeFound) {
            std::snprintf(result.reason, sizeof(result.reason), "Node offline or unreachable");
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
        packet.startupWatts = request.startupWatts;
        packet.scheduleEnabled = request.schedule.enabled ? 1U : 0U;
        packet.scheduleHour = request.schedule.hour;
        packet.scheduleMinute = request.schedule.minute;

        result.accepted = communication.sendTo(
            nextHop,
            request.nodeMacAddress,
            EspNowCommunication::MessageType::CONFIGURE_LOAD_COMMAND,
            packet);
        result.completed = false;

        if (result.accepted && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            pendingHardwareCommands.push_back(PendingHardwareCommand{
                request.commandId, request.nodeMacAddress, request.relayPin, false});
            xSemaphoreGive(stateMutex);
        }

        std::snprintf(result.reason, sizeof(result.reason), result.accepted ? "sent to Node" : "send failed");
        return result;
    }

    if (request.action == ConfigCommandAction::REMOVE_LOAD) {
        if (!request.hasRelayPin) {
            std::snprintf(result.reason, sizeof(result.reason), "relayPin required");
            return result;
        }

        if (request.nodeMacAddress == localMac) {
            HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
            bool removed = false;
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
                removed = centralLoadHardwareStore.removeLoad(
                    request.relayPin, relays, centralNode, reason);
                if (removed) {
                    registry.addLocalCentralNode(
                        CentralNodeConfig::CENTRAL_NODE_NAME,
                        centralNode,
                        static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
                }
                xSemaphoreGive(stateMutex);
            }

            result.accepted = removed;
            result.completed = removed;
            std::snprintf(result.reason, sizeof(result.reason),
                          removed ? "removed" : hardwareConfigurationFailureText(reason));
            if (removed) xSemaphoreGive(optimizationTriggerSemaphore);
            return result;
        }

        Load::MacAddress nextHop{};
        bool routeFound = false;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
            routeFound = registry.findMutableLoad(request.nodeMacAddress, request.relayPin) != nullptr &&
                         findNextHopFromCentral(localMac, request.nodeMacAddress, nextHop);
            xSemaphoreGive(stateMutex);
        }

        if (!routeFound) {
            std::snprintf(result.reason, sizeof(result.reason), "load not found or Node offline");
            return result;
        }

        RemoveLoadCommandPacket packet{request.commandId, request.relayPin};
        result.accepted = communication.sendTo(
            nextHop,
            request.nodeMacAddress,
            EspNowCommunication::MessageType::CONFIGURE_LOAD_COMMAND,
            &packet,
            sizeof(packet));
        result.completed = false;

        if (result.accepted && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            pendingHardwareCommands.push_back(PendingHardwareCommand{
                request.commandId, request.nodeMacAddress, request.relayPin, true});
            xSemaphoreGive(stateMutex);
        }

        std::snprintf(result.reason, sizeof(result.reason), result.accepted ? "sent to Node" : "send failed");
        return result;
    }

    if (request.action == ConfigCommandAction::DECOMMISSION_NODE) {
        if (request.nodeMacAddress == localMac) {
            std::snprintf(result.reason, sizeof(result.reason), "Central cannot decommission itself here");
            return result;
        }

        Load::MacAddress nextHop{};
        bool accepted = false;
        bool routeFound = false;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
            accepted = commissioningRegistry.decommission(request.nodeMacAddress) && commissioningRegistry.persist();
            routeFound = accepted && findNextHopFromCentral(localMac, request.nodeMacAddress, nextHop);
            xSemaphoreGive(stateMutex);
        }

        if (!accepted) {
            std::snprintf(result.reason, sizeof(result.reason), "Node cannot be decommissioned");
            return result;
        }

        if (routeFound) {
            DecommissionCommandPacket packet{request.commandId};
            communication.sendTo(
                nextHop,
                request.nodeMacAddress,
                EspNowCommunication::MessageType::DECOMMISSION_COMMAND,
                packet);
        }

        result.accepted = true;
        result.completed = false;
        std::snprintf(result.reason, sizeof(result.reason), "Central removed Node");
        xSemaphoreGive(optimizationTriggerSemaphore);
        return result;
    }

    if (request.action == ConfigCommandAction::COMMISSION_NODE ||
        request.action == ConfigCommandAction::RENAME_NODE) {
        if (!request.hasFriendlyName || request.nodeMacAddress == localMac) {
            std::snprintf(result.reason, sizeof(result.reason), "friendlyName and Smart Node required");
            return result;
        }

        Load::MacAddress nextHop{};
        NodeLifecycleState previousState = NodeLifecycleState::UNCOMMISSIONED;
        bool ready = false;

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) == pdTRUE) {
            const auto* record = commissioningRegistry.findByMac(request.nodeMacAddress);
            if (record != nullptr) previousState = record->lifecycleState;
            ready = record != nullptr &&
                    commissioningRegistry.requestCommissioning(request.nodeMacAddress, request.friendlyName) &&
                    findNextHopFromCentral(localMac, request.nodeMacAddress, nextHop);
            if (!ready && record != nullptr) {
                commissioningRegistry.applyCommissionResult(request.nodeMacAddress, false, previousState);
            }
            xSemaphoreGive(stateMutex);
        }

        if (!ready) {
            std::snprintf(result.reason, sizeof(result.reason), "Node offline or unreachable");
            return result;
        }

        CommissionCommandPacket packet{};
        packet.commandId = request.commandId;
        std::snprintf(packet.friendlyName, sizeof(packet.friendlyName), "%s", request.friendlyName);

        result.accepted = communication.sendTo(
            nextHop,
            request.nodeMacAddress,
            EspNowCommunication::MessageType::COMMISSION_COMMAND,
            packet);
        result.completed = false;

        if (!result.accepted && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
            commissioningRegistry.applyCommissionResult(request.nodeMacAddress, false, previousState);
            xSemaphoreGive(stateMutex);
        }

        std::snprintf(result.reason, sizeof(result.reason), result.accepted ? "sent to Node" : "send failed");
        return result;
    }

    std::snprintf(result.reason, sizeof(result.reason), "unknown action");
    return result;
}

void printCentralBootSummary(const Load::MacAddress& localMac)
{
    char mac[18]{};
    formatMacAddressText(mac, sizeof(mac), localMac);
    ESP_LOGI(TAG, "Central %s | local loads=%u | nodes=%u | sensor=%s",
             mac,
             static_cast<unsigned int>(centralNode.getNumberOfLoads()),
             static_cast<unsigned int>(registry.getNumberOfNodes()),
             batterySensorConfigured ? toText(sensors.getLastMeasurementSource()) : "NOT_CONFIGURED");
}

} // namespace

#endif // KILOWATTS_CENTRAL_RUNTIME_H
