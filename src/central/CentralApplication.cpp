#include "namespace.h"
#include "CentralApplication.h"

void CentralApplication::runApp()
{
    /*
     * Every operation on the Central node's UART is user-run: the
     * "kilowatts>" prompt only ever shows output the user asked for by
     * typing a command. ESP-NOW/Wi-Fi/time-sync/config-store setup below
     * all run before the console exists and would otherwise print
     * unprompted boot noise ahead of it - silence logging for the whole
     * node from the first line, not just once the console starts.
     */
    esp_log_level_set("*", ESP_LOG_NONE);

    applyRadioChannelOverride();

    communication.setLocalNodeName(CentralNodeConfig::CENTRAL_NODE_NAME);

    if (!communication.initialize()) {
        ESP_LOGE(TAG, "startup: communication.initialize() failed");
        return;
    }
    if (!communication.setAsCentralNode()) {
        ESP_LOGE(TAG, "startup: communication.setAsCentralNode() failed");
        return;
    }

    stateMutex = xSemaphoreCreateMutex();
    optimizationTriggerSemaphore = xSemaphoreCreateBinary();
    if (stateMutex == nullptr || optimizationTriggerSemaphore == nullptr) {
        ESP_LOGE(TAG, "startup: synchronization allocation failed (stateMutex=%s, optimizationTrigger=%s)",
                 stateMutex != nullptr ? "ok" : "failed",
                 optimizationTriggerSemaphore != nullptr ? "ok" : "failed");
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
            ESP_LOGE(TAG, "startup: wifiManager.begin() failed; Wi-Fi/MQTT will remain unavailable this boot");
        }
    } else if (!wifiProvisioningPortal.begin(communication.getChannel())) {
        ESP_LOGE(TAG, "startup: wifiProvisioningPortal.begin() failed; no way to provision Wi-Fi this boot");
    }

    mqttManager.setLoadCommandHandler(&handleLoadCommand, nullptr);
    mqttManager.setSystemCommandHandler(&handleSystemCommand, nullptr);
    mqttManager.setConfigCommandHandler(&handleConfigCommand, nullptr);
    mqttManager.setSimulationCommandHandler(&handleSimulationCommand, nullptr);

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
    consoleCallbacks.configurePowerLimits = &consoleConfigurePowerLimits;
    consoleCallbacks.nodeCommand = &consoleNodeCommand;
    consoleCallbacks.configureLoad = &consoleConfigureLoad;
    consoleCallbacks.removeLoad = &consoleRemoveLoad;
    consoleCallbacks.loadCommand = &consoleLoadCommand;
    consoleCallbacks.network = &consoleNetwork;
    consoleCallbacks.simulation = &consoleSimulation;
    consoleCallbacks.system = &consoleSystem;
    consoleCallbacks.context = nullptr;

    if (!centralConsole.begin(consoleCallbacks)) {
        ESP_LOGE(TAG, "startup: centralConsole.begin() failed; the serial console will be unavailable this boot");
    }

    if (xTaskCreate(sensorAcquisitionTask, "sensor", 4096U, nullptr, 5U, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "startup: failed to create the sensor acquisition task");
    }
    if (xTaskCreate(espNowCommunicationTask, "espnow", 6144U, nullptr, 5U, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "startup: failed to create the ESP-NOW communication task");
    }
    if (xTaskCreate(optimizationTask, "planner", 8192U, nullptr, 6U, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "startup: failed to create the optimization task; planning/safety loop will not run");
    }
    if (xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "startup: failed to create the watchdog task");
    }

    printCentralBootSummary(localMac);
}