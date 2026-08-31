
#include "BestFirstSearch.h"
#include "CentralConfigurationStore.h"
#include "CentralNodeConfig.h"
#include "CentralNodeRegistry.h"
#include "CentralConsole.h"
#include "ChipInfo.h"
#include "CommissioningPackets.h"
#include "CurrentTimeProvider.h"
#include "EspNowCommunication.h"
#include "FirmwareVersion.h"
#include "HardwareConfigurationPackets.h"
#include "Load.h"
#include "LoadConfigurationStore.h"
#include "LoadFilter.h"
#include "MqttCredentialsStore.h"
#include "MqttManager.h"
#include "Node.h"
#include "NodeCommissioningRegistry.h"
#include "NodeRegistryJson.h"
#include "NodeReportPackets.h"
#include "NodeLoadHardwareStore.h"
#include "PowerManager.h"
#include "RadioChannelStore.h"
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
#include <vector>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

using namespace kilowatts;

static const char* TAG = "CENTRAL";

namespace {

EspNowCommunication communication(kilowatts::KILOWATTS_RADIO_CHANNEL);
CurrentTimeProvider currentTimeProvider;
WiFiManager wifiManager(kilowatts::KILOWATTS_RADIO_CHANNEL);
WiFiCredentialsStore wifiCredentialsStore;
RadioChannelStore radioChannelStore;
WiFiProvisioningPortal wifiProvisioningPortal;
MqttManager mqttManager(
    CentralNodeConfig::MQTT_TOPIC_NAMESPACE,
    CentralNodeConfig::MQTT_DEVICE_ID,
    CentralNodeConfig::MQTT_SCHEMA_VERSION);
MqttCredentialsStore mqttCredentialsStore;
CentralConsole centralConsole;

PowerManager batteryMonitor;
RelayController relays;
NodeLoadHardwareStore centralLoadHardwareStore;
CentralConfigurationStore centralConfigurationStore;
LoadConfigurationStore centralLoadConfigurationStore;
CentralNodeRegistry registry;
NodeCommissioningRegistry commissioningRegistry;
RelayCommandDispatcher relayCommandDispatcher;
ChipInfo chipInfo;
Node centralNode(EspNowCommunication::MacAddress{});

SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t optimizationTriggerSemaphore = nullptr;

PowerMeasurements latestBatteryMeasurements{0.0F, 0.0F, 0.0F};
bool batteryReadingValid = false;
std::uint32_t batteryReadingMilliseconds = 0U;
std::int64_t lastOptimizationEpochSeconds = 0;
std::uint32_t relayCommandErrorCount = 0U;
bool mqttStarted = false;
bool mqttCredentialsUnavailableLogged = false;

bool previousBatteryReserveReached = false;
bool previousRequiredRuntimeAchievable = true;
bool previousHardwareBatteryReadingValid = true;
bool previousMeasuredPowerExceededBudget = false;

struct NodeOnlineTracking {
    Load::MacAddress macAddress;
    bool wasOnline;
};
std::vector<NodeOnlineTracking> nodeOnlineTracking;
bool mqttBrokerStartupFailedLogged = false;
std::uint32_t optimizerIntervalMilliseconds = CentralNodeConfig::DEFAULT_OPTIMIZATION_PERIOD_MILLISECONDS;

struct PlanningSnapshot {
    bool budgetValid = false;
    PowerBudget budget{};
    std::size_t fixedOnCount = 0U;
    std::size_t fixedOffCount = 0U;
    std::size_t automaticCandidateCount = 0U;
    std::size_t automaticSelectedCount = 0U;
};

PlanningSnapshot latestPlanningSnapshot{};

constexpr const char* OPTIMIZER_NVS_NAMESPACE = "kw_optimizer";
constexpr const char* OPTIMIZER_NVS_INTERVAL_SECONDS_KEY = "interval_s";

std::uint32_t optimizerMinIntervalSeconds()
{
    return CentralNodeConfig::MIN_OPTIMIZATION_PERIOD_MILLISECONDS / 1000U;
}

std::uint32_t optimizerMaxIntervalSeconds()
{
    return CentralNodeConfig::MAX_OPTIMIZATION_PERIOD_MILLISECONDS / 1000U;
}

bool isValidOptimizerIntervalSeconds(std::uint32_t seconds)
{
    return seconds >= optimizerMinIntervalSeconds() && seconds <= optimizerMaxIntervalSeconds();
}

std::uint32_t getOptimizerIntervalSeconds()
{
    return optimizerIntervalMilliseconds / 1000U;
}

std::uint32_t getOptimizerIntervalMilliseconds()
{
    return optimizerIntervalMilliseconds;
}

void applyRadioChannelOverride()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        result = nvs_flash_init();
    }
    if (result != ESP_OK) return;

    std::uint8_t channel = 0U;
    if (radioChannelStore.load(channel)) {
        (void)communication.setChannel(channel);
        wifiManager.setRequiredChannel(channel);
    }
}

void loadOptimizerIntervalConfiguration()
{
    nvs_handle_t handle = 0;
    if (nvs_open(OPTIMIZER_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;

    std::uint32_t savedSeconds = 0U;
    const esp_err_t result = nvs_get_u32(handle, OPTIMIZER_NVS_INTERVAL_SECONDS_KEY, &savedSeconds);
    nvs_close(handle);

    if (result == ESP_OK && isValidOptimizerIntervalSeconds(savedSeconds)) {
        optimizerIntervalMilliseconds = savedSeconds * 1000U;
    }
}

bool saveOptimizerIntervalSeconds(std::uint32_t seconds)
{
    if (!isValidOptimizerIntervalSeconds(seconds)) return false;

    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(OPTIMIZER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_u32(handle, OPTIMIZER_NVS_INTERVAL_SECONDS_KEY, seconds);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    if (result != ESP_OK) return false;

    optimizerIntervalMilliseconds = seconds * 1000U;
    return true;
}

float runtimeTargetReferenceHours = 0.0F;
std::int64_t runtimeTargetReferenceEpochSeconds = 0;
bool runtimeTargetReferenceValid = false;

float computeRemainingRequiredRuntimeHours(
    float configuredRequiredRuntimeHours,
    bool currentTimeValid,
    std::int64_t nowEpochSeconds)
{
    if (configuredRequiredRuntimeHours <= 0.0F) return 0.0F;
    if (!currentTimeValid) return configuredRequiredRuntimeHours;

    if (!runtimeTargetReferenceValid || runtimeTargetReferenceHours != configuredRequiredRuntimeHours) {
        runtimeTargetReferenceHours = configuredRequiredRuntimeHours;
        runtimeTargetReferenceEpochSeconds = nowEpochSeconds;
        runtimeTargetReferenceValid = true;
    }

    const float elapsedHours =
        static_cast<float>(nowEpochSeconds - runtimeTargetReferenceEpochSeconds) / 3600.0F;
    const float remainingHours = configuredRequiredRuntimeHours - elapsedHours;
    return remainingHours > 0.0F ? remainingHours : 0.0F;
}

struct PendingHardwareCommand {
    std::uint32_t commandId;
    Load::MacAddress nodeMacAddress;
    std::uint8_t relayPin;
    bool remove;
};

std::vector<PendingHardwareCommand> pendingHardwareCommands;
std::uint32_t nextConsoleCommandId = 0x80000000U;

template <std::size_t N>
void copyText(char (&destination)[N], const char* source)
{
    static_assert(N > 0U, "destination buffer must not be empty");
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }

    std::size_t index = 0U;
    while ((index + 1U) < N && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

std::uint32_t allocateConsoleCommandId()
{
    ++nextConsoleCommandId;
    if (nextConsoleCommandId == 0U) nextConsoleCommandId = 0x80000001U;
    return nextConsoleCommandId;
}

esp_timer_handle_t centralRestartTimer = nullptr;

void scheduleCentralRestart(std::uint64_t delayMicroseconds)
{
    if (centralRestartTimer == nullptr) {
        const esp_timer_create_args_t timerArgs = {
            [](void*) { esp_restart(); },
            nullptr,
            ESP_TIMER_TASK,
            "kw_central_restart",
            true};
        if (esp_timer_create(&timerArgs, &centralRestartTimer) != ESP_OK) {
            esp_restart();
            return;
        }
    }
    esp_timer_start_once(centralRestartTimer, delayMicroseconds);
}

const char* wifiStateText(WiFiConnectionState state)
{
    switch (state) {
        case WiFiConnectionState::DISCONNECTED: return "DISCONNECTED";
        case WiFiConnectionState::SCANNING: return "SCANNING";
        case WiFiConnectionState::CONNECTING: return "CONNECTING";
        case WiFiConnectionState::CONNECTED_AWAITING_IP: return "CONNECTED_AWAITING_IP";
        case WiFiConnectionState::CONNECTED_WITH_IP: return "CONNECTED";
        case WiFiConnectionState::RADIO_CHANNEL_MISMATCH: return "CHANNEL_MISMATCH";
    }
    return "UNKNOWN";
}

const char* timeSourceText(TimeSource source)
{
    switch (source) {
        case TimeSource::NONE: return "NONE";
        case TimeSource::NTP: return "NTP";
        case TimeSource::MANUAL: return "MANUAL";
    }
    return "UNKNOWN";
}

const char* measurementSourceText(MeasurementSource source)
{
    switch (source) {
        case MeasurementSource::NONE: return "NONE";
        case MeasurementSource::HARDWARE: return "HARDWARE";
        case MeasurementSource::SIMULATED: return "SIMULATED";
    }
    return "UNKNOWN";
}

const char* stateOfChargeSourceText(StateOfChargeSource source)
{
    switch (source) {
        case StateOfChargeSource::UNKNOWN: return "UNKNOWN";
        case StateOfChargeSource::INITIAL: return "INITIAL";
        case StateOfChargeSource::PERSISTED: return "PERSISTED";
        case StateOfChargeSource::COULOMB_COUNTING: return "COULOMB_COUNTING";
        case StateOfChargeSource::VOLTAGE_ESTIMATE: return "VOLTAGE_ESTIMATE";
        case StateOfChargeSource::SIMULATED: return "SIMULATED";
    }
    return "UNKNOWN";
}

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

bool isCommissionedLifecycle(NodeLifecycleState state)
{
    return state == NodeLifecycleState::COMMISSIONED || state == NodeLifecycleState::OPERATIONAL;
}

bool isNodeOnline(const CentralNodeRegistry::PlanningNode& node, std::uint32_t nowMilliseconds)
{
    return node.isCentralNode ||
           (nowMilliseconds - node.lastSeenMilliseconds) <= CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS;
}

bool isCommissionedSmartNode(const Load::MacAddress& macAddress)
{
    const auto* record = commissioningRegistry.findByMac(macAddress);
    return record != nullptr && record->role == NodeRole::SMART && isCommissionedLifecycle(record->lifecycleState);
}

bool batteryTelemetryIsFresh(std::uint32_t nowMilliseconds)
{
    return batteryReadingValid &&
           (nowMilliseconds - batteryReadingMilliseconds) <= CentralNodeConfig::BATTERY_TELEMETRY_STALE_TIMEOUT_MILLISECONDS;
}

bool findNextHopFromCentral(
    const Load::MacAddress& centralMac,
    const Load::MacAddress& destinationMac,
    Load::MacAddress& nextHop)
{
    const auto* current = registry.findNodeByMacAddress(destinationMac);
    if (current == nullptr || current->isCentralNode) return false;

    for (std::size_t depth = 0U; depth <= registry.getNumberOfNodes(); ++depth) {
        if (current->nextHopToCentralMacAddress == centralMac) {
            nextHop = current->node.getMacAddress();
            return true;
        }
        current = registry.findNodeByMacAddress(current->nextHopToCentralMacAddress);
        if (current == nullptr || current->isCentralNode) return false;
    }
    return false;
}

bool routeToSmartNode(const Load::MacAddress& destinationMac, Load::MacAddress& nextHop)
{
    return findNextHopFromCentral(communication.getLocalMacAddress(), destinationMac, nextHop);
}

CommandResult commandResult(bool accepted, bool completed, const char* reason)
{
    CommandResult result{};
    result.accepted = accepted;
    result.completed = completed;
    copyText(result.reason, reason != nullptr ? reason : "");
    return result;
}

constexpr std::uint8_t BEST_FIRST_NONE = 0U;
constexpr std::uint8_t BEST_FIRST_POWER_BUDGET_EXCEEDED = 2U;

const char* loadModeText(LoadMode::Value mode)
{
    switch (mode) {
        case LoadMode::Value::FIXED_OFF: return "FIXED_OFF";
        case LoadMode::Value::FIXED_ON: return "FIXED_ON";
        case LoadMode::Value::AUTO_OFF: return "AUTO_OFF";
        case LoadMode::Value::AUTO_ON: return "AUTO_ON";
    }
    return "UNKNOWN";
}

const char* loadPowerTypeText(LoadPowerType powerType)
{
    switch (powerType) {
        case LoadPowerType::AC: return "AC";
        case LoadPowerType::DC: return "DC";
    }
    return "UNKNOWN";
}

const char* bestFirstRejectionText(std::uint8_t reason)
{
    switch (reason) {
        case BEST_FIRST_NONE: return "NONE";
        case BEST_FIRST_POWER_BUDGET_EXCEEDED: return "POWER_BUDGET_EXCEEDED";
        default: return "UNKNOWN";
    }
}

bool containsLoadPointer(const std::vector<const Load*>& loads, const Load* candidate)
{
    return std::find(loads.begin(), loads.end(), candidate) != loads.end();
}

PendingHardwareCommand* findPendingHardwareCommand(std::uint32_t commandId)
{
    for (PendingHardwareCommand& pending : pendingHardwareCommands) {
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

void configureLocalHardware(const Load::MacAddress& localMac)
{
    centralNode = Node(localMac);

    if (centralLoadHardwareStore.loadPersisted()) {
        HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
        (void)centralLoadHardwareStore.applyPersistedConfigurations(relays, centralNode, reason);
    }

    registry.addLocalCentralNode(
        CentralNodeConfig::CENTRAL_NODE_NAME,
        centralNode,
        static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
}

bool applyBatteryConfiguration(
    const CentralConfigurationStore::BatterySensorConfiguration& configuration,
    bool restorePersistedStateOfCharge)
{
    if (!CentralConfigurationStore::isValidBatterySensor(configuration)) return false;

    const bool simulating = batteryMonitor.isSimulationEnabled();
    if (!simulating && !configuration.ina219Configured) return false;

    const auto& planning = centralConfigurationStore.getConfiguration().powerPlanning;
    const float minimumStateOfChargePercent =
        planning.configured ? planning.minimumStateOfChargePercent : 0.0F;

    const PowerManager::BusConfiguration busConfiguration{
        CentralNodeConfig::I2C_SERIAL_DATA_PIN,
        CentralNodeConfig::I2C_SERIAL_CLOCK_PIN,
        CentralNodeConfig::I2C_PORT_NUMBER,
        CentralNodeConfig::I2C_CLOCK_SPEED_HZ};

    const PowerManager::SensorConfiguration sensorConfiguration{
        configuration.shuntResistanceOhms,
        configuration.maximumExpectedCurrentAmps,
        configuration.emaAlpha};

    const PowerManager::BatteryConfiguration batteryConfiguration{
        configuration.batteryMetadataConfigured ? configuration.nominalVoltageVolts : 0.0F,
        configuration.batteryMetadataConfigured ? configuration.batteryCapacityAmpHours : 0.0F,
        minimumStateOfChargePercent};

    if (!batteryMonitor.initialize(busConfiguration, sensorConfiguration, batteryConfiguration)) return false;

    if (!configuration.batteryMetadataConfigured) return true;
    return batteryMonitor.initializeStateOfCharge(
        configuration.initialStateOfChargePercent,
        restorePersistedStateOfCharge);
}

bool applyPersistedBatterySensorConfiguration()
{
    const auto& battery = centralConfigurationStore.getConfiguration().batterySensor;
    if (!battery.ina219Configured && !battery.batteryMetadataConfigured) return false;
    return applyBatteryConfiguration(battery, true);
}

void sensorAcquisitionTask(void* parameter)
{
    (void)parameter;
    TickType_t previousTick = xTaskGetTickCount();
    std::uint32_t cyclesSincePersist = 0U;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CentralNodeConfig::SENSOR_ACQUISITION_PERIOD_MILLISECONDS));

        const TickType_t nowTick = xTaskGetTickCount();
        const float deltaSeconds = static_cast<float>(pdTICKS_TO_MS(nowTick - previousTick)) / 1000.0F;
        previousTick = nowTick;

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) != pdTRUE) continue;

        const bool simulating = batteryMonitor.isSimulationEnabled();
        const bool canRead = simulating || batteryMonitor.isInitialized();

        if (canRead && batteryMonitor.updateMeasurements()) {
            latestBatteryMeasurements = batteryMonitor.getMeasurements();
            batteryReadingValid = true;
            batteryReadingMilliseconds = static_cast<std::uint32_t>(pdTICKS_TO_MS(nowTick));

            if (batteryMonitor.isStateOfChargeValid()) {
                (void)batteryMonitor.updateStateOfCharge(deltaSeconds);
            }

            ++cyclesSincePersist;
            if (cyclesSincePersist >= 60U) {
                (void)batteryMonitor.persistStateOfCharge();
                cyclesSincePersist = 0U;
            }
        } else {
            batteryReadingValid = false;
        }

        const bool alertOnHardwareFailureEdge =
            !simulating && batteryReadingValid != previousHardwareBatteryReadingValid;
        const bool hardwareReadingNowValid = batteryReadingValid;
        previousHardwareBatteryReadingValid = batteryReadingValid;

        xSemaphoreGive(stateMutex);

        if (alertOnHardwareFailureEdge) {
            mqttManager.publishAlert(
                "BATTERY_SENSOR",
                hardwareReadingNowValid ? "info" : "critical",
                hardwareReadingNowValid
                    ? "INA219 battery sensor reading resumed"
                    : "INA219 battery sensor stopped responding");
        }
    }
}

void applyStoredLoadSettings()
{
    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const auto* planningNode = registry.getNode(nodeIndex);
        if (planningNode == nullptr) continue;

        for (std::size_t loadIndex = 0U; loadIndex < planningNode->node.getNumberOfLoads(); ++loadIndex) {
            const Load* current = planningNode->node.getLoad(loadIndex);
            if (current == nullptr) continue;

            Load* mutableLoad = registry.findMutableLoad(current->getMacAddress(), current->getRelayPin());
            if (mutableLoad != nullptr) (void)centralLoadConfigurationStore.applyToLoad(*mutableLoad);
        }
    }
}

void buildLoadFilter(LoadFilter& filter, std::uint32_t nowMilliseconds)
{
    filter.reset();

    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const auto* planningNode = registry.getNode(nodeIndex);
        if (planningNode == nullptr) continue;

        if (!planningNode->isCentralNode &&
            (!isNodeOnline(*planningNode, nowMilliseconds) ||
             !isCommissionedSmartNode(planningNode->node.getMacAddress()))) {
            continue;
        }

        for (std::size_t loadIndex = 0U; loadIndex < planningNode->node.getNumberOfLoads(); ++loadIndex) {
            const Load* load = planningNode->node.getLoad(loadIndex);
            if (load != nullptr) (void)filter.addLoad(*load);
        }
    }
}

float sumFixedOnPowerWatts(const LoadFilter& filter)
{
    float total = 0.0F;
    for (std::size_t index = 0U; index < filter.getNumberOfFixedOnLoads(); ++index) {
        const Load* load = filter.getFixedOnLoad(index);
        if (load != nullptr) total += load->getPowerRatingWatts();
    }
    return total;
}

void resetFixedRejectionReasons(const LoadFilter& filter)
{
    for (std::size_t index = 0U; index < filter.getNumberOfFixedOnLoads(); ++index) {
        const Load* load = filter.getFixedOnLoad(index);
        if (load == nullptr) continue;
        Load* mutableLoad = registry.findMutableLoad(load->getMacAddress(), load->getRelayPin());
        if (mutableLoad != nullptr) mutableLoad->setLastBestFirstRejectionReason(BEST_FIRST_NONE);
    }

    for (std::size_t index = 0U; index < filter.getNumberOfFixedOffLoads(); ++index) {
        const Load* load = filter.getFixedOffLoad(index);
        if (load == nullptr) continue;
        Load* mutableLoad = registry.findMutableLoad(load->getMacAddress(), load->getRelayPin());
        if (mutableLoad != nullptr) mutableLoad->setLastBestFirstRejectionReason(BEST_FIRST_NONE);
    }
}

void recordAutoRejectionReason(const LoadFilter& filter, std::uint8_t reason)
{
    for (std::size_t index = 0U; index < filter.getNumberOfAutoCandidateLoads(); ++index) {
        const Load* load = filter.getAutoCandidateLoad(index);
        if (load == nullptr) continue;
        Load* mutableLoad = registry.findMutableLoad(load->getMacAddress(), load->getRelayPin());
        if (mutableLoad != nullptr) mutableLoad->setLastBestFirstRejectionReason(reason);
    }
}

std::vector<const Load*> automaticCandidates(const LoadFilter& filter)
{
    std::vector<const Load*> result;
    result.reserve(filter.getNumberOfAutoCandidateLoads());

    for (std::size_t index = 0U; index < filter.getNumberOfAutoCandidateLoads(); ++index) {
        const Load* load = filter.getAutoCandidateLoad(index);
        if (load != nullptr) result.push_back(load);
    }
    return result;
}

std::vector<RelayCommandDispatcher::RelayTarget> buildRelayTargets(
    const LoadFilter& filter,
    const std::vector<const Load*>& selectedAutoLoads)
{
    (void)selectedAutoLoads;
    std::vector<RelayCommandDispatcher::RelayTarget> targets;

    auto append = [&targets](const Load* load) {
        if (load == nullptr) return;
        targets.push_back(RelayCommandDispatcher::RelayTarget{
            load->getMacAddress(), load->getRelayPin(), load->isOn()});
    };

    for (std::size_t i = 0U; i < filter.getNumberOfFixedOffLoads(); ++i) append(filter.getFixedOffLoad(i));
    for (std::size_t i = 0U; i < filter.getNumberOfAutoCandidateLoads(); ++i) append(filter.getAutoCandidateLoad(i));
    for (std::size_t i = 0U; i < filter.getNumberOfFixedOnLoads(); ++i) append(filter.getFixedOnLoad(i));
    return RelayCommandDispatcher::buildDispatchOrder(targets);
}

bool dispatchRelayTargetAndWait(const RelayCommandDispatcher::RelayTarget& target)
{
    const Load::MacAddress localMac = communication.getLocalMacAddress();

    if (target.nodeMacAddress == localMac) {
        const bool success = relays.setRelayState(target.relayPin, target.desiredOn);
        if (!success && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) == pdTRUE) {
            ++relayCommandErrorCount;
            xSemaphoreGive(stateMutex);
        }
        return success;
    }

    Load::MacAddress nextHop{};
    std::uint32_t commandId = 0U;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) != pdTRUE) return false;
    const bool routeFound = routeToSmartNode(target.nodeMacAddress, nextHop);
    if (routeFound) commandId = relayCommandDispatcher.beginCommand(target);
    xSemaphoreGive(stateMutex);

    if (!routeFound || commandId == 0U) {
        ++relayCommandErrorCount;
        return false;
    }

    RelayCommandPacket packet{};
    packet.relayPin = target.relayPin;
    packet.desiredState = static_cast<std::uint8_t>(target.desiredOn ? RelayCommandState::ON : RelayCommandState::OFF);
    packet.commandId = commandId;

    if (!communication.sendTo(
            nextHop,
            target.nodeMacAddress,
            EspNowCommunication::MessageType::RELAY_COMMAND,
            packet)) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) == pdTRUE) {
            (void)relayCommandDispatcher.completeCommand(commandId, false);
            ++relayCommandErrorCount;
            xSemaphoreGive(stateMutex);
        }
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(CentralNodeConfig::RELAY_COMMAND_ACK_TIMEOUT_MILLISECONDS);

    while (xTaskGetTickCount() < deadline) {
        bool pending = true;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100U)) == pdTRUE) {
            pending = relayCommandDispatcher.isPending(commandId);
            xSemaphoreGive(stateMutex);
        }
        if (!pending) {
            bool success = false;
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100U)) == pdTRUE) {
                success = relayCommandDispatcher.wasCommandSuccessful(commandId);
                xSemaphoreGive(stateMutex);
            }
            return success;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) == pdTRUE) {
        if (relayCommandDispatcher.completeCommand(commandId, false)) ++relayCommandErrorCount;
        xSemaphoreGive(stateMutex);
    }
    return false;
}

SystemStateInputs makeSystemStateInputs()
{
    SystemStateInputs inputs{};
    const auto& configuration = centralConfigurationStore.getConfiguration();
    const auto& planning = configuration.powerPlanning;

    inputs.batterySensorConfigured = configuration.batterySensor.ina219Configured;
    inputs.batteryNominalVoltageVolts = configuration.batterySensor.batteryMetadataConfigured
        ? configuration.batterySensor.nominalVoltageVolts
        : 0.0F;
    inputs.batteryCapacityAmpHours = configuration.batterySensor.batteryMetadataConfigured
        ? configuration.batterySensor.batteryCapacityAmpHours
        : 0.0F;

    const float fullEnergyWh =
        inputs.batteryCapacityAmpHours * inputs.batteryNominalVoltageVolts;
    inputs.batteryRatedEnergyWattHours = fullEnergyWh;

    inputs.stateOfChargePercent = batteryMonitor.getStateOfChargePercent();
    inputs.stateOfChargeValid = batteryMonitor.isStateOfChargeValid();
    inputs.stateOfChargeSourceText = stateOfChargeSourceText(batteryMonitor.getStateOfChargeSource());
    inputs.batteryStoredEnergyWattHours = inputs.stateOfChargeValid
        ? fullEnergyWh * (inputs.stateOfChargePercent / 100.0F)
        : 0.0F;

    const bool reserveKnown = inputs.stateOfChargeValid && planning.configured;
    inputs.batteryUsableEnergyWattHours = reserveKnown
        ? fullEnergyWh *
            (std::max(0.0F, inputs.stateOfChargePercent - planning.minimumStateOfChargePercent) / 100.0F)
        : 0.0F;
    inputs.batteryVoltageVolts = latestBatteryMeasurements.voltageVolts;
    inputs.batteryCurrentAmps = latestBatteryMeasurements.currentAmps;
    inputs.P_measured = latestBatteryMeasurements.powerWatts;
    inputs.batteryMeasurementSourceText = measurementSourceText(batteryMonitor.getMeasurementSource());
    inputs.batteryReserveReached = reserveKnown &&
        inputs.stateOfChargePercent <= planning.minimumStateOfChargePercent;

    inputs.requiredRuntimeConfigured = planning.configured && planning.requiredRuntimeHours > 0.0F;
    inputs.requiredRuntimeHours = planning.requiredRuntimeHours;
    inputs.remainingRuntimeHours = batteryMonitor.getRemainingRequiredRuntimeHours();
    inputs.runtimeEstimateValid = reserveKnown && inputs.P_measured > 0.0F;
    inputs.estimatedRuntimeHours = inputs.runtimeEstimateValid
        ? inputs.batteryUsableEnergyWattHours / inputs.P_measured
        : 0.0F;
    inputs.requiredRuntimeAchievable =
        !latestPlanningSnapshot.budgetValid || latestPlanningSnapshot.budget.requiredRuntimeAchievable;

    if (latestPlanningSnapshot.budgetValid) {
        inputs.P_budget = latestPlanningSnapshot.budget.P_budget;
        inputs.P_reserve = latestPlanningSnapshot.budget.P_reserve;
        inputs.P_fixed = latestPlanningSnapshot.budget.P_fixed;
        inputs.P_auto_available = latestPlanningSnapshot.budget.P_auto_available;
        inputs.P_auto = latestPlanningSnapshot.budget.P_auto;
        inputs.P_remaining = latestPlanningSnapshot.budget.P_remaining;
    }

    inputs.wifiConnected = wifiManager.isConnected();
    inputs.wifiStateText = wifiStateText(wifiManager.getState());
    inputs.mqttConnected = mqttManager.isConnected();
    inputs.currentTimeValid = currentTimeProvider.isCurrentTimeValid();
    inputs.currentTimeSourceText = timeSourceText(currentTimeProvider.getCurrentTimeSource());
    inputs.lastOptimizationEpochSeconds = lastOptimizationEpochSeconds;
    inputs.pinCommandErrorCount = relayCommandErrorCount;
    return inputs;
}

void publishCurrentState()
{
    if (!mqttManager.isConnected()) return;

    const std::uint32_t now = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    (void)mqttManager.publishStatus();

    SystemStateInputs systemInputs{};
    std::string treeJson;
    std::string loadsJson;
    std::string nodesJson;
    std::string configNodesJson;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) return;

    applyStoredLoadSettings();
    systemInputs = makeSystemStateInputs();
    treeJson = TopologyTree::buildTreeJson(
        registry, commissioningRegistry, CentralNodeConfig::MQTT_SCHEMA_VERSION,
        now, CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS);
    loadsJson = TopologyTree::buildLoadsJson(registry, CentralNodeConfig::MQTT_SCHEMA_VERSION);
    nodesJson = NodeRegistryJson::buildStateNodesJson(
        commissioningRegistry, registry, CentralNodeConfig::MQTT_SCHEMA_VERSION,
        now, CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS);
    configNodesJson = NodeRegistryJson::buildConfigNodesJson(
        commissioningRegistry, registry, CentralNodeConfig::MQTT_SCHEMA_VERSION);

    xSemaphoreGive(stateMutex);

    if (systemInputs.batteryReserveReached != previousBatteryReserveReached) {
        mqttManager.publishAlert(
            "BATTERY_RESERVE",
            systemInputs.batteryReserveReached ? "critical" : "info",
            systemInputs.batteryReserveReached
                ? "battery state of charge reached the configured reserve"
                : "battery state of charge recovered above the configured reserve");
        previousBatteryReserveReached = systemInputs.batteryReserveReached;
    }

    if (systemInputs.requiredRuntimeAchievable != previousRequiredRuntimeAchievable) {
        mqttManager.publishAlert(
            "RUNTIME_TARGET",
            systemInputs.requiredRuntimeAchievable ? "info" : "warning",
            systemInputs.requiredRuntimeAchievable
                ? "required runtime target is achievable again"
                : "required runtime target is no longer achievable at the current load");
        previousRequiredRuntimeAchievable = systemInputs.requiredRuntimeAchievable;
    }

    const bool measuredPowerExceededBudget =
        latestPlanningSnapshot.budgetValid && batteryReadingValid &&
        systemInputs.P_measured > systemInputs.P_budget;
    if (measuredPowerExceededBudget != previousMeasuredPowerExceededBudget) {
        mqttManager.publishAlert(
            "MEASURED_POWER_BUDGET",
            measuredPowerExceededBudget ? "warning" : "info",
            measuredPowerExceededBudget
                ? "measured power is above the configured P_budget"
                : "measured power returned within the configured P_budget");
        previousMeasuredPowerExceededBudget = measuredPowerExceededBudget;
    }

    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const auto* planningNode = registry.getNode(nodeIndex);
        if (planningNode == nullptr || planningNode->isCentralNode) continue;

        const bool onlineNow = isNodeOnline(*planningNode, now);
        const Load::MacAddress nodeMac = planningNode->node.getMacAddress();

        NodeOnlineTracking* tracking = nullptr;
        for (NodeOnlineTracking& entry : nodeOnlineTracking) {
            if (entry.macAddress == nodeMac) {
                tracking = &entry;
                break;
            }
        }

        if (tracking == nullptr) {
            nodeOnlineTracking.push_back(NodeOnlineTracking{nodeMac, onlineNow});
            continue;
        }

        if (tracking->wasOnline != onlineNow) {
            char detail[64]{};
            std::snprintf(detail, sizeof(detail), "Smart Node %s",
                onlineNow ? "came back online" : "stopped reporting and is now offline");
            mqttManager.publishAlert("NODE_OFFLINE", onlineNow ? "info" : "warning", detail);
            tracking->wasOnline = onlineNow;
        }
    }

    (void)mqttManager.publish(
        MqttManager::TOPIC_STATE_SYSTEM,
        SystemStateJson::build(systemInputs, CentralNodeConfig::MQTT_SCHEMA_VERSION),
        0,
        true);
    (void)mqttManager.publish(MqttManager::TOPIC_STATE_TREE, treeJson, 0, true);
    (void)mqttManager.publish(MqttManager::TOPIC_STATE_LOADS, loadsJson, 0, true);
    (void)mqttManager.publish(MqttManager::TOPIC_STATE_NODES, nodesJson, 0, true);
    (void)mqttManager.publish(MqttManager::TOPIC_CONFIG_NODES, configNodesJson, 0, true);
}

bool schedulesEqual(const AutoSchedule& first, const AutoSchedule& second)
{
    return first.enabled == second.enabled && first.hour == second.hour && first.minute == second.minute;
}

bool updateAutoRuntimeState(Load& load, bool requestedOn)
{
    if (!load.isAuto()) return false;

    const LoadMode::Value newMode = requestedOn ? LoadMode::Auto::ON : LoadMode::Auto::OFF;
    const LoadConfigurationStore::ConfigurationEntry entry{
        load.getMacAddress(), load.getRelayPin(), load.getPriority(), newMode, load.getSchedule()};

    LoadConfigurationStore::ConfigurationEntry previous{};
    const bool hadPrevious = centralLoadConfigurationStore.findConfiguration(
        load.getMacAddress(), load.getRelayPin(), previous);

    const bool changed =
        !hadPrevious || previous.priority != entry.priority || previous.mode != entry.mode ||
        !schedulesEqual(previous.schedule, entry.schedule) || load.getMode() != newMode;

    load.setMode(newMode);
    (void)centralLoadConfigurationStore.setConfiguration(entry);
    return changed;
}

void dispatchRelayOrderWithOnStagger(
    const std::vector<RelayCommandDispatcher::RelayTarget>& dispatchOrder)
{
    bool firstOn = true;
    for (const auto& target : dispatchOrder) {
        if (target.desiredOn) {
            vTaskDelay(pdMS_TO_TICKS(
                firstOn
                    ? CentralNodeConfig::RELAY_ON_FIRST_DELAY_MILLISECONDS
                    : CentralNodeConfig::RELAY_ON_BETWEEN_DELAY_MILLISECONDS));
            firstOn = false;
        }
        (void)dispatchRelayTargetAndWait(target);
    }
}

void runOptimizationCycle(bool printDashboard)
{
    std::vector<RelayCommandDispatcher::RelayTarget> dispatchOrder;
    std::vector<const Load*> selectedAutoLoads;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(1000U)) != pdTRUE) return;

    applyStoredLoadSettings();
    const std::uint32_t now = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));

    LoadFilter filter;
    buildLoadFilter(filter, now);
    resetFixedRejectionReasons(filter);

    PlanningSnapshot snapshot{};
    snapshot.fixedOnCount = filter.getNumberOfFixedOnLoads();
    snapshot.fixedOffCount = filter.getNumberOfFixedOffLoads();
    snapshot.automaticCandidateCount = filter.getNumberOfAutoCandidateLoads();

    const float P_fixed = sumFixedOnPowerWatts(filter);
    const auto& configuration = centralConfigurationStore.getConfiguration();
    const auto& planning = configuration.powerPlanning;

    const float remainingRequiredRuntimeHours = computeRemainingRequiredRuntimeHours(
        planning.requiredRuntimeHours,
        currentTimeProvider.isCurrentTimeValid(),
        static_cast<std::int64_t>(std::time(nullptr)));

    const bool runtimeTargetActive = planning.requiredRuntimeHours > 0.0F;
    const bool runtimeInputsReady = !runtimeTargetActive ||
        (configuration.batterySensor.batteryMetadataConfigured && batteryMonitor.isStateOfChargeValid());

    if (planning.configured && runtimeInputsReady &&
        batteryMonitor.setPowerBudgetWatts(planning.P_budget) &&
        batteryMonitor.setPowerReserveWatts(planning.P_reserve) &&
        batteryMonitor.setFixedPowerWatts(P_fixed) &&
        batteryMonitor.setAutoPowerWatts(0.0F) &&
        batteryMonitor.setRemainingRequiredRuntimeHours(remainingRequiredRuntimeHours) &&
        batteryMonitor.updatePowerBudget()) {

        snapshot.budgetValid = true;
        snapshot.budget = batteryMonitor.getPowerBudget();
        const std::vector<const Load*> autoLoads = automaticCandidates(filter);

        BestFirstSearch search(
            snapshot.budget.P_auto_available,
            autoLoads,
            currentTimeProvider);
        selectedAutoLoads = search.getBestCombination();

        bool autoRuntimeStateChanged = false;
        float P_auto = 0.0F;

        for (const Load* load : autoLoads) {
            if (load == nullptr) continue;

            Load* mutableLoad = registry.findMutableLoad(load->getMacAddress(), load->getRelayPin());
            if (mutableLoad == nullptr) continue;

            const bool selected = containsLoadPointer(selectedAutoLoads, load);
            mutableLoad->setLastBestFirstRejectionReason(
                selected ? BEST_FIRST_NONE : BEST_FIRST_POWER_BUDGET_EXCEEDED);
            autoRuntimeStateChanged = updateAutoRuntimeState(*mutableLoad, selected) || autoRuntimeStateChanged;

            if (selected) {
                P_auto += load->getPowerRatingWatts();
                ++snapshot.automaticSelectedCount;
            }
        }
        if (autoRuntimeStateChanged) (void)centralLoadConfigurationStore.persist();

        if (batteryMonitor.setAutoPowerWatts(P_auto) && batteryMonitor.updatePowerBudget()) {
            snapshot.budget = batteryMonitor.getPowerBudget();
        } else {
            snapshot.budgetValid = false;
        }
    } else {
        recordAutoRejectionReason(filter, BEST_FIRST_POWER_BUDGET_EXCEEDED);

        bool autoRuntimeStateChanged = false;
        for (std::size_t i = 0U; i < filter.getNumberOfAutoCandidateLoads(); ++i) {
            const Load* load = filter.getAutoCandidateLoad(i);
            if (load == nullptr) continue;
            Load* mutableLoad = registry.findMutableLoad(load->getMacAddress(), load->getRelayPin());
            if (mutableLoad != nullptr) {
                autoRuntimeStateChanged = updateAutoRuntimeState(*mutableLoad, false) || autoRuntimeStateChanged;
            }
        }
        if (autoRuntimeStateChanged) (void)centralLoadConfigurationStore.persist();
    }

    latestPlanningSnapshot = snapshot;
    if (currentTimeProvider.isCurrentTimeValid()) {
        lastOptimizationEpochSeconds = static_cast<std::int64_t>(std::time(nullptr));
    }

    dispatchOrder = buildRelayTargets(filter, selectedAutoLoads);
    xSemaphoreGive(stateMutex);

    dispatchRelayOrderWithOnStagger(dispatchOrder);

    if (printDashboard) {
        std::printf("\n============================================================\n");
        std::printf("                    KILOWATTS DASHBOARD\n");
        std::printf("============================================================\n");
        std::printf("Measurement source   : %s\n", measurementSourceText(batteryMonitor.getMeasurementSource()));
        if (batteryReadingValid) {
            std::printf("Voltage              : %.2f V\n", static_cast<double>(latestBatteryMeasurements.voltageVolts));
            std::printf("Current              : %.2f A\n", static_cast<double>(latestBatteryMeasurements.currentAmps));
            std::printf("P_measured           : %.2f W\n", static_cast<double>(latestBatteryMeasurements.powerWatts));
        } else {
            std::printf("P_measured           : NOT AVAILABLE\n");
        }
        if (batteryMonitor.isStateOfChargeValid()) {
            std::printf("Battery SoC          : %.1f %%\n", static_cast<double>(batteryMonitor.getStateOfChargePercent()));
        } else {
            std::printf("Battery SoC          : NOT AVAILABLE\n");
        }
        std::printf("------------------------------------------------------------\n");
        if (snapshot.budgetValid) {
            std::printf("P_budget             : %.2f W\n", static_cast<double>(snapshot.budget.P_budget));
            std::printf("P_reserve            : %.2f W\n", static_cast<double>(snapshot.budget.P_reserve));
            std::printf("P_fixed              : %.2f W\n", static_cast<double>(snapshot.budget.P_fixed));
            std::printf("P_auto_available     : %.2f W\n", static_cast<double>(snapshot.budget.P_auto_available));
            std::printf("P_auto               : %.2f W\n", static_cast<double>(snapshot.budget.P_auto));
            std::printf("P_remaining          : %.2f W\n", static_cast<double>(snapshot.budget.P_remaining));
            if (snapshot.budget.runtimeBudgetActive) {
                std::printf("Runtime              : %.2f h remaining | %s\n",
                    static_cast<double>(batteryMonitor.getRemainingRequiredRuntimeHours()),
                    snapshot.budget.requiredRuntimeAchievable ? "ACHIEVABLE" : "NOT ACHIEVABLE");
            }
        } else {
            std::printf("Power planning       : NOT AVAILABLE\n");
        }
        std::printf("Fixed ON / OFF       : %u / %u\n",
            static_cast<unsigned int>(snapshot.fixedOnCount),
            static_cast<unsigned int>(snapshot.fixedOffCount));
        std::printf("Automatic loads      : %u / %u\n",
            static_cast<unsigned int>(snapshot.automaticSelectedCount),
            static_cast<unsigned int>(snapshot.automaticCandidateCount));
        std::printf("============================================================\n\n");
    }

    publishCurrentState();
}

void checkMqttStartTrigger()
{
    if (mqttStarted || !wifiManager.isConnected()) return;

    MqttCredentialsStore::Credentials credentials{};
    if (!mqttCredentialsStore.load(credentials)) {
        if (!mqttCredentialsUnavailableLogged) {
            ESP_LOGW(TAG, "MQTT credentials not configured");
            mqttCredentialsUnavailableLogged = true;
        }
        return;
    }
    mqttCredentialsUnavailableLogged = false;

    if (!mqttManager.begin(MqttManager::Credentials{
            credentials.host,
            credentials.port,
            credentials.useTls,
            credentials.username,
            credentials.password})) {
        if (!mqttBrokerStartupFailedLogged) {
            ESP_LOGW(TAG, "MQTT start failed");
            mqttBrokerStartupFailedLogged = true;
        }
        return;
    }

    mqttBrokerStartupFailedLogged = false;
    mqttStarted = true;
}

void optimizationTask(void* parameter)
{
    (void)parameter;
    vTaskDelay(pdMS_TO_TICKS(5000U));

    while (true) {
        checkMqttStartTrigger();
        runOptimizationCycle(false);
        (void)xSemaphoreTake(
            optimizationTriggerSemaphore,
            pdMS_TO_TICKS(getOptimizerIntervalMilliseconds()));
    }
}

void checkWiFiProvisioningTrigger()
{
    if (wifiManager.isConnected()) {
        if (wifiProvisioningPortal.isActive()) wifiProvisioningPortal.end();
        return;
    }

    if (!wifiCredentialsStore.isConfigured() && !wifiProvisioningPortal.isActive()) {
        (void)wifiProvisioningPortal.begin(communication.getChannel());
    }
}

void watchdogTask(void* parameter)
{
    (void)parameter;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CentralNodeConfig::WATCHDOG_PERIOD_MILLISECONDS));
        checkWiFiProvisioningTrigger();
        checkMqttStartTrigger();

        const Load::MacAddress localMac = communication.getLocalMacAddress();
        NodeCommissioningRegistry::Diagnostics diagnostics{};
        diagnostics.freeHeapBytes = chipInfo.getFreeHeapBytes();
        diagnostics.minFreeHeapBytes = chipInfo.getMinFreeHeapBytes();
        diagnostics.flashSizeBytes = chipInfo.getFlashSizeBytes();
        diagnostics.psramSizeBytes = chipInfo.getPsramSizeBytes();
        diagnostics.siliconRevision = static_cast<std::uint16_t>(chipInfo.getSiliconRevision());
        diagnostics.cpuCores = static_cast<std::uint8_t>(chipInfo.getCpuCores());
        diagnostics.cpuFrequencyMhz = chipInfo.getCpuFrequencyMhz();
        diagnostics.temperatureAvailable = chipInfo.getTemperatureCelsius(diagnostics.temperatureCelsius);
        chipInfo.getResetReasonText(diagnostics.resetReason, sizeof(diagnostics.resetReason));

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) == pdTRUE) {
            (void)commissioningRegistry.updateDiagnostics(localMac, diagnostics);
            xSemaphoreGive(stateMutex);
        }
        publishCurrentState();
    }
}

void handleRelayAcknowledgement(
    const Load::MacAddress& originMac,
    const RelayCommandAcknowledgementPacket& acknowledgement)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) != pdTRUE) return;

    const auto* pending = relayCommandDispatcher.findPendingCommand(acknowledgement.commandId);
    if (pending != nullptr && pending->nodeMacAddress == originMac &&
        pending->relayPin == acknowledgement.relayPin &&
        pending->desiredOn == (acknowledgement.requestedState == static_cast<std::uint8_t>(RelayCommandState::ON))) {
        const bool success = acknowledgement.success != 0U;
        if (!success) ++relayCommandErrorCount;
        (void)relayCommandDispatcher.completeCommand(acknowledgement.commandId, success);
    }
    xSemaphoreGive(stateMutex);
}

void handleHardwareConfigurationAcknowledgement(
    const Load::MacAddress& originMac,
    const ConfigureLoadAcknowledgementPacket& acknowledgement)
{
    bool known = false;
    bool remove = false;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) == pdTRUE) {
        PendingHardwareCommand* pending = findPendingHardwareCommand(acknowledgement.commandId);
        if (pending != nullptr && pending->nodeMacAddress == originMac && pending->relayPin == acknowledgement.relayPin) {
            known = true;
            remove = pending->remove;
            if (acknowledgement.success != 0U && remove) {
                (void)registry.removeLoad(originMac, acknowledgement.relayPin);
                (void)centralLoadConfigurationStore.removeConfiguration(originMac, acknowledgement.relayPin);
                (void)centralLoadConfigurationStore.persist();
            }
            removePendingHardwareCommand(acknowledgement.commandId);
        }
        xSemaphoreGive(stateMutex);
    }

    if (!known) return;

    char target[18]{};
    formatMacAddressText(target, sizeof(target), originMac);
    const bool success = acknowledgement.success != 0U;

    mqttManager.publishAcknowledgement(
        acknowledgement.commandId,
        remove ? "REMOVE_LOAD" : "CONFIGURE_LOAD",
        success ? AckStatus::APPLIED : AckStatus::FAILED,
        success ? "applied" : hardwareConfigurationFailureText(
            static_cast<HardwareConfigurationFailureReason>(acknowledgement.failureReason)),
        target);

    if (success) {
        mqttManager.publishEvent(remove ? "LOAD_REMOVED" : "LOAD_CONFIGURED", target, nullptr);
        xSemaphoreGive(optimizationTriggerSemaphore);
    }
}

void handleCommissionAcknowledgement(
    const Load::MacAddress& originMac,
    const CommissionAckPacket& acknowledgement)
{
    bool applied = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) == pdTRUE) {
        const NodeLifecycleState state = static_cast<NodeLifecycleState>(acknowledgement.resultingState);
        applied = commissioningRegistry.applyCommissionResult(
            originMac, acknowledgement.commandId, acknowledgement.success != 0U, state);
        if (applied) (void)commissioningRegistry.persist();
        xSemaphoreGive(stateMutex);
    }

    if (applied) {
        char target[18]{};
        formatMacAddressText(target, sizeof(target), originMac);
        mqttManager.publishAcknowledgement(
            acknowledgement.commandId,
            "COMMISSION_NODE",
            acknowledgement.success != 0U ? AckStatus::APPLIED : AckStatus::FAILED,
            acknowledgement.success != 0U ? "applied" : "Node rejected commissioning",
            target);
        xSemaphoreGive(optimizationTriggerSemaphore);
    }
}

void handleIdentityReport(
    const Load::MacAddress& originMac,
    const IdentityReportPacket& report,
    std::uint32_t nowMilliseconds)
{
    if (report.role > static_cast<std::uint8_t>(NodeRole::SMART) ||
        report.relayCapabilityCount > MAX_RELAY_GPIO_CAPABILITIES) return;

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
    copyText(diagnostics.resetReason, report.resetReason);

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) != pdTRUE) return;

    (void)commissioningRegistry.recordDiscovered(
        originMac,
        static_cast<NodeRole>(report.role),
        report.firmwareVersion,
        report.chipModel,
        report.relayPins.data(),
        report.relayCapabilityCount,
        nowMilliseconds);
    (void)commissioningRegistry.updateDiagnostics(originMac, diagnostics);
    xSemaphoreGive(stateMutex);
}

void espNowCommunicationTask(void* parameter)
{
    (void)parameter;
    const Load::MacAddress localMac = communication.getLocalMacAddress();

    while (true) {
        EspNowCommunication::ReceivedMessage received{};
        if (!communication.receive(received, 500U)) continue;

        const bool localDestination = communication.isMessageForThisNode(received.message);
        const auto type = received.message.header.messageType;
        const Load::MacAddress originMac = received.message.header.originMacAddress;

        if (!localDestination) {
            Load::MacAddress nextHop{};
            bool found = false;
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200U)) == pdTRUE) {
                found = findNextHopFromCentral(localMac, received.message.header.destinationMacAddress, nextHop);
                xSemaphoreGive(stateMutex);
            }
            if (found) (void)communication.forwardMessageTo(nextHop, received.message);
            continue;
        }

        if (type == EspNowCommunication::MessageType::NODE_REPORT &&
            received.message.header.payloadLength == sizeof(NodeReportPacket)) {
            NodeReportPacket report{};
            std::memcpy(&report, received.message.payload.data(), sizeof(report));

            bool accepted = report.nodeMacAddress == originMac;
            if (accepted && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) == pdTRUE) {
                registry.applyNodeReport(
                    report,
                    static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
                const auto* node = registry.findNodeByMacAddress(originMac);
                if (node != nullptr) {
                    for (std::size_t i = 0U; i < node->node.getNumberOfLoads(); ++i) {
                        const Load* load = node->node.getLoad(i);
                        if (load == nullptr) continue;
                        Load* mutableLoad = registry.findMutableLoad(originMac, load->getRelayPin());
                        if (mutableLoad != nullptr) (void)centralLoadConfigurationStore.applyToLoad(*mutableLoad);
                    }
                }
                xSemaphoreGive(stateMutex);
            }

            NodeReportAcknowledgementPacket acknowledgement{};
            acknowledgement.reportSequenceId = report.reportSequenceId;
            acknowledgement.status = static_cast<std::uint8_t>(
                accepted ? NodeReportAckStatus::ACCEPTED : NodeReportAckStatus::REJECTED);
            (void)communication.sendTo(
                received.senderMacAddress,
                originMac,
                EspNowCommunication::MessageType::NODE_REPORT_ACK,
                acknowledgement);
            if (accepted) xSemaphoreGive(optimizationTriggerSemaphore);
            continue;
        }

        if (type == EspNowCommunication::MessageType::IDENTITY_REPORT &&
            received.message.header.payloadLength == sizeof(IdentityReportPacket)) {
            IdentityReportPacket report{};
            std::memcpy(&report, received.message.payload.data(), sizeof(report));
            handleIdentityReport(
                originMac,
                report,
                static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
            continue;
        }

        if (type == EspNowCommunication::MessageType::COMMISSION_ACK &&
            received.message.header.payloadLength == sizeof(CommissionAckPacket)) {
            CommissionAckPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));
            handleCommissionAcknowledgement(originMac, acknowledgement);
            continue;
        }

        if (type == EspNowCommunication::MessageType::ACKNOWLEDGEMENT &&
            received.message.header.payloadLength == sizeof(RelayCommandAcknowledgementPacket)) {
            RelayCommandAcknowledgementPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));
            handleRelayAcknowledgement(originMac, acknowledgement);
            continue;
        }

        if (type == EspNowCommunication::MessageType::CONFIGURE_LOAD_ACK &&
            received.message.header.payloadLength == sizeof(ConfigureLoadAcknowledgementPacket)) {
            ConfigureLoadAcknowledgementPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));
            handleHardwareConfigurationAcknowledgement(originMac, acknowledgement);
            continue;
        }

        if (type == EspNowCommunication::MessageType::DECOMMISSION_ACK &&
            received.message.header.payloadLength == sizeof(DecommissionAckPacket)) {
            DecommissionAckPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));
            char target[18]{};
            formatMacAddressText(target, sizeof(target), originMac);
            mqttManager.publishAcknowledgement(
                acknowledgement.commandId,
                "DECOMMISSION_NODE",
                acknowledgement.success != 0U ? AckStatus::APPLIED : AckStatus::FAILED,
                acknowledgement.success != 0U ? "applied" : "Node rejected decommission",
                target);
            continue;
        }

        if (type == EspNowCommunication::MessageType::FACTORY_RESET_ACK &&
            received.message.header.payloadLength == sizeof(FactoryResetAckPacket)) {
            FactoryResetAckPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));
            char target[18]{};
            formatMacAddressText(target, sizeof(target), originMac);
            mqttManager.publishAcknowledgement(
                acknowledgement.commandId,
                "FACTORY_RESET_NODE",
                acknowledgement.success != 0U ? AckStatus::APPLIED : AckStatus::FAILED,
                acknowledgement.success != 0U ? "applied" : "Node reset failed",
                target);
        }
    }
}

CommandResult handleLoadCommand(void* context, const LoadCommandRequest& request)
{
    (void)context;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return commandResult(false, false, "state is busy");
    }

    Load* load = registry.findMutableLoad(request.nodeMacAddress, request.relayPin);
    const Load::MacAddress localMac = communication.getLocalMacAddress();
    const bool validOwner = request.nodeMacAddress == localMac || isCommissionedSmartNode(request.nodeMacAddress);

    if (load == nullptr || !validOwner) {
        xSemaphoreGive(stateMutex);
        return commandResult(false, false, "unknown or unavailable Load");
    }

    std::uint16_t priority = load->getPriority();
    LoadMode::Value mode = load->getMode();
    AutoSchedule schedule = load->getSchedule();

    if (request.hasPriority) priority = request.priority;
    if (request.hasMode) mode = request.mode;
    if (request.hasSchedule) schedule = request.schedule;
    if (mode == LoadMode::Fixed::ON || mode == LoadMode::Fixed::OFF) schedule = AutoSchedule{false, 0U, 0U};

    LoadConfigurationStore::ConfigurationEntry entry{
        request.nodeMacAddress, request.relayPin, priority, mode, schedule};

    const bool saved =
        centralLoadConfigurationStore.setConfiguration(entry) &&
        centralLoadConfigurationStore.applyToLoad(*load) &&
        centralLoadConfigurationStore.persist();

    xSemaphoreGive(stateMutex);
    if (!saved) return commandResult(false, false, "could not save Load settings");
    xSemaphoreGive(optimizationTriggerSemaphore);
    return commandResult(true, true, "applied");
}

CommandResult handleSystemCommand(
    void* context,
    const kilowatts::SystemCommandRequest& request)
{
    (void)context;

    if (request.action == SystemCommandAction::REQUEST_OPTIMIZATION_CYCLE) {
        xSemaphoreGive(optimizationTriggerSemaphore);
        return commandResult(true, true, "optimization requested");
    }

    if (request.action == SystemCommandAction::REPORT_OPTIMIZER_INTERVAL) {
        char text[96]{};
        std::snprintf(text, sizeof(text), "optimizer interval is %lu seconds",
            static_cast<unsigned long>(getOptimizerIntervalSeconds()));
        return commandResult(true, true, text);
    }

    if (request.action == SystemCommandAction::SET_OPTIMIZER_INTERVAL) {
        if (!request.hasOptimizerIntervalSeconds ||
            !isValidOptimizerIntervalSeconds(request.optimizerIntervalSeconds)) {
            char text[128]{};
            std::snprintf(text, sizeof(text), "optimizer interval must be %lu..%lu seconds",
                static_cast<unsigned long>(optimizerMinIntervalSeconds()),
                static_cast<unsigned long>(optimizerMaxIntervalSeconds()));
            return commandResult(false, false, text);
        }

        if (!saveOptimizerIntervalSeconds(request.optimizerIntervalSeconds)) {
            return commandResult(false, false, "could not save optimizer interval");
        }
        xSemaphoreGive(optimizationTriggerSemaphore);

        char text[96]{};
        std::snprintf(text, sizeof(text), "optimizer interval set to %lu seconds",
            static_cast<unsigned long>(request.optimizerIntervalSeconds));
        return commandResult(true, true, text);
    }

    if (request.action == SystemCommandAction::REBOOT_CENTRAL) {
        scheduleCentralRestart(1500000ULL);
        return commandResult(true, true, "Central restarting in 1.5 seconds");
    }

    if (request.action == SystemCommandAction::FACTORY_RESET_CENTRAL) {
        if (std::strcmp(request.confirmText, "RESET") != 0 &&
            std::strcmp(request.confirmText, "FACTORY_RESET_CONFIRMED") != 0) {
            return commandResult(false, false, "reset confirmation required");
        }
        const esp_err_t erased = nvs_flash_erase();
        if (erased != ESP_OK) return commandResult(false, false, "Central NVS erase failed");
        (void)nvs_flash_init();
        vTaskDelay(pdMS_TO_TICKS(100U));
        esp_restart();
        return commandResult(true, true, "restarting");
    }

    if (request.action == SystemCommandAction::FACTORY_RESET_NODE) {
        if (!request.hasTargetNodeMacAddress) return commandResult(false, false, "targetNodeMac required");

        Load::MacAddress nextHop{};
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }
        const bool routeFound = routeToSmartNode(request.targetNodeMacAddress, nextHop);
        xSemaphoreGive(stateMutex);
        if (!routeFound) return commandResult(false, false, "no ESP-NOW route to target Node");

        FactoryResetCommandPacket packet{};
        packet.commandId = request.commandId;
        packet.confirmToken = FACTORY_RESET_CONFIRM_TOKEN;
        const bool sent = communication.sendTo(
            nextHop,
            request.targetNodeMacAddress,
            EspNowCommunication::MessageType::FACTORY_RESET_COMMAND,
            packet);
        return commandResult(sent, false, sent ? "factory reset dispatched" : "ESP-NOW send failed");
    }

    return commandResult(false, false, "unsupported system command");
}

CommandResult handleConfigCommand(void* context, const ConfigCommandRequest& request)
{
    (void)context;
    const Load::MacAddress localMac = communication.getLocalMacAddress();

    if (request.action == ConfigCommandAction::CONFIGURE_BATTERY_SENSOR) {
        if (!request.hasBatterySensorConfiguration || request.nodeMacAddress != localMac) {
            return commandResult(false, false, "battery sensor configuration targets Central only");
        }

        const CentralConfigurationStore::BatterySensorConfiguration configuration{
            true,
            true,
            request.batteryShuntResistanceOhms,
            request.batteryMaximumExpectedCurrentAmps,
            request.batteryEmaAlpha,
            request.batteryCapacityAmpHours,
            request.batteryInitialStateOfChargePercent,
            request.batteryNominalVoltageVolts};

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }
        if (!CentralConfigurationStore::isValidBatterySensor(configuration)) {
            xSemaphoreGive(stateMutex);
            return commandResult(false, false, "invalid battery monitor configuration");
        }

        const auto previous = centralConfigurationStore.getConfiguration().batterySensor;
        const bool stored =
            centralConfigurationStore.setBatterySensor(configuration) &&
            centralConfigurationStore.persist();

        bool monitorReady = false;
        if (stored) {
            monitorReady = applyBatteryConfiguration(
                configuration,
                previous.batteryMetadataConfigured);
            batteryReadingValid = false;
        }
        xSemaphoreGive(stateMutex);

        if (!stored) return commandResult(false, false, "could not persist battery monitor configuration");
        xSemaphoreGive(optimizationTriggerSemaphore);

        return commandResult(
            true,
            true,
            monitorReady ? "battery monitor configured" : "configuration saved; INA219 is not currently responding");
    }

    if (request.action == ConfigCommandAction::COMMISSION_NODE ||
        request.action == ConfigCommandAction::RENAME_NODE) {
        if (!request.hasFriendlyName || request.nodeMacAddress == localMac) {
            return commandResult(false, false, "valid Smart Node and friendlyName required");
        }

        Load::MacAddress nextHop{};
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }
        const bool known = commissioningRegistry.findByMac(request.nodeMacAddress) != nullptr;
        const bool routeFound = routeToSmartNode(request.nodeMacAddress, nextHop);
        const bool accepted = known && routeFound && commissioningRegistry.requestCommissioning(
            request.nodeMacAddress, request.friendlyName, request.commandId);
        if (accepted) (void)commissioningRegistry.persist();
        xSemaphoreGive(stateMutex);

        if (!accepted) {
            return commandResult(false, false, known ? "commissioning request rejected or no route" : "Node has not been discovered");
        }

        CommissionCommandPacket packet{};
        packet.commandId = request.commandId;
        copyText(packet.friendlyName, request.friendlyName);
        const bool sent = communication.sendTo(
            nextHop,
            request.nodeMacAddress,
            EspNowCommunication::MessageType::COMMISSION_COMMAND,
            packet);
        return commandResult(sent, false, sent ? "dispatched; awaiting Node confirmation" : "ESP-NOW send failed");
    }

    if (request.action == ConfigCommandAction::DECOMMISSION_NODE) {
        if (request.nodeMacAddress == localMac) return commandResult(false, false, "Central cannot decommission itself");

        Load::MacAddress nextHop{};
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }
        const bool routeFound = routeToSmartNode(request.nodeMacAddress, nextHop);
        const bool decommissioned = commissioningRegistry.decommission(request.nodeMacAddress);
        if (decommissioned) {
            (void)centralLoadConfigurationStore.persist();
            (void)commissioningRegistry.persist();
        }
        xSemaphoreGive(stateMutex);

        if (!decommissioned) return commandResult(false, false, "unknown Node or invalid lifecycle transition");

        if (routeFound) {
            DecommissionCommandPacket packet{};
            packet.commandId = request.commandId;
            (void)communication.sendTo(
                nextHop,
                request.nodeMacAddress,
                EspNowCommunication::MessageType::DECOMMISSION_COMMAND,
                packet);
        }
        xSemaphoreGive(optimizationTriggerSemaphore);
        return commandResult(true, !routeFound, routeFound ? "decommission dispatched" : "decommissioned at Central; Node is offline");
    }

    if (request.action == ConfigCommandAction::CONFIGURE_LOAD) {
        if (!request.hasLoadConfiguration) return commandResult(false, false, "loadConfiguration required");

        if (request.nodeMacAddress == localMac) {
            NodeLoadHardwareStore::LoadConfiguration configuration{};
            copyText(configuration.name, request.loadName);
            configuration.relayPin = request.relayPin;
            configuration.relayActiveHigh = request.relayActiveHigh;
            configuration.powerRatingWatts = request.powerRatingWatts;
            configuration.powerType = request.powerType;
            configuration.mode = request.mode;
            configuration.priority = request.priority;
            configuration.schedule = request.schedule;

            HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
                return commandResult(false, false, "state is busy");
            }

            const bool configured = centralLoadHardwareStore.configureNewLoad(
                configuration, relays, centralNode, reason);
            if (configured) {
                registry.addLocalCentralNode(
                    CentralNodeConfig::CENTRAL_NODE_NAME,
                    centralNode,
                    static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
            }
            xSemaphoreGive(stateMutex);

            if (!configured) return commandResult(false, false, hardwareConfigurationFailureText(reason));
            xSemaphoreGive(optimizationTriggerSemaphore);
            return commandResult(true, true, "Central Load configured");
        }

        Load::MacAddress nextHop{};
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }
        const bool allowed = isCommissionedSmartNode(request.nodeMacAddress);
        const bool routeFound = allowed && routeToSmartNode(request.nodeMacAddress, nextHop);
        xSemaphoreGive(stateMutex);

        if (!allowed || !routeFound) {
            return commandResult(false, false,
                allowed ? "no ESP-NOW route to Smart Node" : "Smart Node is not commissioned");
        }

        ConfigureLoadCommandPacket packet{};
        packet.commandId = request.commandId;
        copyText(packet.loadName, request.loadName);
        packet.relayPin = request.relayPin;
        packet.relayActiveHigh = request.relayActiveHigh ? 1U : 0U;
        packet.mode = static_cast<std::uint8_t>(request.mode);
        packet.powerType = static_cast<std::uint8_t>(request.powerType);
        packet.priority = request.priority;
        packet.powerRatingWatts = request.powerRatingWatts;
        packet.scheduleEnabled = request.schedule.enabled ? 1U : 0U;
        packet.scheduleHour = request.schedule.hour;
        packet.scheduleMinute = request.schedule.minute;

        const bool sent = communication.sendTo(
            nextHop,
            request.nodeMacAddress,
            EspNowCommunication::MessageType::CONFIGURE_LOAD_COMMAND,
            packet);
        return commandResult(sent, false, sent ? "configuration dispatched" : "ESP-NOW send failed");
    }

    if (request.action == ConfigCommandAction::CONFIGURE_POWER_PLANNING) {
        if (!request.hasPowerPlanningConfiguration || request.nodeMacAddress != localMac) {
            return commandResult(false, false, "power planning configuration targets Central only");
        }

        const CentralConfigurationStore::PowerPlanningConfiguration configuration{
            true,
            request.P_budget,
            request.P_reserve,
            request.minimumStateOfChargePercent,
            request.requiredRuntimeHours};

        if (!CentralConfigurationStore::isValidPowerPlanning(configuration)) {
            return commandResult(false, false, "invalid power planning configuration");
        }

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }

        const bool stored =
            centralConfigurationStore.setPowerPlanning(configuration) &&
            centralConfigurationStore.persist();

        if (stored) {
            const auto& battery = centralConfigurationStore.getConfiguration().batterySensor;
            if (battery.ina219Configured || battery.batteryMetadataConfigured) {
                (void)applyBatteryConfiguration(battery, true);
                batteryReadingValid = false;
            }
        }
        xSemaphoreGive(stateMutex);

        if (!stored) return commandResult(false, false, "could not persist power planning configuration");
        xSemaphoreGive(optimizationTriggerSemaphore);
        return commandResult(true, true, "power planning configured");
    }

    if (request.action == ConfigCommandAction::REMOVE_LOAD) {
        if (!request.hasRelayPin) return commandResult(false, false, "relayPin required");

        if (request.nodeMacAddress == localMac) {
            HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
                return commandResult(false, false, "state is busy");
            }
            const bool removed = centralLoadHardwareStore.removeLoad(
                request.relayPin, relays, centralNode, reason);

            if (removed) {
                (void)centralLoadConfigurationStore.removeConfiguration(localMac, request.relayPin);
                (void)centralLoadConfigurationStore.persist();
                registry.addLocalCentralNode(
                    CentralNodeConfig::CENTRAL_NODE_NAME,
                    centralNode,
                    static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
            }
            xSemaphoreGive(stateMutex);

            if (!removed) return commandResult(false, false, hardwareConfigurationFailureText(reason));
            xSemaphoreGive(optimizationTriggerSemaphore);
            return commandResult(true, true, "Central Load removed");
        }

        Load::MacAddress nextHop{};
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }
        const bool allowed = isCommissionedSmartNode(request.nodeMacAddress);
        const bool routeFound = allowed && routeToSmartNode(request.nodeMacAddress, nextHop);
        xSemaphoreGive(stateMutex);

        if (!allowed || !routeFound) {
            return commandResult(false, false,
                allowed ? "no ESP-NOW route to Smart Node" : "Smart Node is not commissioned");
        }

        RemoveLoadCommandPacket packet{};
        packet.commandId = request.commandId;
        packet.relayPin = request.relayPin;
        const bool sent = communication.sendTo(
            nextHop,
            request.nodeMacAddress,
            EspNowCommunication::MessageType::CONFIGURE_LOAD_COMMAND,
            packet);

        if (sent && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(250U)) == pdTRUE) {
            pendingHardwareCommands.push_back(PendingHardwareCommand{
                request.commandId, request.nodeMacAddress, request.relayPin, true});
            xSemaphoreGive(stateMutex);
        }
        return commandResult(sent, false,
            sent ? "remove dispatched; awaiting Smart Node confirmation" : "ESP-NOW send failed");
    }

    return commandResult(false, false, "unsupported configuration command");
}

CommandResult handleSimulationCommand(void* context, const SimulationCommandRequest& request)
{
    (void)context;

    if (request.action == SimulationCommandAction::ENABLE) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }
        bool ok = batteryMonitor.enableSimulation(true);
        batteryReadingValid = false;
        if (ok) {
            const auto& battery = centralConfigurationStore.getConfiguration().batterySensor;
            if (battery.batteryMetadataConfigured) {
                ok = applyBatteryConfiguration(battery, true);
            }
        }
        xSemaphoreGive(stateMutex);
        if (ok) xSemaphoreGive(optimizationTriggerSemaphore);
        return commandResult(ok, ok, ok ? "simulation input enabled" : "simulation could not start");
    }

    if (request.action == SimulationCommandAction::DISABLE) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }
        bool ok = batteryMonitor.enableSimulation(false);
        batteryReadingValid = false;
        if (ok) {
            const auto& battery = centralConfigurationStore.getConfiguration().batterySensor;
            if (!battery.ina219Configured) {
                ok = false;
            } else {
                ok = applyBatteryConfiguration(battery, true);
            }
        }
        xSemaphoreGive(stateMutex);
        if (ok) xSemaphoreGive(optimizationTriggerSemaphore);
        return commandResult(ok, ok,
            ok ? "INA219 input selected" : "INA219 is not configured or not detected");
    }

    if (request.action == SimulationCommandAction::SET_VALUES) {
        if (!request.hasElectricalMeasurements && !request.hasStateOfChargePercent) {
            return commandResult(false, false, "simulation values required");
        }
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
            return commandResult(false, false, "state is busy");
        }
        if (!batteryMonitor.isSimulationEnabled()) {
            xSemaphoreGive(stateMutex);
            return commandResult(false, false, "enable simulation before setting values");
        }

        bool ok = true;
        if (request.hasElectricalMeasurements) {
            ok = batteryMonitor.setSimulatedMeasurements(
                request.batteryVoltageVolts,
                request.batteryCurrentAmps);
        }
        if (ok && request.hasStateOfChargePercent) {
            ok = batteryMonitor.setSimulatedStateOfChargePercent(request.stateOfChargePercent);
        }
        if (ok && request.hasElectricalMeasurements) {
            ok = batteryMonitor.updateMeasurements();
            if (ok) {
                latestBatteryMeasurements = batteryMonitor.getMeasurements();
                batteryReadingValid = true;
                batteryReadingMilliseconds = static_cast<std::uint32_t>(
                    pdTICKS_TO_MS(xTaskGetTickCount()));
            }
        }
        xSemaphoreGive(stateMutex);
        if (ok) xSemaphoreGive(optimizationTriggerSemaphore);
        return commandResult(ok, ok, ok ? "simulation values applied" : "invalid simulation values");
    }

    return commandResult(false, false, "unsupported simulation command");
}

CommandResult consoleConfigureIna219(void*, const Ina219SetupCommandRequest& request)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return commandResult(false, false, "state is busy");
    }

    auto configuration = centralConfigurationStore.getConfiguration().batterySensor;
    configuration.ina219Configured = true;
    configuration.shuntResistanceOhms = request.shuntResistanceOhms;
    configuration.maximumExpectedCurrentAmps = request.maximumExpectedCurrentAmps;
    configuration.emaAlpha = request.emaAlpha;

    const bool stored =
        CentralConfigurationStore::isValidBatterySensor(configuration) &&
        centralConfigurationStore.setBatterySensor(configuration) &&
        centralConfigurationStore.persist();

    bool sensorReady = false;
    if (stored && !batteryMonitor.isSimulationEnabled()) {
        sensorReady = applyBatteryConfiguration(configuration, true);
        batteryReadingValid = false;
    }
    xSemaphoreGive(stateMutex);

    if (!stored) return commandResult(false, false, "could not save INA219 setup");
    xSemaphoreGive(optimizationTriggerSemaphore);

    if (batteryMonitor.isSimulationEnabled()) {
        return commandResult(true, true, "INA219 setup saved; simulation remains active");
    }
    return commandResult(
        true,
        true,
        sensorReady ? "INA219 setup saved; sensor detected" : "INA219 setup saved; sensor not detected");
}

CommandResult consoleConfigureBatterySetup(void*, const BatterySetupCommandRequest& request)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return commandResult(false, false, "state is busy");
    }

    auto configuration = centralConfigurationStore.getConfiguration().batterySensor;
    const bool hadBatteryMetadata = configuration.batteryMetadataConfigured;
    configuration.batteryMetadataConfigured = true;
    configuration.batteryCapacityAmpHours = request.batteryCapacityAmpHours;
    configuration.initialStateOfChargePercent = request.initialStateOfChargePercent;
    configuration.nominalVoltageVolts = request.nominalVoltageVolts;

    const bool stored =
        CentralConfigurationStore::isValidBatterySensor(configuration) &&
        centralConfigurationStore.setBatterySensor(configuration) &&
        centralConfigurationStore.persist();

    if (stored) {
        const bool canApply = batteryMonitor.isSimulationEnabled() || configuration.ina219Configured;
        if (canApply) {
            (void)applyBatteryConfiguration(configuration, hadBatteryMetadata);
            batteryReadingValid = false;
        }
    }
    xSemaphoreGive(stateMutex);

    if (!stored) return commandResult(false, false, "could not save battery setup");
    xSemaphoreGive(optimizationTriggerSemaphore);
    return commandResult(true, true, "battery setup saved");
}

CommandResult consoleConfigurePowerPlanning(void*, const PowerPlanningCommandRequest& request)
{
    ConfigCommandRequest command{};
    command.commandId = allocateConsoleCommandId();
    command.action = ConfigCommandAction::CONFIGURE_POWER_PLANNING;
    command.nodeMacAddress = communication.getLocalMacAddress();
    command.hasPowerPlanningConfiguration = true;
    command.P_budget = request.P_budget;
    command.P_reserve = request.P_reserve;
    command.minimumStateOfChargePercent = request.minimumStateOfChargePercent;
    command.requiredRuntimeHours = request.requiredRuntimeHours;

    const CommandResult result = handleConfigCommand(nullptr, command);
    return commandResult(result.accepted, result.completed, result.reason);
}

CommandResult consoleNodeCommand(void*, const NodeCommandRequest& request)
{
    ConfigCommandRequest command{};
    command.commandId = allocateConsoleCommandId();
    command.nodeMacAddress = request.nodeMacAddress;

    if (request.action == NodeCommandRequest::Action::DECOMMISSION) {
        command.action = ConfigCommandAction::DECOMMISSION_NODE;
    } else {
        command.action = request.action == NodeCommandRequest::Action::COMMISSION
            ? ConfigCommandAction::COMMISSION_NODE
            : ConfigCommandAction::RENAME_NODE;
        command.hasFriendlyName = true;
        copyText(command.friendlyName, request.friendlyName);
    }

    const CommandResult result = handleConfigCommand(nullptr, command);
    return commandResult(result.accepted, result.completed, result.reason);
}

CommandResult consoleLoadCommand(void*, const LoadCommandRequest& request)
{
    return handleLoadCommand(nullptr, request);
}

CommandResult consoleConfigureLoad(void*, const LoadConfigurationCommandRequest& request)
{
    ConfigCommandRequest command{};
    command.commandId = allocateConsoleCommandId();
    command.action = ConfigCommandAction::CONFIGURE_LOAD;
    command.nodeMacAddress = request.nodeMacAddress;
    command.hasLoadConfiguration = true;
    copyText(command.loadName, request.name);
    command.relayPin = request.relayPin;
    command.relayActiveHigh = request.relayActiveHigh;
    command.mode = request.mode;
    command.powerType = request.powerType;
    command.priority = request.priority;
    command.powerRatingWatts = request.powerRatingWatts;
    command.schedule = request.schedule;

    const CommandResult result = handleConfigCommand(nullptr, command);
    return commandResult(result.accepted, result.completed, result.reason);
}

CommandResult consoleRemoveLoad(void*, const RemoveLoadCommandRequest& request)
{
    ConfigCommandRequest command{};
    command.commandId = allocateConsoleCommandId();
    command.action = ConfigCommandAction::REMOVE_LOAD;
    command.nodeMacAddress = request.nodeMacAddress;
    command.hasRelayPin = true;
    command.relayPin = request.relayPin;

    const CommandResult result = handleConfigCommand(nullptr, command);
    return commandResult(result.accepted, result.completed, result.reason);
}

CommandResult consoleSystem(void*, const SystemCommandRequest& request)
{
    return handleSystemCommand(nullptr, request);
}

CommandResult consoleNetwork(void*, const NetworkCommandRequest& request)
{
    if (request.target == NetworkCommandTarget::WIFI) {
        if (request.action == NetworkCommandRequest::Action::STATUS) {
            std::printf("WIFI\n");
            std::printf("Configured : %s\n", wifiCredentialsStore.isConfigured() ? "YES" : "NO");
            std::printf("State      : %s\n", wifiStateText(wifiManager.getState()));
            std::printf("Channel    : %u\n", static_cast<unsigned int>(wifiManager.getConnectedChannel()));
            std::printf("Radio Ch.  : %u\n", static_cast<unsigned int>(communication.getChannel()));
            return commandResult(true, true, "status printed");
        }
        if (request.action == NetworkCommandRequest::Action::SCAN) {
            const bool ok = wifiManager.printNearbyNetworks();
            return commandResult(ok, ok, ok ? "scan completed" : "scan failed");
        }
        if (request.action == NetworkCommandRequest::Action::SETUP) {
            const bool ok = wifiProvisioningPortal.begin(communication.getChannel());
            return commandResult(ok, ok, ok ? "setup AP active at 192.168.4.1" : "setup AP could not start");
        }
        if (request.action == NetworkCommandRequest::Action::SET) {
            const bool ok = wifiCredentialsStore.save(request.ssid, request.wifiPassword);
            if (!ok) return commandResult(false, false, "Wi-Fi credentials rejected");

            std::uint8_t discoveredChannel = 0U;
            if (wifiManager.findChannelForSsid(request.ssid, discoveredChannel) &&
                radioChannelStore.save(discoveredChannel)) {
                char text[80];
                std::snprintf(text, sizeof(text), "Wi-Fi saved on channel %u; restart Central to apply",
                    static_cast<unsigned int>(discoveredChannel));
                return commandResult(true, true, text);
            }
            return commandResult(true, true,
                "Wi-Fi saved; restart Central to apply (SSID not currently in range - set the radio channel manually if it still mismatches)");
        }
        if (request.action == NetworkCommandRequest::Action::SET_CHANNEL) {
            const bool ok = radioChannelStore.save(request.wifiChannel);
            return commandResult(ok, ok,
                ok ? "radio channel saved; restart Central to apply" : "radio channel rejected (must be 1-14)");
        }
        if (request.action == NetworkCommandRequest::Action::CLEAR) {
            const bool ok = wifiCredentialsStore.clear();
            return commandResult(ok, ok,
                ok ? "Wi-Fi credentials cleared; restart Central" : "Wi-Fi credentials could not be cleared");
        }
    }

    if (request.action == NetworkCommandRequest::Action::STATUS) {
        std::printf("MQTT\n");
        std::printf("Configured : %s\n", mqttCredentialsStore.isConfigured() ? "YES" : "NO");
        std::printf("State      : %s\n",
            mqttManager.getState() == MqttConnectionState::CONNECTED ? "CONNECTED" :
            (mqttManager.getState() == MqttConnectionState::CONNECTING ? "CONNECTING" : "DISCONNECTED"));
        return commandResult(true, true, "status printed");
    }
    if (request.action == NetworkCommandRequest::Action::SET) {
        const bool ok = mqttCredentialsStore.save(
            request.mqttHost,
            request.mqttPort,
            request.mqttUseTls,
            request.mqttUsername,
            request.mqttPassword);
        if (ok) {
            mqttStarted = false;
            mqttCredentialsUnavailableLogged = false;
            mqttBrokerStartupFailedLogged = false;
        }
        return commandResult(ok, ok, ok ? "MQTT saved; restart Central to apply" : "MQTT settings rejected");
    }
    if (request.action == NetworkCommandRequest::Action::CLEAR) {
        const bool ok = mqttCredentialsStore.clear();
        if (ok) mqttStarted = false;
        return commandResult(ok, ok,
            ok ? "MQTT credentials cleared; restart Central" : "MQTT credentials could not be cleared");
    }

    return commandResult(false, false, "unsupported network command");
}

CommandResult consoleSimulation(void*, const SimulationCommandRequest& request)
{
    return handleSimulationCommand(nullptr, request);
}

bool consoleSensorMode(void*, bool simulated)
{
    SimulationCommandRequest command{};
    command.commandId = allocateConsoleCommandId();
    command.action = simulated ? SimulationCommandAction::ENABLE : SimulationCommandAction::DISABLE;
    return handleSimulationCommand(nullptr, command).accepted;
}

Load::MacAddress consoleLocalMac(void*)
{
    return communication.getLocalMacAddress();
}

void consoleStatus(void*)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::printf("STATUS: BUSY\n");
        return;
    }

    const std::uint32_t now = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    std::size_t onlineSmartNodes = 0U;
    std::size_t totalSmartNodes = 0U;
    for (std::size_t i = 0U; i < registry.getNumberOfNodes(); ++i) {
        const auto* node = registry.getNode(i);
        if (node == nullptr || node->isCentralNode) continue;
        ++totalSmartNodes;
        if (isNodeOnline(*node, now)) ++onlineSmartNodes;
    }

    std::printf("SYSTEM STATUS\n");
    std::printf("Wi-Fi       : %s\n", wifiStateText(wifiManager.getState()));
    std::printf("MQTT        : %s\n", mqttManager.isConnected() ? "CONNECTED" : "DISCONNECTED");
    std::printf("Smart Nodes : %u / %u online\n",
        static_cast<unsigned int>(onlineSmartNodes),
        static_cast<unsigned int>(totalSmartNodes));
    std::printf("Battery     : %s\n", batteryTelemetryIsFresh(now) ? "AVAILABLE" : "NOT AVAILABLE");
    std::printf("Power plan  : %s\n", latestPlanningSnapshot.budgetValid ? "AVAILABLE" : "NOT AVAILABLE");
    if (latestPlanningSnapshot.budgetValid) {
        std::printf("P_auto_available: %.2f W\n",
            static_cast<double>(latestPlanningSnapshot.budget.P_auto_available));
    }
    std::printf("Control errors: %u\n", static_cast<unsigned int>(relayCommandErrorCount));
    xSemaphoreGive(stateMutex);
}

void consoleNodes(void*)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::printf("NODES: BUSY\n");
        return;
    }

    const std::uint32_t now = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    std::printf("NODES\n");
    for (std::size_t i = 0U; i < registry.getNumberOfNodes(); ++i) {
        const auto* node = registry.getNode(i);
        if (node == nullptr) continue;
        char mac[18]{};
        formatMacAddressText(mac, sizeof(mac), node->node.getMacAddress());
        const auto* record = commissioningRegistry.findByMac(node->node.getMacAddress());
        std::printf("%-7s %-19s %-17s %-10s Loads=%u\n",
            node->isCentralNode ? "CENTRAL" : "SMART",
            node->nodeName.c_str(),
            mac,
            isNodeOnline(*node, now) ? "ONLINE" : "OFFLINE",
            static_cast<unsigned int>(node->node.getNumberOfLoads()));
        if (record != nullptr) std::printf("        lifecycle=%s\n", kilowatts::toText(record->lifecycleState));
    }
    xSemaphoreGive(stateMutex);
}

void consoleNodeStatus(void*, const Load::MacAddress& address)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::printf("NODE: BUSY\n");
        return;
    }

    const auto* node = registry.findNodeByMacAddress(address);
    if (node == nullptr) {
        std::printf("Node not found\n");
        xSemaphoreGive(stateMutex);
        return;
    }

    char mac[18]{};
    formatMacAddressText(mac, sizeof(mac), address);
    const std::uint32_t now = static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    const auto* record = commissioningRegistry.findByMac(address);

    std::printf("NODE STATUS\n");
    std::printf("Name       : %s\n", node->nodeName.c_str());
    std::printf("MAC        : %s\n", mac);
    std::printf("Role       : %s\n", node->isCentralNode ? "CENTRAL" : "SMART");
    std::printf("Online     : %s\n", isNodeOnline(*node, now) ? "YES" : "NO");
    std::printf("Lifecycle  : %s\n", record != nullptr ? kilowatts::toText(record->lifecycleState) : "unknown");
    std::printf("Hop count  : %u\n", static_cast<unsigned int>(node->hopCountToCentral));
    std::printf("Loads      : %u\n", static_cast<unsigned int>(node->node.getNumberOfLoads()));

    for (std::size_t i = 0U; i < node->node.getNumberOfLoads(); ++i) {
        const Load* load = node->node.getLoad(i);
        if (load == nullptr) continue;
        std::printf("  pin=%u | %s | %.2f W | priority=%u | %s\n",
            static_cast<unsigned int>(load->getRelayPin()),
            load->getName().c_str(),
            static_cast<double>(load->getPowerRatingWatts()),
            static_cast<unsigned int>(load->getPriority()),
            loadModeText(load->getMode()));
    }
    xSemaphoreGive(stateMutex);
}

void consoleLoads(void*)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::printf("LOADS: BUSY\n");
        return;
    }

    applyStoredLoadSettings();
    std::printf("LOADS\n");
    std::size_t count = 0U;
    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const auto* node = registry.getNode(nodeIndex);
        if (node == nullptr) continue;
        char mac[18]{};
        formatMacAddressText(mac, sizeof(mac), node->node.getMacAddress());
        for (std::size_t loadIndex = 0U; loadIndex < node->node.getNumberOfLoads(); ++loadIndex) {
            const Load* load = node->node.getLoad(loadIndex);
            if (load == nullptr) continue;
            ++count;
            std::printf("%s pin=%u | %-16s | %7.2f W | priority=%u | %-9s | %s\n",
                mac,
                static_cast<unsigned int>(load->getRelayPin()),
                load->getName().c_str(),
                static_cast<double>(load->getPowerRatingWatts()),
                static_cast<unsigned int>(load->getPriority()),
                loadModeText(load->getMode()),
                loadPowerTypeText(load->getPowerType()));
        }
    }
    if (count == 0U) std::printf("None\n");
    xSemaphoreGive(stateMutex);
}

void consoleLoadStatus(void*, const Load::MacAddress& macAddress, std::uint8_t relayPin)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::printf("LOAD: BUSY\n");
        return;
    }

    applyStoredLoadSettings();
    const Load* load = registry.findMutableLoad(macAddress, relayPin);
    if (load == nullptr) {
        std::printf("Load not found\n");
        xSemaphoreGive(stateMutex);
        return;
    }

    char mac[18]{};
    formatMacAddressText(mac, sizeof(mac), macAddress);
    const AutoSchedule schedule = load->getSchedule();
    std::printf("LOAD STATUS\n");
    std::printf("Node MAC       : %s\n", mac);
    std::printf("Relay pin      : %u\n", static_cast<unsigned int>(relayPin));
    std::printf("Name           : %s\n", load->getName().c_str());
    std::printf("Power rating   : %.2f W\n", static_cast<double>(load->getPowerRatingWatts()));
    std::printf("Priority       : %u\n", static_cast<unsigned int>(load->getPriority()));
    std::printf("Power type     : %s\n", loadPowerTypeText(load->getPowerType()));
    std::printf("Mode           : %s\n", loadModeText(load->getMode()));
    if (schedule.enabled) {
        std::printf("AUTO schedule  : %02u:%02u\n",
            static_cast<unsigned int>(schedule.hour),
            static_cast<unsigned int>(schedule.minute));
    } else {
        std::printf("AUTO schedule  : none\n");
    }
    std::printf("BFS result     : %s\n",
        bestFirstRejectionText(load->getLastBestFirstRejectionReason()));

    if (macAddress == communication.getLocalMacAddress()) {
        bool found = false;
        for (std::size_t i = 0U; i < relays.getNumberOfRelays(); ++i) {
            const RelayController::RelayConfiguration* config = relays.getRelay(i);
            if (config != nullptr && config->relayPin == relayPin) {
                std::printf("Relay polarity : %s\n", config->activeHigh ? "active-HIGH" : "active-LOW");
                std::printf("Relay applied  : %s\n", relays.isHardwareApplied(relayPin) ? "yes" : "no");
                found = true;
                break;
            }
        }
        if (!found) std::printf("Relay polarity : NOT REGISTERED on this Node's RelayController\n");
    } else {
        std::printf("Relay polarity : unavailable (Load is on a Smart Node, not Central)\n");
    }
    xSemaphoreGive(stateMutex);
}

void consoleDashboard(void*)
{
    runOptimizationCycle(true);
}

void consoleOptimize(void*)
{
    runOptimizationCycle(true);
}

void printCentralBootSummary(const Load::MacAddress& localMac)
{
    char mac[18]{};
    formatMacAddressText(mac, sizeof(mac), localMac);
    ESP_LOGI(TAG, "Central ready: %s", mac);
}

} // namespace
