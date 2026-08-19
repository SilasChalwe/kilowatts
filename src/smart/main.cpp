#if defined(DEVICE_ROLE_SMART)
#include "namespace.h"




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
     * A Node with no valid persisted commissioning record boots
     * UNCOMMISSIONED (NodeIdentityStore's own default) and advertises an
     * automatically derived name; loadPersisted() restores a previously
     * commissioned identity/name instead when one exists.
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

    nodeMutex = xSemaphoreCreateMutex();
    identityMutex = xSemaphoreCreateMutex();
    relayCommandQueue = xQueueCreate(8U, sizeof(RelayCommandQueueItem));
    thisSmartNode = new Node(localMac);

    const bool hardwareConfigurationRestored = smartNodeConfigurationStore.loadPersisted();
    HardwareConfigurationFailureReason restoreFailure = HardwareConfigurationFailureReason::NONE;
    const bool canRestoreHardware =
        identityStore.getLifecycleState() == NodeLifecycleState::COMMISSIONED ||
        identityStore.getLifecycleState() == NodeLifecycleState::OPERATIONAL;
    if (hardwareConfigurationRestored && canRestoreHardware &&
        persistedRelayConfigurationMatchesBoardProfile()) {
        if (!smartNodeConfigurationStore.applyPersistedConfigurations(
                relays, *thisSmartNode, restoreFailure)) {
            ESP_LOGE(TAG, "NVS_RESTORE: Smart Node hardware configuration could not be applied (reason=%u)",
                     static_cast<unsigned int>(restoreFailure));
        }
    } else if (hardwareConfigurationRestored) {
        ESP_LOGW(TAG, "NVS_RESTORE: retaining hardware configuration without applying it while Node is uncommissioned or the board profile is not verified");
    }
    ESP_LOGI(TAG, "NVS_RESTORE: hardwareConfiguration=%s configuredLoads=%u",
             hardwareConfigurationRestored ? "RESTORED" : "ABSENT",
             static_cast<unsigned int>(smartNodeConfigurationStore.getNumberOfConfigurations()));
    ESP_LOGI(TAG, "LOAD_POWER_SOURCE: installer ratings only (no per-load INA219 on Smart Nodes)");
    sendIdentityReport();

    /*
     * Local functionality (relay control and watchdog) starts immediately
     * and unconditionally — it never waits for a successful upstream
     * ESP-NOW discovery first. Discovery itself is retried asynchronously
     * inside espNowCommunicationTask() once it starts running, without
     * blocking any other task; if Central stays unreachable, relay/
     * watchdog simply keep running on their own with the last safe relay
     * state held.
     */
    xTaskCreate(relayControlTask, "relay_ctrl", 4096U, nullptr, 6U, nullptr);
    xTaskCreate(espNowCommunicationTask, "espnow_app", 4096U, nullptr, 5U, nullptr);
    xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr);
    xTaskCreate(consoleTask, "console", 4096U, nullptr, 3U, nullptr);

    ESP_LOGI(TAG, "%s is ready: relay control and ESP-NOW tasks running", communication.getLocalNodeName());
    printSmartBootSummary(localMac);
}

#endif // DEVICE_ROLE_SMART
