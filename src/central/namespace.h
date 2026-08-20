
/**
 * @file namespace.h
 * @brief Central Node orchestration entry point.
 *
 * This file only instantiates real modules, creates the FreeRTOS
 * tasks/queues/synchronization that wire them together, and starts the
 * system. Every real responsibility (registry aggregation, filtering,
 * power-limit calculation, scheduling, Best-First Search, relay dispatch
 * sequencing, JSON formatting, transport) lives in the lib/ module that
 * owns it — this file sequences calls to those modules, it does not
 * reimplement any of their mathematics.
 *
 * Planning cycle (run by optimizationTask()):
 *
 *   latest atomic telemetry (stateMutex-protected snapshot)
 *       -> BatteryStateOfCharge::update()
 *       -> SafePowerLimitCalculator::calculate()
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
 * and zero known remote Nodes until real ESP-NOW discovery
 * (IDENTITY_REPORT) and an operator's commissioning command populate
 * NodeCommissioningRegistry — handled inline inside the ESP-NOW
 * Communication Task's own receive dispatch (IDENTITY_REPORT/
 * COMMISSION_ACK/DECOMMISSION_ACK, the same place NODE_REPORT/
 * ACKNOWLEDGEMENT already are) and the MQTT commands/config handler
 * (handleConfigCommand()).
 */

#include "BestFirstSearch.h"
#include "CentralNodeConfig.h"
#include "CentralConfigurationStore.h"
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
#include "MqttCredentialsStore.h"
#include "MqttManager.h"
#include "Node.h"
#include "NodeCommissioningRegistry.h"
#include "NodeLifecycle.h"
#include "NodeLoadHardwareStore.h"
#include "NodeReportPackets.h"
#include "NodeRegistryJson.h"
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
WiFiCredentialsStore wifiCredentialsStore;
MqttCredentialsStore mqttCredentialsStore;
WiFiProvisioningPortal wifiProvisioningPortal;

/*
 * How many consecutive failed WiFiManager reconnect attempts are tolerated
 * before the self-hosted setup Access Point comes up (see watchdogTask()).
 * Deliberately more than one transient failure (a neighbour's AP briefly
 * busy, a missed scan window) so the portal only appears once it is clear
 * the configured credentials genuinely cannot associate.
 */
constexpr std::uint32_t WIFI_PROVISIONING_TRIGGER_ATTEMPT_COUNT = 3U;
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
 * Central's own local Node, owning whatever Loads are directly wired to
 * Central's own relay pins (distinct from the Loads Central learns about
 * remotely from Smart Nodes over ESP-NOW). Node has no default
 * constructor (a Node's MAC is fixed at construction), so this starts
 * with a placeholder all-zero MAC and is replaced with the real one in
 * configureLocalHardware() the moment the real local MAC is known -
 * exactly once, at boot, before anything else can reference it.
 */
Node centralNode(EspNowCommunication::MacAddress{});

/** Persists Central's own local Load/relay hardware configuration - see NodeLoadHardwareStore's own README. */
NodeLoadHardwareStore centralLoadHardwareStore;

/**
 * Central's authoritative identity/lifecycle record for every Node it
 * knows about — a distinct concern from `registry` above (planning-time
 * Node/Load domain data and route), never duplicating its fields; joined
 * by MAC address where both are needed (see NodeRegistryJson). Guarded by
 * stateMutex, same as every other shared planning structure.
 */
NodeCommissioningRegistry commissioningRegistry;

SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t optimizationTriggerSemaphore = nullptr;

/**
 * Central has no battery sensor by default. It becomes configured only when
 * a persisted installer configuration has first registered and probed the
 * real INA219. Guarded by stateMutex.
 */
bool batterySensorConfigured = false;
std::uint8_t batterySensorI2CAddress = 0U;

LoadMeasurements latestBatteryMeasurements{
    CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS, 0.0F, 0.0F
};
std::int64_t lastOptimizationEpochSeconds = 0;
std::uint32_t faultCount = 0U;

/*
 * Battery telemetry validity is tracked separately from the measurement
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
 * Remembers which remote Node MAC addresses are currently considered
 * offline (last NODE_REPORT older than
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
 * FAULTED/UNAVAILABLE — a faulted/unavailable Load is never commanded ON
 * again until fault/recovery policy permits it. A Load that fails this
 * check keeps whatever target/confirmed state it already had — it is
 * simply left out of this cycle's planning and dispatch, never forced
 * OFF or assumed safe.
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
           std::fabs(first.initialStateOfChargePercent - second.initialStateOfChargePercent) <= EPSILON &&
           std::fabs(first.nominalVoltageVolts - second.nominalVoltageVolts) <= EPSILON;
}


/**
 * Initializes Central's own I2C bus and registers Central's own local
 * Node, restoring any local Loads an installer has previously configured
 * on Central's own relay pins (see NodeLoadHardwareStore). A freshly
 * flashed/uncommissioned Central has zero local Loads/Branches and zero
 * configured sensors, including its own battery-bus INA219 —
 * batterySensorConfigured stays false until an installer command
 * registers and verifies one for real.
 */
void configureLocalHardware(const EspNowCommunication::MacAddress& localMac)
{
    sensors.initializeBus(CentralNodeConfig::i2cBusConfiguration());

    centralNode = Node(localMac);

    /*
     * Same board-safety contract as Smart Node's own
     * persistedRelayConfigurationMatchesBoardProfile(): a persisted relay
     * pin from a previous flash must never be trusted blindly if this
     * image's board profile no longer declares it safe.
     * commissioningRegistry.registerSelf() has not run yet at this point
     * in boot, so this checks CentralNodeConfig::VERIFIED_RELAY_GPIO_PINS
     * directly rather than going through nodeDeclaresRelayPin()/a
     * commissioning record.
     */
    const bool hardwareConfigurationRestored = centralLoadHardwareStore.loadPersisted();
    bool boardProfileMatches = true;
    for (std::size_t index = 0U; index < centralLoadHardwareStore.getNumberOfConfigurations(); ++index) {
        const NodeLoadHardwareStore::LoadConfiguration* configuration = centralLoadHardwareStore.getConfiguration(index);
        if (configuration == nullptr) {
            continue;
        }
        bool declared = false;
        for (const std::uint8_t pin : CentralNodeConfig::VERIFIED_RELAY_GPIO_PINS) {
            if (pin == configuration->relayPin) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            ESP_LOGE(TAG,
                     "NVS_RESTORE: configured local relay GPIO %u is not declared safe by this flashed board profile",
                     static_cast<unsigned int>(configuration->relayPin));
            boardProfileMatches = false;
            break;
        }
    }

    if (hardwareConfigurationRestored && boardProfileMatches) {
        HardwareConfigurationFailureReason restoreFailureReason = HardwareConfigurationFailureReason::NONE;
        if (!centralLoadHardwareStore.applyPersistedConfigurations(relays, centralNode, restoreFailureReason)) {
            ESP_LOGE(TAG, "Failed to restore Central's persisted local Load configuration: %s",
                     hardwareConfigurationFailureText(restoreFailureReason));
        }
    } else if (hardwareConfigurationRestored) {
        ESP_LOGW(TAG, "NVS_RESTORE: retaining Central local Load configuration without applying it "
                      "- board profile is not verified");
    }

    registry.addLocalCentralNode(CentralNodeConfig::CENTRAL_NODE_NAME, centralNode, 0U);

    /*
     * addLocalCentralNode() must run first (setLocalCentralBranchMaximumCurrentAmps()
     * is rejected otherwise) - without this loop, a restored local Load's
     * I_branch,max would stay unknown to BestFirstSearch until an
     * installer reconfigured it again after every reboot.
     */
    for (std::size_t index = 0U; index < centralLoadHardwareStore.getNumberOfConfigurations(); ++index) {
        const NodeLoadHardwareStore::LoadConfiguration* configuration = centralLoadHardwareStore.getConfiguration(index);
        if (configuration != nullptr && centralNode.getLoadByRelayPin(configuration->relayPin) != nullptr) {
            registry.setLocalCentralBranchMaximumCurrentAmps(configuration->relayPin, configuration->branchMaximumCurrentAmps);
        }
    }
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


/**
 * The installer-entered nameplate voltage (see CentralConfigurationStore's
 * CONFIGURE_BATTERY_SENSOR command) whenever a battery sensor has been
 * commissioned; CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS is only
 * ever used before that, on a never-commissioned device, or wherever a
 * live voltage reading is temporarily unavailable/invalid. Firmware never
 * assumes a fixed nameplate voltage for a real, commissioned battery.
 */
float configuredOrDefaultNominalVoltageVolts()
{
    const CentralConfigurationStore::BatterySensorConfiguration& batterySensor =
        centralConfigurationStore.getConfiguration().batterySensor;
    return batterySensor.configured ? batterySensor.nominalVoltageVolts : CentralNodeConfig::NOMINAL_BATTERY_VOLTAGE_VOLTS;
}


SafePowerLimitCalculator::Inputs configuredSafePowerLimitInputs(
    float stateOfChargePercent, float batteryBusVoltageVolts,
    const CentralConfigurationStore::SafetyPolicy& policy)
{
    const CentralConfigurationStore::BatterySensorConfiguration& batterySensor =
        centralConfigurationStore.getConfiguration().batterySensor;

    SafePowerLimitCalculator::Inputs inputs{};
    inputs.stateOfChargePercent = stateOfChargePercent;
    inputs.minimumStateOfChargePercent = policy.minimumStateOfChargePercent;
    inputs.nominalBatteryVoltageVolts = configuredOrDefaultNominalVoltageVolts();
    inputs.batteryCapacityAmpHours = batterySensor.configured
        ? batterySensor.batteryCapacityAmpHours
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
 * Sensor Acquisition Task (~1s): reads Central's one battery-bus INA219
 * through the filtered measurement path and advances
 * BatteryStateOfCharge from that battery sensor's filtered current.
 * Individual Smart-node Loads use installer-entered ratings, not
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
 * A failed/timed-out command NEVER calls Load::setConfirmedRelayState() —
 * a failed acknowledgement does not prove the relay is OFF, so the
 * previous trusted confirmed state (and its validity) is simply left
 * exactly as it was. Only Load::setHealth() changes on failure.
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

    /* Every field logged here is taken from the exact target/packet being sent, not recomputed. */
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
 * Determines every Fixed Load's relay target for this cycle. Fixed Loads
 * are never Best-First Search candidates: normally a Fixed Load's target
 * simply follows its own configured LoadMode (FIXED_ON -> target ON,
 * FIXED_OFF -> target OFF).
 *
 * Also protects against combined FIXED_ON running power exceeding what
 * is genuinely available this cycle. The trigger used here is exactly
 * P_fixed > P_available: no separate SoC <= SoC_min check is needed,
 * because SafePowerLimitCalculator's own usable-energy clamp already forces
 * P_available itself to zero once SoC has reached SoC_min, so that
 * condition is already covered by this single comparison.
 *
 * There is no Best-First-style ranking for which Fixed Load(s) to shed
 * under this condition, so a single deterministic policy is used
 * instead: shed currently FIXED_ON Loads in ascending configured-priority
 * order (Load::getPriority() lowest first — this project's convention is
 * that a higher priority number is more important, e.g. Light=8, so the
 * least-important Load is shed first) until the remaining committed
 * Fixed power fits within availablePowerWatts or no further eligible
 * Load remains. Only Loads currently eligible for a new command
 * (isLoadEligibleForNewCommandsThisCycle()) are ever chosen to shed — an
 * offline/faulted Fixed Load cannot receive a new command this cycle
 * either way, so it is left exactly at its previous target rather than
 * being force-assumed OFF.
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

        load->setTargetRelayState(true);
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


/*
 * MqttManager::begin()'s own documented contract (see MqttManager.h) is
 * that the caller starts it only once Wi-Fi actually has an IP address -
 * starting it unconditionally at boot made it dial a broker over a
 * network that was not there yet (repeated esp-tls/getaddrinfo failures
 * while Wi-Fi was still disconnected or the setup Access Point was up).
 * This is therefore a one-shot trigger, checked every optimisation cycle:
 * the moment WiFiManager first reports a real IP, MQTT starts exactly
 * once for the rest of this boot. esp-mqtt handles its own reconnection
 * internally afterwards, so no equivalent stop/restart is needed if
 * Wi-Fi later drops and comes back.
 */
void checkMqttStartTrigger()
{
    static bool mqttStarted = false;

    if (!mqttStarted && wifiManager.isConnected()) {
        mqttStarted = true;

        MqttCredentialsStore::Credentials provisioned{};
        const bool useProvisionedBroker = mqttCredentialsStore.load(provisioned);
        const bool started = useProvisionedBroker
            ? mqttManager.begin(MqttManager::Credentials{
                  provisioned.host, provisioned.port, provisioned.useTls,
                  provisioned.username, provisioned.password})
            : mqttManager.begin(MqttManager::Credentials{
                  Secrets::MQTT_BROKER_HOST, Secrets::MQTT_BROKER_PORT, Secrets::MQTT_BROKER_USE_TLS,
                  Secrets::MQTT_USERNAME, Secrets::MQTT_PASSWORD});

        if (useProvisionedBroker) {
            ESP_LOGI(TAG, "MQTT_START: Wi-Fi connected; starting MQTT client (provisioned broker '%s')", provisioned.host);
        } else {
            ESP_LOGI(TAG, "MQTT_START: Wi-Fi connected; starting MQTT client (built-in cloud broker)");
        }
        if (!started) {
            ESP_LOGW(TAG, "MQTT did not start; continuing with local-only control");
        }
    }
}


/**
 * Optimisation Task (nominal period 5s, also woken immediately by
 * optimizationTriggerSemaphore): the complete planning cycle described
 * at the top of this file.
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
    SafePowerLimitCalculator safePowerLimit;
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

    /* Timed-out relay commands become faults before this cycle plans anything new. */
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
     * Going offline is only detectable here, by timeout (the absence of
     * a report, not an event) — logged only on the transition so a Node
     * that stays offline for many cycles never floods the log. The
     * matching NODE_RECOVERED transition is detected and logged where a
     * fresh report actually arrives (espNowCommunicationTask), which
     * also immediately re-triggers optimisation rather than waiting for
     * the next periodic cycle.
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
     * Only ever call SafePowerLimitCalculator::calculate() from a
     * trustworthy battery reading. While untrustworthy, planning freezes
     * at the last genuinely valid Pavailable/voltage rather than
     * fabricating a fresh limit from a stale/failed reading — actuation
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

        if (safePowerLimit.calculate(configuredSafePowerLimitInputs(stateOfChargePercent, batteryVoltageVolts, safetyPolicy))) {
            lastValidAvailablePowerWatts = safePowerLimit.getAvailablePowerWatts();
            lastValidBatteryVoltageVolts = batteryVoltageVolts;
        }

        availablePowerWattsForPlanning = safePowerLimit.getAvailablePowerWatts();
        maximumBatteryPowerWattsForPlanning = safePowerLimit.getMaximumBatteryPowerWatts();
    } else {
        batteryVoltageVolts = lastValidBatteryVoltageVolts;
        batteryCurrentAmps = 0.0F;
        /* Pavailable stays at its zero-initialized default until a real battery state has ever been established - never fabricated. */
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

    /*
     * Best-First Search has nothing to search over when there are zero
     * Auto Load candidates (Fixed Loads are never BFS candidates - their
     * targets were already decided above by applyFixedLoadTargets()), so
     * it is not even constructed in that case: constructing it alone logs
     * unconditionally (BestFirstSearch's constructor), which would
     * otherwise print a full "created / weights / search started /
     * running / completed" sequence every single optimisation cycle for
     * no reason on a system with no Auto Loads configured yet.
     * remainingPowerWattsForState/powerConsumedByActiveLoadsWatts default to
     * exactly what a search that started but never admitted anything
     * would report anyway (P_remaining = the full Auto-load limit,
     * P_committed = Fixed-only power) so downstream state/logging stays
     * correct either way.
     */
    const std::size_t autoCandidateLoadCount = loadFilter.getNumberOfAutoCandidateLoads();
    float remainingPowerWattsForState = availablePowerManager.getPowerAvailableForAutoLoadsWatts();
    float powerConsumedByActiveLoadsWatts = availablePowerManager.getFixedOnRunningPowerWatts();

    if (autoCandidateLoadCount == 0U) {
        ESP_LOGI(TAG, "Best-First Search skipped this cycle: no Auto Load candidates configured");
    } else {
        BestFirstSearch search;
        search.setSearchScoreWeights(CentralNodeConfig::bestFirstSearchWeights());

        BestFirstSearch::ElectricalPlanningState planningState{};
        planningState.stateOfChargePercent = stateOfChargePercent;
        planningState.minimumStateOfChargePercent = safetyPolicy.minimumStateOfChargePercent;
        planningState.warningStateOfChargePercent = safetyPolicy.warningStateOfChargePercent;
        planningState.batteryBusVoltageVolts = batteryVoltageVolts > 0.0F ? batteryVoltageVolts : configuredOrDefaultNominalVoltageVolts();
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
                    /* A stale/offline Node or a faulted/unavailable Load is never a new BFS candidate. */
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
                 * relay state only — an Auto Load's configured AUTO_ON/AUTO_OFF
                 * mode (getMode()) is a user/system setting that Best-First
                 * Search's per-cycle selection/rejection must never overwrite.
                 */
                mutableLoad->setTargetRelayState(selected);
            }

            /*
             * Admitted Auto Loads' ON commands must be dispatched in exactly
             * the order run() extracted them from OPEN — getAdmittedLoad() is
             * the only source of that order; reconstructing it from
             * registry/Node/MAC traversal order would silently discard it.
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

            remainingPowerWattsForState = search.getRemainingPowerWatts();
            powerConsumedByActiveLoadsWatts = search.getCommittedPowerWatts();
        } else {
            ESP_LOGW(TAG, "Best-First Search did not start this cycle (invalid planning state)");
        }
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
                continue; // never send a new command to an unreachable Node
            }
            if (load->getTargetRelayState() && load->getHealth() != LoadHealth::AVAILABLE) {
                continue; // never command a faulted/unavailable Load ON again until recovery
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
     * Live sequential accounting for the pre-ON safety re-check: starts
     * from the conservative rated estimate of power that may already be
     * flowing, then only ever advances after a genuinely CONFIRMED
     * transition — never optimistically, and never from a value
     * BestFirstSearch merely planned.
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
             * planning snapshot. The P_remaining/P_committed guard models
             * BestFirstSearch's own sequential Auto-load allocation
             * specifically, so it is only re-applied below to admitted
             * Auto ON transitions, not re-applied here.
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
         * Admitted Auto-load ON transition: re-check feasibility
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
                ? latestBatteryMeasurements.voltageVolts : configuredOrDefaultNominalVoltageVolts();

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
             * evaluating the next one.
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
        systemState.remainingPowerWatts = remainingPowerWattsForState;
        systemState.committedPowerWatts = powerConsumedByActiveLoadsWatts;
        systemState.wifiConnected = wifiManager.isConnected();
        systemState.wifiStateText = wifiStateText(wifiManager.getState());
        systemState.mqttConnected = mqttManager.isConnected();
        systemState.currentTimeValid = currentTimeProvider.isCurrentTimeValid();
        systemState.currentTimeSourceText =
            currentTimeProvider.getCurrentTimeSource() == TimeSource::NTP ? "NTP" :
            (currentTimeProvider.getCurrentTimeSource() == TimeSource::MANUAL ? "MANUAL" : "NONE");
        systemState.lastOptimizationEpochSeconds = lastOptimizationEpochSeconds;
        systemState.faultCount = faultCount;
        systemState.faultSummaryText = !batteryStateKnown
            ? (!batterySensorConfigured ? "Battery sensor not configured; Auto-load allocation restricted"
               : (!stateOfChargeValid ? "SoC is UNKNOWN; Auto-load allocation restricted"
                  : "Battery telemetry invalid or stale; planning frozen at last valid values"))
            : (faultCount > 0U ? "See per-Load health in state/loads" : "");

        const std::string systemJson = SystemStateJson::build(systemState, CentralNodeConfig::MQTT_SCHEMA_VERSION);
        const std::string treeJson = TopologyTree::buildTreeJson(
            registry, commissioningRegistry, CentralNodeConfig::MQTT_SCHEMA_VERSION,
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

        /* NODE_OFFLINE events, collected while stateMutex was held above. */
        for (const auto& offlineNode : newlyOfflineNodes) {
            char targetText[18] = {};
            formatMacAddressText(targetText, sizeof(targetText), offlineNode.first);
            mqttManager.publishEvent("NODE_OFFLINE", targetText, offlineNode.second.c_str());
        }
    }

    if (batterySensorConfigured) {
        ESP_LOGI(TAG, "Battery: V=%.2fV I=%.2fA P=%.1fW SoC=%.1f%% (source=%s)",
                 static_cast<double>(batteryVoltageVolts), static_cast<double>(batteryCurrentAmps),
                 static_cast<double>(batteryVoltageVolts * batteryCurrentAmps),
                 static_cast<double>(stateOfChargePercent), toText(batteryStateOfCharge.getSource()));
    } else {
        ESP_LOGI(TAG, "Battery: NOT CONFIGURED - no INA219 sensor commissioned for the battery bus yet");
    }

    ESP_LOGI(TAG, "Optimisation cycle complete: SoC=%.1f%% Pavailable=%.1fW Premaining=%.1fW "
                  "PowerConsumedByActiveLoads=%.1fW PowerConsumedByFixedLoads=%.1fW",
             static_cast<double>(stateOfChargePercent),
             static_cast<double>(availablePowerManager.getTotalAvailablePowerWatts()),
             static_cast<double>(remainingPowerWattsForState),
             static_cast<double>(powerConsumedByActiveLoadsWatts),
             static_cast<double>(availablePowerManager.getFixedOnRunningPowerWatts()));
}


void optimizationTask(void *parameter)
{
    (void)parameter;

    while (true) {
        xSemaphoreTake(optimizationTriggerSemaphore, pdMS_TO_TICKS(CentralNodeConfig::OPTIMIZATION_PERIOD_MILLISECONDS));
        checkMqttStartTrigger();
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

            /* Log decoded values BEFORE mutation, exactly as received. */
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

            ESP_LOGI(TAG, "CENTRAL_APPLY NODE_REPORT seq=%u node=%s result=ACCEPTED loadsReceived=%u loadsNowStored=%u",
                     static_cast<unsigned int>(packet.reportSequenceId), srcText,
                     static_cast<unsigned int>(packet.numberOfLoads), static_cast<unsigned int>(loadsNowStored));

            /* Prove decode+apply, not just MAC-layer delivery. */
            if (ackRouteFound) {
                NodeReportAcknowledgementPacket ack{};
                ack.reportSequenceId = packet.reportSequenceId;
                ack.status = static_cast<std::uint8_t>(NodeReportAckStatus::ACCEPTED);
                communication.sendTo(ackNextHopMac, packet.nodeMacAddress,
                                      EspNowCommunication::MessageType::NODE_REPORT_ACK, ack);
            }

            if (recovered) {
                /* Recovery re-triggers optimisation immediately rather than waiting up to one full period. */
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

            /* Decoded values, before this class mutates any Load. */
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
                             * is OFF — leave the previous trusted confirmed
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

                NodeCommissioningRegistry::Diagnostics diagnostics{};
                diagnostics.freeHeapBytes = packet.freeHeapBytes;
                diagnostics.minFreeHeapBytes = packet.minFreeHeapBytes;
                diagnostics.flashSizeBytes = packet.flashSizeBytes;
                diagnostics.psramSizeBytes = packet.psramSizeBytes;
                diagnostics.siliconRevision = packet.siliconRevision;
                diagnostics.cpuCores = packet.cpuCores;
                std::snprintf(diagnostics.resetReason, sizeof(diagnostics.resetReason), "%s", packet.resetReason);
                commissioningRegistry.updateDiagnostics(originMac, diagnostics);

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
                   received.message.header.messageType == EspNowCommunication::MessageType::FACTORY_RESET_ACK &&
                   received.message.header.payloadLength == sizeof(FactoryResetAckPacket)) {

            FactoryResetAckPacket acknowledgement{};
            std::memcpy(&acknowledgement, received.message.payload.data(), sizeof(acknowledgement));

            char targetText[18] = {};
            formatMacAddressText(targetText, sizeof(targetText), received.message.header.originMacAddress);

            /*
             * Only ever sent on rejection - a successful remote factory
             * reset reboots the Node immediately instead of replying.
             * This ACK therefore only ever reports FAILED/REJECTED, never
             * APPLIED.
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


/*
 * Once WiFiManager has failed to associate
 * WIFI_PROVISIONING_TRIGGER_ATTEMPT_COUNT times in a row, the
 * installer-provisioned credentials (WiFiCredentialsStore) are treated as
 * genuinely unreachable rather than a transient miss, and the self-hosted
 * setup Access Point comes up so an installer can provision a working
 * SSID/password from a phone. Brought back down the moment WiFiManager
 * reports a real IP again - it never stays active concurrently with a
 * working station connection. A RADIO_CHANNEL_MISMATCH is a different,
 * AP-side problem (the configured SSID is broadcasting on the wrong
 * channel) that reprovisioning cannot fix, so it deliberately does not
 * trigger the portal.
 */
void checkWiFiProvisioningTrigger()
{
    const bool stuckDisconnected =
        wifiManager.getState() == WiFiConnectionState::DISCONNECTED &&
        wifiManager.getReconnectAttemptCount() >= WIFI_PROVISIONING_TRIGGER_ATTEMPT_COUNT;

    if (stuckDisconnected && !wifiProvisioningPortal.isActive()) {
        ESP_LOGW(TAG, "WIFI_PROVISIONING: %u failed reconnect attempts; starting self-hosted setup Access Point",
                 static_cast<unsigned int>(wifiManager.getReconnectAttemptCount()));
        wifiProvisioningPortal.begin(kilowatts::KILOWATTS_RADIO_CHANNEL);
    } else if (wifiManager.isConnected() && wifiProvisioningPortal.isActive()) {
        ESP_LOGI(TAG, "WIFI_PROVISIONING: station connected; stopping the setup Access Point");
        wifiProvisioningPortal.end();
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
        checkWiFiProvisioningTrigger();
        ESP_LOGI(TAG, "===========================================");
    }
}


/**
 * MQTT load-command handler: performs every domain validation, then
 * updates the domain Load, persists the change, and signals the
 * Optimisation Task — it never calls the optimizer directly (never
 * recursively call the optimizer from MQTT callbacks).
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
 * Establishes safe physical outputs, erases every installation-specific
 * persisted record (a full NVS erase - the ESP32's MAC address and chip
 * identity live in efuses, never NVS, so this can never touch immutable
 * hardware identity), and reboots into PRODUCTION + UNCOMMISSIONED. Never
 * returns.
 *
 * A deliberate, explicit action distinct from uploading new firmware - a
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
 * One concise, authoritative boot-state block - every value here is read
 * live from the modules that own it, never hardcoded text.
 */
void printCentralBootSummary(const EspNowCommunication::MacAddress& localMac)
{
    char macText[18] = {};
    formatMacAddressText(macText, sizeof(macText), localMac);

    const CentralNodeRegistry::PlanningNode* localNode = registry.findNodeByMacAddress(localMac);
    const std::size_t localLoadCount = localNode != nullptr ? localNode->node.getNumberOfLoads() : 0U;

    ESP_LOGI(TAG, "========== KILOWATTS CENTRAL BOOT STATE ==========");
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
    ESP_LOGI(TAG, "Physical Actuation    : ENABLED");
    ESP_LOGI(TAG, "===================================================");
}


/**
 * Minimal engineering test console: reads simple line commands from the
 * same UART the log output already uses, and calls exactly the same
 * handler function the MQTT commands/system path calls - never a second,
 * divergent code path. This exists so factory reset is verifiable on real
 * hardware even when Wi-Fi/MQTT connectivity is unavailable; it is not a
 * production control surface.
 *
 * Commands: factory_reset
 */
void consoleTask(void *parameter)
{
    (void)parameter;

    char line[160];
    ESP_LOGI(TAG, "CONSOLE: engineering test console ready (factory_reset)");

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

        if (std::strcmp(line, "factory_reset") == 0) {
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
 * MQTT config-command handler: validates the target Node and requested
 * transition against commissioningRegistry, then dispatches the matching
 * ESP-NOW commissioning packet over the Node's already-known route
 * (found exactly like a relay command's route is — see
 * findNextHopFromCentral()). The handler's own return only reflects this
 * immediate, local outcome; a commissioning/rename's final APPLIED/FAILED
 * result is published separately once the Node's own
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
            request.batteryNominalVoltageVolts,
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
                          "battery hardware settings locked after commissioning; factory-reset to rewire");
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

        /*
         * Central's own directly-wired Loads never go over ESP-NOW - this
         * completes synchronously, unlike the Smart Node path below which
         * only dispatches a command and waits for a later
         * CONFIGURE_LOAD_ACK. Checks CentralNodeConfig::isVerifiedRelayPin()
         * directly - the same compiled-in board profile Smart Node checks
         * via SmartNodeConfig::isVerifiedRelayPin() - rather than a
         * commissioning-registry record, since that record is only ever
         * written once by registerSelf() and never refreshed on later
         * boots, so it must never be this check's source of truth.
         */
        if (request.nodeMacAddress == localMac) {
            if (!CentralNodeConfig::isVerifiedRelayPin(request.relayPin)) {
                std::snprintf(result.reason, sizeof(result.reason),
                              "relay GPIO is not declared safe by Central's own board profile");
                xSemaphoreGive(stateMutex);
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
            configuration.branchMaximumCurrentAmps = request.branchMaximumCurrentAmps;
            configuration.startupWatts = request.startupWatts;
            configuration.schedule = request.schedule;

            HardwareConfigurationFailureReason failureReason = HardwareConfigurationFailureReason::NONE;
            const bool configured =
                centralLoadHardwareStore.configureNewLoad(configuration, relays, centralNode, failureReason);

            if (configured) {
                registry.setLocalCentralBranchMaximumCurrentAmps(request.relayPin, request.branchMaximumCurrentAmps);
                registry.addLocalCentralNode(CentralNodeConfig::CENTRAL_NODE_NAME, centralNode,
                                              static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
            }

            xSemaphoreGive(stateMutex);
            if (configured) {
                xSemaphoreGive(optimizationTriggerSemaphore);
            }

            result.accepted = configured;
            std::snprintf(result.reason, sizeof(result.reason),
                          configured ? "applied" : hardwareConfigurationFailureText(failureReason));
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