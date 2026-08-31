#include "namespace.h"
#include "CentralApplication.h"

namespace {

CommandResult consoleNetworkReadable(void* context, const NetworkCommandRequest& request)
{
    if (request.action == NetworkCommandRequest::Action::STATUS) {
        if (request.target == NetworkCommandTarget::WIFI) {
            std::printf("WIFI\n");
            std::printf("Configured       : %s\n", wifiCredentialsStore.isConfigured() ? "YES" : "NO");
            std::printf("Wi-Fi connection : %s\n", wifiStateText(wifiManager.getState()));
            std::printf("Wi-Fi channel    : %u\n", static_cast<unsigned int>(wifiManager.getConnectedChannel()));
            std::printf("Radio channel    : %u\n", static_cast<unsigned int>(communication.getChannel()));
            return commandResult(true, true, "status printed");
        }

        std::printf("MQTT\n");
        std::printf("Configured       : %s\n", mqttCredentialsStore.isConfigured() ? "YES" : "NO");
        std::printf("MQTT connection  : %s\n",
            mqttManager.getState() == MqttConnectionState::CONNECTED ? "CONNECTED" :
            (mqttManager.getState() == MqttConnectionState::CONNECTING ? "CONNECTING" : "DISCONNECTED"));
        return commandResult(true, true, "status printed");
    }

    return consoleNetwork(context, request);
}

void consoleBatteryReadable(void*)
{
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        std::printf("BATTERY: BUSY\n");
        return;
    }

    const auto& configuration = centralConfigurationStore.getConfiguration();
    const auto& battery = configuration.batterySensor;
    const auto& planning = configuration.powerPlanning;

    const bool simulating = batteryMonitor.isSimulationEnabled();
    const bool ina219Detected =
        battery.ina219Configured && batteryMonitor.isHardwareSensorPresent();

    const char* ina219Status = "NOT CONFIGURED";
    if (battery.ina219Configured) {
        if (simulating) {
            ina219Status = "CONFIGURED (SIMULATION ACTIVE)";
        } else {
            ina219Status = ina219Detected ? "DETECTED" : "NOT DETECTED";
        }
    }

    const char* measurementSource = "NONE";
    if (simulating) {
        measurementSource = "SIMULATED";
    } else if (!battery.ina219Configured) {
        measurementSource = "INA219 NOT CONFIGURED";
    } else if (!ina219Detected) {
        measurementSource = "INA219 NOT DETECTED";
    } else {
        measurementSource = "INA219";
    }

    std::printf("BATTERY MONITOR\n");
    std::printf("INA219             : %s\n", ina219Status);
    std::printf("Battery setup      : %s\n",
        battery.batteryMetadataConfigured ? "CONFIGURED" : "NOT CONFIGURED");
    std::printf("Measurement source : %s\n", measurementSource);
    if (batteryReadingValid) {
        std::printf("Voltage            : %.3f V\n", static_cast<double>(latestBatteryMeasurements.voltageVolts));
        std::printf("Current            : %.3f A\n", static_cast<double>(latestBatteryMeasurements.currentAmps));
        std::printf("P_measured         : %.3f W\n", static_cast<double>(latestBatteryMeasurements.powerWatts));
    } else {
        std::printf("Voltage            : --\n");
        std::printf("Current            : --\n");
        std::printf("P_measured         : --\n");
    }

    if (batteryMonitor.isStateOfChargeValid()) {
        std::printf("State of Charge    : %.2f %%\n", static_cast<double>(batteryMonitor.getStateOfChargePercent()));
        std::printf("SoC source         : %s\n", stateOfChargeSourceText(batteryMonitor.getStateOfChargeSource()));
    } else {
        std::printf("State of Charge    : --\n");
        std::printf("SoC source         : UNKNOWN\n");
    }

    std::printf("Power planning     : %s\n", planning.configured ? "CONFIGURED" : "NOT CONFIGURED");
    if (planning.configured) {
        std::printf("P_budget           : %.2f W\n", static_cast<double>(planning.P_budget));
        std::printf("P_reserve          : %.2f W\n", static_cast<double>(planning.P_reserve));
        std::printf("Minimum SoC        : %.1f %%\n", static_cast<double>(planning.minimumStateOfChargePercent));
        if (planning.requiredRuntimeHours > 0.0F) {
            if (!battery.batteryMetadataConfigured) {
                std::printf("Required runtime   : INVALID - battery setup not configured\n");
            } else {
                std::printf("Required runtime   : %.2f h | %.2f h remaining\n",
                    static_cast<double>(planning.requiredRuntimeHours),
                    static_cast<double>(batteryMonitor.getRemainingRequiredRuntimeHours()));
            }
        } else {
            std::printf("Required runtime   : NOT CONFIGURED\n");
        }
    }

    xSemaphoreGive(stateMutex);
}

CommandResult consoleConfigurePowerPlanningValidated(
    void* context,
    const PowerPlanningCommandRequest& request)
{
    if (request.requiredRuntimeHours > 0.0F) {
        const auto& battery = centralConfigurationStore.getConfiguration().batterySensor;
        if (!battery.batteryMetadataConfigured ||
            battery.batteryCapacityAmpHours <= 0.0F ||
            battery.nominalVoltageVolts <= 0.0F) {
            return commandResult(
                false,
                false,
                "runtime requires battery setup (capacity and voltage); existing power plan was not changed");
        }

        if (!batteryMonitor.isStateOfChargeValid()) {
            return commandResult(
                false,
                false,
                "runtime requires a valid battery state of charge; existing power plan was not changed");
        }
    }

    return consoleConfigurePowerPlanning(context, request);
}

} // namespace

void CentralApplication::runApp()
{
    esp_log_level_set("*", ESP_LOG_NONE);

    applyRadioChannelOverride();
    communication.setLocalNodeName(CentralNodeConfig::CENTRAL_NODE_NAME);

    if (!communication.initialize() || !communication.setAsCentralNode()) {
        ESP_LOGE(TAG, "startup: ESP-NOW communication initialization failed");
        return;
    }

    stateMutex = xSemaphoreCreateMutex();
    optimizationTriggerSemaphore = xSemaphoreCreateBinary();
    if (stateMutex == nullptr || optimizationTriggerSemaphore == nullptr) {
        ESP_LOGE(TAG, "startup: synchronization allocation failed");
        return;
    }

    currentTimeProvider.initializeTimeSynchronization();

    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();
    configureLocalHardware(localMac);

    centralConfigurationStore.loadPersisted();
    loadOptimizerIntervalConfiguration();
    applyPersistedBatterySensorConfiguration();
    centralLoadConfigurationStore.loadPersisted();
    applyStoredLoadSettings();
    commissioningRegistry.loadPersisted();

    char chipModel[NodeCommissioningRegistry::CHIP_MODEL_BUFFER_SIZE]{};
    chipInfo.getChipModelText(chipModel, sizeof(chipModel));
    commissioningRegistry.registerSelf(
        localMac,
        NodeRole::CENTRAL,
        CentralNodeConfig::CENTRAL_NODE_NAME,
        KILOWATTS_FIRMWARE_VERSION,
        chipModel,
        nullptr,
        0U,
        static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));

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
    commissioningRegistry.updateDiagnostics(localMac, diagnostics);
    commissioningRegistry.persist();

    WiFiCredentialsStore::Credentials wifi{};
    if (wifiCredentialsStore.load(wifi)) {
        if (!wifiManager.begin(WiFiManager::Credentials{
                wifi.ssid,
                wifi.password,
                CentralNodeConfig::WIFI_STATION_HOSTNAME})) {
            ESP_LOGE(TAG, "startup: Wi-Fi initialization failed");
        }
    } else if (!wifiProvisioningPortal.begin(communication.getChannel())) {
        ESP_LOGE(TAG, "startup: Wi-Fi provisioning portal failed");
    }

    // Frontend MQTT commands are operational only.
    mqttManager.setLoadCommandHandler(&handleLoadCommand, nullptr);
    mqttManager.setSystemCommandHandler(&handleSystemCommand, nullptr);
    mqttManager.setConfigCommandHandler(&handleConfigCommand, nullptr);
    mqttManager.setSimulationCommandHandler(&handleSimulationCommand, nullptr);
    mqttManager.setPowerPlanningCommandHandler(&consoleConfigurePowerPlanningValidated, nullptr);

    CentralConsole::Callbacks consoleCallbacks{};
    consoleCallbacks.status = &consoleStatus;
    consoleCallbacks.dashboard = &consoleDashboard;
    consoleCallbacks.batteryStatus = &consoleBatteryReadable;
    consoleCallbacks.nodes = &consoleNodes;
    consoleCallbacks.nodeStatus = &consoleNodeStatus;
    consoleCallbacks.loads = &consoleLoads;
    consoleCallbacks.loadStatus = &consoleLoadStatus;
    consoleCallbacks.optimize = &consoleOptimize;
    consoleCallbacks.sensorMode = &consoleSensorMode;
    consoleCallbacks.localMac = &consoleLocalMac;
    consoleCallbacks.configureIna219 = &consoleConfigureIna219;
    consoleCallbacks.configureBatterySetup = &consoleConfigureBatterySetup;
    consoleCallbacks.configurePowerPlanning = &consoleConfigurePowerPlanningValidated;
    consoleCallbacks.nodeCommand = &consoleNodeCommand;
    consoleCallbacks.configureLoad = &consoleConfigureLoad;
    consoleCallbacks.removeLoad = &consoleRemoveLoad;
    consoleCallbacks.loadCommand = &consoleLoadCommand;
    consoleCallbacks.network = &consoleNetworkReadable;
    consoleCallbacks.simulation = &consoleSimulation;
    consoleCallbacks.system = &consoleSystem;
    consoleCallbacks.context = nullptr;

    if (!centralConsole.begin(consoleCallbacks)) {
        ESP_LOGE(TAG, "startup: serial console initialization failed");
    }

    if (xTaskCreate(sensorAcquisitionTask, "sensor", 4096U, nullptr, 5U, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "startup: failed to create sensor acquisition task");
    }
    if (xTaskCreate(espNowCommunicationTask, "espnow", 6144U, nullptr, 5U, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "startup: failed to create ESP-NOW task");
    }
    if (xTaskCreate(optimizationTask, "planner", 8192U, nullptr, 6U, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "startup: failed to create planning task");
    }
    if (xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "startup: failed to create watchdog task");
    }

    printCentralBootSummary(localMac);
}
