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
    mqttManager.setPowerPlanningCommandHandler(&consoleConfigurePowerPlanning, nullptr);

    CentralConsole::Callbacks consoleCallbacks{};
    consoleCallbacks.status = &consoleStatus;
    consoleCallbacks.dashboard = &consoleDashboard;
    consoleCallbacks.batteryStatus = &consoleBatteryStatus;
    consoleCallbacks.nodes = &consoleNodes;
    consoleCallbacks.nodeStatus = &consoleNodeStatus;
    consoleCallbacks.loads = &consoleLoads;
    consoleCallbacks.loadStatus = &consoleLoadStatus;
    consoleCallbacks.optimize = &consoleOptimize;
    consoleCallbacks.sensorMode = &consoleSensorMode;
    consoleCallbacks.localMac = &consoleLocalMac;
    consoleCallbacks.configureBattery = &consoleConfigureBattery;
    consoleCallbacks.configurePowerPlanning = &consoleConfigurePowerPlanning;
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
