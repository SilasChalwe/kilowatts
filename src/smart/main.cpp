#if defined(DEVICE_ROLE_SMART)
#include "namespace.h"

extern "C" void app_main()
{
    if (!communication.initialize()) {
        ESP_LOGE(TAG, "Smart Node communication failed to start");
        return;
    }

    currentTimeProvider.initializeTimeSynchronization();

    nodeMutex = xSemaphoreCreateMutex();
    identityMutex = xSemaphoreCreateMutex();
    relayCommandQueue = xQueueCreate(8U, sizeof(RelayCommandQueueItem));
    if (nodeMutex == nullptr || identityMutex == nullptr || relayCommandQueue == nullptr) {
        ESP_LOGE(TAG, "Could not create Smart Node runtime objects");
        return;
    }

    const EspNowCommunication::MacAddress localMac = communication.getLocalMacAddress();

    identityStore.loadPersisted();
    char automaticName[EspNowCommunication::NODE_NAME_LENGTH]{};
    computeAutomaticNodeName(localMac, automaticName, sizeof(automaticName));
    communication.setLocalNodeName(
        isCommissioned() ? identityStore.getFriendlyName() : automaticName);

    thisSmartNode = new Node(localMac);

    if (smartNodeConfigurationStore.loadPersisted() && isCommissioned()) {
        HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
        if (!smartNodeConfigurationStore.applyPersistedConfigurations(
                relays, *thisSmartNode, reason)) {
            ESP_LOGE(TAG, "Could not restore Smart Node loads");
        }
    }

    sendIdentityReport();

    xTaskCreate(relayControlTask, "pin_control", 4096U, nullptr, 6U, nullptr);
    xTaskCreate(espNowCommunicationTask, "espnow", 6144U, nullptr, 5U, nullptr);
    xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr);

    printSmartBootSummary(localMac);
    ESP_LOGI(TAG, "Smart Node ready");
}

#endif // DEVICE_ROLE_SMART
