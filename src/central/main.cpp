#if defined(DEVICE_ROLE_CENTRAL)
#include "namespace.h"

extern "C" void app_main()
{
    communication.setLocalNodeName(CentralNodeConfig::CENTRAL_NODE_NAME);

    if (!communication.initialize() || !communication.setAsCentralNode()) {
        ESP_LOGE(TAG, "Central communication failed to start");
        return;
    }

    stateMutex = xSemaphoreCreateMutex();
    optimizationTriggerSemaphore = xSemaphoreCreateBinary();
    if (stateMutex == nullptr || optimizationTriggerSemaphore == nullptr) {
        ESP_LOGE(TAG, "Could not create Central synchronization objects");
        return;
    }

    currentTimeProvider.initializeTimeSynchronization();

    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();
    configureLocalHardware(localMac);

    centralConfigurationStore.loadPersisted();
    applyPersistedBatterySensorConfiguration();
    loadConfigurationStore.loadPersisted();
    commissioningRegistry.loadPersisted();

    char chipModel[NodeCommissioningRegistry::CHIP_MODEL_BUFFER_SIZE]{};
    chipInfo.getChipModelText(chipModel, sizeof(chipModel));
    /* No compiled-in safe-pin list to declare anymore — the installer names a relay pin directly per Load. */
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
        wifiManager.begin(WiFiManager::Credentials{
            wifi.ssid,
            wifi.password,
            CentralNodeConfig::WIFI_STATION_HOSTNAME});
    } else {
        wifiProvisioningPortal.begin(kilowatts::KILOWATTS_RADIO_CHANNEL);
    }

    mqttManager.setLoadCommandHandler(&handleLoadCommand, nullptr);
    mqttManager.setSystemCommandHandler(&handleSystemCommand, nullptr);
    mqttManager.setConfigCommandHandler(&handleConfigCommand, nullptr);

    xTaskCreate(sensorAcquisitionTask, "sensor", 4096U, nullptr, 5U, nullptr);
    xTaskCreate(espNowCommunicationTask, "espnow", 6144U, nullptr, 5U, nullptr);
    xTaskCreate(optimizationTask, "planner", 8192U, nullptr, 6U, nullptr);
    xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr);

    printCentralBootSummary(localMac);
    ESP_LOGI(TAG, "Central ready");
}

#endif // DEVICE_ROLE_CENTRAL
