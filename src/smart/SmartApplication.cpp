#include "SmartApplication.h"

#if defined(DEVICE_ROLE_SMART)

#include "namespace.h"

void SmartApplication::runApp()
{
    // Keep UART clean: Smart background tasks must not print unless explicitly requested.
    esp_log_level_set("*", ESP_LOG_NONE);

    if (!communication.initialize()) {
        ESP_LOGE(TAG, "startup: communication.initialize() failed");
        return;
    }

    currentTimeProvider.initializeTimeSynchronization();

    nodeMutex = xSemaphoreCreateMutex();
    identityMutex = xSemaphoreCreateMutex();
    relayCommandQueue = xQueueCreate(8U, sizeof(RelayCommandQueueItem));
    if (nodeMutex == nullptr || identityMutex == nullptr || relayCommandQueue == nullptr) {
        ESP_LOGE(TAG, "startup: runtime allocation failed (nodeMutex=%s, identityMutex=%s, relayQueue=%s)",
                 nodeMutex != nullptr ? "ok" : "failed",
                 identityMutex != nullptr ? "ok" : "failed",
                 relayCommandQueue != nullptr ? "ok" : "failed");
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
            ESP_LOGE(TAG, "startup: persisted load restore failed (reason=%u)",
                     static_cast<unsigned int>(reason));
        }
    }

    sendIdentityReport();

    if (!startSmartConsole()) {
        ESP_LOGE(TAG, "startup: console REPL start failed");
    }

    xTaskCreate(relayControlTask, "pin_control", 4096U, nullptr, 6U, nullptr);
    xTaskCreate(espNowCommunicationTask, "espnow", 6144U, nullptr, 5U, nullptr);
    xTaskCreate(watchdogTask, "watchdog", 3072U, nullptr, 2U, nullptr);

    printSmartBootSummary(localMac);
}

#endif
