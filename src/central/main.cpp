#if defined(DEVICE_ROLE_CENTRAL)
#include "namespace.h"





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
     * batteryStateOfCharge is deliberately NOT initialize()d here: with no
     * battery sensor configured, isValid() must honestly report
     * false/UNKNOWN. initialize() only ever runs after a real
     * installer-verified sensor exists.
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
                                            CentralNodeConfig::VERIFIED_RELAY_GPIO_PINS.data(),
                                            CentralNodeConfig::VERIFIED_RELAY_GPIO_PINS.size(),
                                            static_cast<std::uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())))) {
        commissioningRegistry.persist();
    }

    /*
     * Central never sends itself an IdentityReportPacket over ESP-NOW, so
     * unlike a Smart Node its Diagnostics is only ever a boot-time
     * snapshot here - never refreshed again for the life of this boot.
     * Good enough for a first version; a genuinely live reading would need
     * a periodic self-refresh alongside the ~2s state publish cycle.
     */
    {
        NodeCommissioningRegistry::Diagnostics selfDiagnostics{};
        selfDiagnostics.freeHeapBytes = chipInfo.getFreeHeapBytes();
        selfDiagnostics.minFreeHeapBytes = chipInfo.getMinFreeHeapBytes();
        selfDiagnostics.flashSizeBytes = chipInfo.getFlashSizeBytes();
        selfDiagnostics.psramSizeBytes = chipInfo.getPsramSizeBytes();
        selfDiagnostics.siliconRevision = static_cast<std::uint16_t>(chipInfo.getSiliconRevision());
        selfDiagnostics.cpuCores = static_cast<std::uint8_t>(chipInfo.getCpuCores());
        chipInfo.getResetReasonText(selfDiagnostics.resetReason, sizeof(selfDiagnostics.resetReason));
        commissioningRegistry.updateDiagnostics(localMac, selfDiagnostics);
    }

    /*
     * Wi-Fi/MQTT are best-effort: their absence must never prevent local
     * sensing, Best-First Search, ESP-NOW or relay control from working.
     *
     * A never-provisioned device does not know any site's Wi-Fi and must
     * not guess: it skips WiFiManager entirely and comes straight up as
     * WiFiProvisioningPortal's self-hosted setup Access Point so an
     * installer can supply a real SSID/password from a phone.
     * KilowattsSecrets.h no longer carries a compiled-in Wi-Fi
     * SSID/password fallback for this reason. Once an installer has
     * provisioned real credentials (WiFiCredentialsStore), every
     * subsequent boot uses those with WiFiManager as normal; watchdogTask's
     * checkWiFiProvisioningTrigger() brings the portal back if those
     * provisioned credentials later stop working.
     */
    WiFiCredentialsStore::Credentials activeWiFiCredentials{};
    if (wifiCredentialsStore.load(activeWiFiCredentials)) {
        ESP_LOGI(TAG, "WIFI_CREDENTIALS: using installer-provisioned SSID '%s'", activeWiFiCredentials.ssid);
        if (!wifiManager.begin(WiFiManager::Credentials{
                activeWiFiCredentials.ssid, activeWiFiCredentials.password, CentralNodeConfig::WIFI_STATION_HOSTNAME})) {
            ESP_LOGW(TAG, "Wi-Fi did not start; continuing with local-only control");
        }
    } else {
        ESP_LOGI(TAG, "WIFI_CREDENTIALS: none provisioned yet; starting self-hosted setup Access Point");
        if (!wifiProvisioningPortal.begin(kilowatts::KILOWATTS_RADIO_CHANNEL)) {
            ESP_LOGW(TAG, "Setup Access Point did not start; continuing with local-only control");
        }
    }

    /*
     * MQTT itself is not started here - checkMqttStartTrigger() (called
     * from optimizationTask()) starts it the moment Wi-Fi first reports a
     * real IP address, per MqttManager::begin()'s own documented contract.
     * Command handlers are registered up front regardless, so nothing is
     * missed once the client does start.
     */
    mqttManager.setLoadCommandHandler(&handleLoadCommand, nullptr);
    mqttManager.setSystemCommandHandler(&handleSystemCommand, nullptr);
    mqttManager.setConfigCommandHandler(&handleConfigCommand, nullptr);

    xTaskCreate(sensorAcquisitionTask, "sensor_acq", 4096U, nullptr, 5U, nullptr);
    xTaskCreate(espNowCommunicationTask, "espnow_app", 4096U, nullptr, 5U, nullptr);
    xTaskCreate(optimizationTask, "optimization", 8192U, nullptr, 6U, nullptr);
    xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr);
    xTaskCreate(consoleTask, "console", 4096U, nullptr, 3U, nullptr);

    printCentralBootSummary(localMac);
    ESP_LOGI(TAG, "Central is ready: sensing, ESP-NOW, optimisation and watchdog tasks running");
}

#endif // DEVICE_ROLE_CENTRAL
