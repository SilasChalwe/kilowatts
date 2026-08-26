/**
 * @file EspNowCommunication.cpp
 * @brief Implements dynamic ESP-NOW communication for the Kilowatts tree network.
 */

#include "EspNowCommunication.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"


static const char *TAG = "ESP_NOW_COMM";


namespace kilowatts {


namespace {

void formatMacAddressForDisplay(
    char* output,
    std::size_t outputSize,
    const EspNowCommunication::MacAddress& macAddress)
{
    if (output == nullptr || outputSize == 0U)
    {
        return;
    }

    std::snprintf(
        output,
        outputSize,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        macAddress[0],
        macAddress[1],
        macAddress[2],
        macAddress[3],
        macAddress[4],
        macAddress[5]
    );
}

} // namespace


EspNowCommunication*
EspNowCommunication::instance_ = nullptr;


const EspNowCommunication::MacAddress
EspNowCommunication::BROADCAST_MAC_ADDRESS = {
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF
};


EspNowCommunication::EspNowCommunication(
    std::uint8_t channel
)
    : localNodeName_{},
      localMacAddress_{},
      channel_(channel),
      initialized_(false),
      centralNode_(false),
      upstreamNodeSelected_(false),
      upstreamNodeMacAddress_{},
      upstreamNodeSignalStrengthDbm_(-128),
      centralMacAddressKnown_(false),
      centralMacAddress_{},
      hopCountToCentral_(UNKNOWN_HOP_COUNT),
      directDownstreamNodes_{},
      discoveryInProgress_(false),
      activeDiscoveryId_(0U),
      discoveryCandidateFound_(false),
      bestUpstreamCandidateMacAddress_{},
      bestUpstreamCandidateSignalStrengthDbm_(-128),
      bestUpstreamCandidateHopCount_(UNKNOWN_HOP_COUNT),
      bestUpstreamCandidateCentralMacAddress_{},
      nextMessageId_(1U),
      sendCompleted_(nullptr),
      sendMutex_(nullptr),
      discoveryMutex_(nullptr),
      directDownstreamNodesMutex_(nullptr),
      rawReceivedPackets_(nullptr),
      applicationMessages_(nullptr),
      serviceTask_(nullptr),
      lastSendStatus_(ESP_NOW_SEND_FAIL)
{
    std::snprintf(
        localNodeName_.data(),
        localNodeName_.size(),
        "%s",
        "Unnamed"
    );
}


EspNowCommunication::~EspNowCommunication()
{
    if (serviceTask_ != nullptr)
    {
        vTaskDelete(serviceTask_);
        serviceTask_ = nullptr;
    }

    if (initialized_)
    {
        esp_now_unregister_send_cb();
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        initialized_ = false;
    }

    if (rawReceivedPackets_ != nullptr)
    {
        vQueueDelete(rawReceivedPackets_);
        rawReceivedPackets_ = nullptr;
    }

    if (applicationMessages_ != nullptr)
    {
        vQueueDelete(applicationMessages_);
        applicationMessages_ = nullptr;
    }

    if (sendCompleted_ != nullptr)
    {
        vSemaphoreDelete(sendCompleted_);
        sendCompleted_ = nullptr;
    }

    if (sendMutex_ != nullptr)
    {
        vSemaphoreDelete(sendMutex_);
        sendMutex_ = nullptr;
    }

    if (discoveryMutex_ != nullptr)
    {
        vSemaphoreDelete(discoveryMutex_);
        discoveryMutex_ = nullptr;
    }

    if (directDownstreamNodesMutex_ != nullptr)
    {
        vSemaphoreDelete(directDownstreamNodesMutex_);
        directDownstreamNodesMutex_ = nullptr;
    }

    if (instance_ == this)
    {
        instance_ = nullptr;
    }
}


void EspNowCommunication::setLocalNodeName(
    const char* nodeName
)
{
    const char* nameToStore =
        nodeName != nullptr &&
        nodeName[0] != '\0'
            ? nodeName
            : "Unnamed";

    std::snprintf(
        localNodeName_.data(),
        localNodeName_.size(),
        "%s",
        nameToStore
    );
}


const char* EspNowCommunication::getLocalNodeName() const
{
    return localNodeName_.data();
}


bool EspNowCommunication::registerDirectDownstreamNode(
    const char* nodeName,
    const MacAddress& nodeMacAddress,
    std::uint16_t hopCountToCentral
)
{
    bool centralMacAddressKnown = false;
    std::uint16_t localHopCountToCentral = UNKNOWN_HOP_COUNT;
    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        centralMacAddressKnown = centralMacAddressKnown_;
        localHopCountToCentral = hopCountToCentral_;
        xSemaphoreGive(discoveryMutex_);
    }

    if (!initialized_ ||
        !centralMacAddressKnown ||
        localHopCountToCentral == UNKNOWN_HOP_COUNT)
    {
        ESP_LOGW(
            TAG,
            "Cannot register direct downstream Node before this Node has a valid route to Central"
        );

        return false;
    }

    const std::uint16_t expectedHopCount =
        static_cast<std::uint16_t>(
            localHopCountToCentral + 1U
        );

    if (hopCountToCentral != expectedHopCount)
    {
        ESP_LOGW(
            TAG,
            "Cannot register direct downstream Node: reported hop=%u expected=%u",
            static_cast<unsigned int>(
                hopCountToCentral
            ),
            static_cast<unsigned int>(
                expectedHopCount
            )
        );

        return false;
    }

    return registerOrUpdateDirectDownstreamNode(
        nodeName,
        nodeMacAddress,
        0,
        false,
        hopCountToCentral
    );
}


bool EspNowCommunication::registerDirectDownstreamNode(
    const char* nodeName,
    const MacAddress& nodeMacAddress,
    std::int8_t signalStrengthDbm,
    std::uint16_t hopCountToCentral
)
{
    bool centralMacAddressKnown = false;
    std::uint16_t localHopCountToCentral = UNKNOWN_HOP_COUNT;
    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        centralMacAddressKnown = centralMacAddressKnown_;
        localHopCountToCentral = hopCountToCentral_;
        xSemaphoreGive(discoveryMutex_);
    }

    if (!initialized_ ||
        !centralMacAddressKnown ||
        localHopCountToCentral == UNKNOWN_HOP_COUNT)
    {
        ESP_LOGW(
            TAG,
            "Cannot register direct downstream Node before this Node has a valid route to Central"
        );

        return false;
    }

    const std::uint16_t expectedHopCount =
        static_cast<std::uint16_t>(
            localHopCountToCentral + 1U
        );

    if (hopCountToCentral != expectedHopCount)
    {
        ESP_LOGW(
            TAG,
            "Cannot register direct downstream Node: reported hop=%u expected=%u",
            static_cast<unsigned int>(hopCountToCentral),
            static_cast<unsigned int>(expectedHopCount)
        );

        return false;
    }

    return registerOrUpdateDirectDownstreamNode(
        nodeName,
        nodeMacAddress,
        signalStrengthDbm,
        true,
        hopCountToCentral
    );
}


bool EspNowCommunication::initialize()
{
    if (initialized_)
    {
        ESP_LOGW(
            TAG,
            "ESP-NOW communication is already initialized"
        );

        return true;
    }

    if (instance_ != nullptr &&
        instance_ != this)
    {
        ESP_LOGE(
            TAG,
            "Only one EspNowCommunication instance may be active on one ESP32"
        );

        return false;
    }

    if (channel_ == 0U ||
        channel_ > 14U)
    {
        ESP_LOGE(
            TAG,
            "Invalid ESP-NOW channel: %u",
            static_cast<unsigned int>(channel_)
        );

        return false;
    }

    if (!initializeNVS())
    {
        return false;
    }

    if (!initializeWiFi())
    {
        return false;
    }

    if (!chipInfo_.getMacAddress(
            localMacAddress_
        ))
    {
        ESP_LOGE(
            TAG,
            "Initialization failed because local MAC address could not be read"
        );

        return false;
    }

    char localMacAddressText[18]{};
    formatMacAddressForDisplay(localMacAddressText, sizeof(localMacAddressText), localMacAddress_);
    ESP_LOGI(TAG, "Local Node MAC=%s", localMacAddressText);

    sendCompleted_ =
        xSemaphoreCreateBinary();

    sendMutex_ =
        xSemaphoreCreateMutex();

    discoveryMutex_ =
        xSemaphoreCreateMutex();

    directDownstreamNodesMutex_ =
        xSemaphoreCreateMutex();

    rawReceivedPackets_ =
        xQueueCreate(
            12U,
            sizeof(RawReceivedPacket)
        );

    applicationMessages_ =
        xQueueCreate(
            12U,
            sizeof(ReceivedMessage)
        );

    if (sendCompleted_ == nullptr ||
        sendMutex_ == nullptr ||
        discoveryMutex_ == nullptr ||
        directDownstreamNodesMutex_ == nullptr ||
        rawReceivedPackets_ == nullptr ||
        applicationMessages_ == nullptr)
    {
        ESP_LOGE(
            TAG,
            "Could not allocate ESP-NOW synchronization resources"
        );

        return false;
    }

    instance_ = this;

    if (!initializeEspNow())
    {
        instance_ = nullptr;
        return false;
    }

    if (!addBroadcastPeer())
    {
        return false;
    }

    const BaseType_t taskCreated =
        xTaskCreate(
            serviceTaskEntry,
            "espnow_service",
            4096U,
            this,
            5U,
            &serviceTask_
        );

    if (taskCreated != pdPASS)
    {
        ESP_LOGE(
            TAG,
            "Could not create ESP-NOW service task"
        );

        return false;
    }

    initialized_ = true;

    ESP_LOGI(
        TAG,
        "ESP-NOW initialized on channel %u",
        static_cast<unsigned int>(channel_)
    );

    return true;
}


bool EspNowCommunication::setAsCentralNode()
{
    if (!initialized_)
    {
        ESP_LOGW(
            TAG,
            "Cannot mark Central Node before ESP-NOW initialization"
        );

        return false;
    }

    if (xSemaphoreTake(discoveryMutex_, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Cannot establish Central route state because the route lock failed");
        return false;
    }

    centralNode_ = true;
    upstreamNodeSelected_ = false;
    upstreamNodeMacAddress_.fill(0U);

    centralMacAddress_ =
        localMacAddress_;

    centralMacAddressKnown_ = true;

    hopCountToCentral_ = 0U;

    xSemaphoreGive(discoveryMutex_);

    if (directDownstreamNodesMutex_ != nullptr &&
        xSemaphoreTake(
            directDownstreamNodesMutex_,
            portMAX_DELAY
        ) == pdTRUE)
    {
        directDownstreamNodes_.clear();

        xSemaphoreGive(
            directDownstreamNodesMutex_
        );
    }

    char localMacAddressText[18]{};
    formatMacAddressForDisplay(localMacAddressText, sizeof(localMacAddressText), localMacAddress_);
    ESP_LOGI(TAG, "This ESP32 is the Central Node: MAC=%s hopCount=0", localMacAddressText);

    printConnectionInfo();

    return true;
}


bool EspNowCommunication::discoverUpstreamNode(
    std::uint32_t discoveryWindowMilliseconds
)
{
    if (!initialized_ ||
        centralNode_ ||
        discoveryWindowMilliseconds == 0U)
    {
        ESP_LOGW(
            TAG,
            "Upstream Node discovery cannot start: initialized=%s central=%s window=%u ms",
            initialized_ ? "Yes" : "No",
            centralNode_ ? "Yes" : "No",
            static_cast<unsigned int>(
                discoveryWindowMilliseconds
            )
        );

        return false;
    }

    if (xSemaphoreTake(
            discoveryMutex_,
            portMAX_DELAY
        ) != pdTRUE)
    {
        return false;
    }

    discoveryInProgress_ = true;

    activeDiscoveryId_ =
        esp_random();

    if (activeDiscoveryId_ == 0U)
    {
        activeDiscoveryId_ = 1U;
    }

    discoveryCandidateFound_ = false;

    bestUpstreamCandidateMacAddress_.fill(0U);

    bestUpstreamCandidateSignalStrengthDbm_ =
        -128;

    bestUpstreamCandidateHopCount_ =
        UNKNOWN_HOP_COUNT;

    bestUpstreamCandidateCentralMacAddress_.fill(
        0U
    );

    const std::uint32_t discoveryId =
        activeDiscoveryId_;

    xSemaphoreGive(
        discoveryMutex_
    );

    DiscoveryRequest request{
        discoveryId
    };

    ESP_LOGI(
        TAG,
        "Broadcasting Upstream Node discovery request: discoveryId=%u window=%u ms",
        static_cast<unsigned int>(
            discoveryId
        ),
        static_cast<unsigned int>(
            discoveryWindowMilliseconds
        )
    );

    if (!sendTo(
            BROADCAST_MAC_ADDRESS,
            BROADCAST_MAC_ADDRESS,
            MessageType::DISCOVERY_REQUEST,
            request,
            1000U
        ))
    {
        if (xSemaphoreTake(
                discoveryMutex_,
                portMAX_DELAY
            ) == pdTRUE)
        {
            discoveryInProgress_ = false;

            xSemaphoreGive(
                discoveryMutex_
            );
        }

        ESP_LOGE(
            TAG,
            "Discovery broadcast could not be sent"
        );

        return false;
    }

    vTaskDelay(
        toTicks(
            discoveryWindowMilliseconds
        )
    );

    if (xSemaphoreTake(
            discoveryMutex_,
            portMAX_DELAY
        ) != pdTRUE)
    {
        return false;
    }

    discoveryInProgress_ = false;

    const bool candidateFound =
        discoveryCandidateFound_;

    const MacAddress selectedUpstreamNode =
        bestUpstreamCandidateMacAddress_;

    const std::int8_t selectedRssi =
        bestUpstreamCandidateSignalStrengthDbm_;

    const std::uint16_t selectedUpstreamNodeHop =
        bestUpstreamCandidateHopCount_;

    const MacAddress selectedCentral =
        bestUpstreamCandidateCentralMacAddress_;

    xSemaphoreGive(
        discoveryMutex_
    );

    if (!candidateFound)
    {
        ESP_LOGW(
            TAG,
            "No valid upstream Node was discovered"
        );

        return false;
    }

    if (!addPeer(selectedUpstreamNode))
    {
        ESP_LOGE(
            TAG,
            "Nearest candidate was found but could not be added as a peer"
        );

        return false;
    }

    if (xSemaphoreTake(discoveryMutex_, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Discovered route could not be applied because the route lock failed");
        return false;
    }

    upstreamNodeMacAddress_ = selectedUpstreamNode;
    upstreamNodeSignalStrengthDbm_ = selectedRssi;
    upstreamNodeSelected_ = true;
    centralMacAddress_ = selectedCentral;
    centralMacAddressKnown_ = true;
    hopCountToCentral_ = static_cast<std::uint16_t>(selectedUpstreamNodeHop + 1U);

    xSemaphoreGive(discoveryMutex_);

    char selectedUpstreamNodeText[18]{};
    formatMacAddressForDisplay(
        selectedUpstreamNodeText,
        sizeof(selectedUpstreamNodeText),
        selectedUpstreamNode
    );
    ESP_LOGI(
        TAG,
        "Upstream Node selected: MAC=%s upstreamHopToCentral=%u RSSI=%d dBm thisNodeHopToCentral=%u",
        selectedUpstreamNodeText,
        static_cast<unsigned int>(
            selectedUpstreamNodeHop
        ),
        static_cast<int>(
            selectedRssi
        ),
        static_cast<unsigned int>(
            selectedUpstreamNodeHop + 1U
        )
    );

    if (!sendConnectionHandshakeToUpstreamNode())
    {
        ESP_LOGW(
            TAG,
            "Upstream Node was selected, but the connection handshake could not be delivered"
        );
    }

    printConnectionInfo();

    return true;
}


bool EspNowCommunication::addPeer(
    const MacAddress& peerMacAddress
)
{
    if (!initialized_ || instance_ != this)
    {
        ESP_LOGW(
            TAG,
            "Cannot add peer before this ESP-NOW communication instance is fully initialized"
        );

        return false;
    }

    if (hasPeer(peerMacAddress))
    {
        return true;
    }

    esp_now_peer_info_t peerInformation{};

    std::memcpy(
        peerInformation.peer_addr,
        peerMacAddress.data(),
        MAC_ADDRESS_LENGTH
    );

    peerInformation.channel =
        channel_;

    peerInformation.ifidx =
        WIFI_IF_STA;

    peerInformation.encrypt =
        false;

    const esp_err_t result =
        esp_now_add_peer(
            &peerInformation
        );

    char peerMacAddressText[18]{};
    formatMacAddressForDisplay(peerMacAddressText, sizeof(peerMacAddressText), peerMacAddress);

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not add peer %s error=%s",
            peerMacAddressText,
            esp_err_to_name(result)
        );

        return false;
    }

    ESP_LOGI(TAG, "Peer added: MAC=%s", peerMacAddressText);

    return true;
}


bool EspNowCommunication::removePeer(
    const MacAddress& peerMacAddress
)
{
    if (!hasPeer(peerMacAddress))
    {
        return true;
    }

    const esp_err_t result =
        esp_now_del_peer(
            peerMacAddress.data()
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not remove peer: error=%s",
            esp_err_to_name(result)
        );

        return false;
    }

    char peerMacAddressText[18]{};
    formatMacAddressForDisplay(peerMacAddressText, sizeof(peerMacAddressText), peerMacAddress);
    ESP_LOGI(TAG, "Peer removed: MAC=%s", peerMacAddressText);

    return true;
}


bool EspNowCommunication::hasPeer(
    const MacAddress& peerMacAddress
) const
{
    return esp_now_is_peer_exist(
        peerMacAddress.data()
    );
}


bool EspNowCommunication::sendTo(
    const MacAddress& nextHopMacAddress,
    const MacAddress& destinationMacAddress,
    MessageType messageType,
    const void* payload,
    std::size_t payloadLength,
    std::uint32_t timeoutMilliseconds
)
{
    Message message{};

    if (!buildMessage(
            message,
            destinationMacAddress,
            messageType,
            payload,
            payloadLength
        ))
    {
        return false;
    }

    return sendMessageTo(
        nextHopMacAddress,
        message,
        timeoutMilliseconds
    );
}


bool EspNowCommunication::sendToCentral(
    MessageType messageType,
    const void* payload,
    std::size_t payloadLength,
    std::uint32_t timeoutMilliseconds
)
{
    MacAddress upstreamNodeMacAddress{};
    MacAddress centralMacAddress{};
    bool routeAvailable = false;

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        routeAvailable = upstreamNodeSelected_ && centralMacAddressKnown_;
        upstreamNodeMacAddress = upstreamNodeMacAddress_;
        centralMacAddress = centralMacAddress_;
        xSemaphoreGive(discoveryMutex_);
    }

    if (!routeAvailable)
    {
        ESP_LOGW(
            TAG,
            "Cannot send to Central because no upstream route is selected"
        );

        return false;
    }

    return sendTo(
        upstreamNodeMacAddress,
        centralMacAddress,
        messageType,
        payload,
        payloadLength,
        timeoutMilliseconds
    );
}


bool EspNowCommunication::forwardMessageTo(
    const MacAddress& nextHopMacAddress,
    const Message& message,
    std::uint32_t timeoutMilliseconds
)
{
    if (message.header.payloadLength > MAX_PAYLOAD_SIZE)
    {
        ESP_LOGW(
            TAG,
            "Message cannot be forwarded because payload length %u exceeds maximum %u",
            static_cast<unsigned int>(message.header.payloadLength),
            static_cast<unsigned int>(MAX_PAYLOAD_SIZE)
        );

        return false;
    }

    if (message.header.hopsRemaining == 0U)
    {
        ESP_LOGW(
            TAG,
            "Message cannot be forwarded because hop limit reached zero"
        );

        return false;
    }

    Message forwardedMessage =
        message;

    --forwardedMessage.header.hopsRemaining;

    char nextHopMacAddressText[18]{};
    char finalDestinationMacAddressText[18]{};
    formatMacAddressForDisplay(
        nextHopMacAddressText,
        sizeof(nextHopMacAddressText),
        nextHopMacAddress
    );
    formatMacAddressForDisplay(
        finalDestinationMacAddressText,
        sizeof(finalDestinationMacAddressText),
        forwardedMessage.header.destinationMacAddress
    );

    ESP_LOGI(
        TAG,
        "Forwarding messageId=%u to nextHop=%s finalDestination=%s hopsRemaining=%u",
        static_cast<unsigned int>(
            forwardedMessage.header.messageId
        ),
        nextHopMacAddressText,
        finalDestinationMacAddressText,
        static_cast<unsigned int>(
            forwardedMessage.header.hopsRemaining
        )
    );

    return sendMessageTo(
        nextHopMacAddress,
        forwardedMessage,
        timeoutMilliseconds
    );
}


bool EspNowCommunication::receive(
    ReceivedMessage& receivedMessage,
    std::uint32_t timeoutMilliseconds
)
{
    if (!initialized_ ||
        applicationMessages_ == nullptr)
    {
        return false;
    }

    return xQueueReceive(
        applicationMessages_,
        &receivedMessage,
        toTicks(timeoutMilliseconds)
    ) == pdTRUE;
}


bool EspNowCommunication::isMessageForThisNode(
    const Message& message
) const
{
    return
        macAddressesMatch(
            message.header.destinationMacAddress,
            localMacAddress_
        ) ||
        isBroadcastMacAddress(
            message.header.destinationMacAddress
        );
}


bool EspNowCommunication::isInitialized() const
{
    return initialized_;
}


const EspNowCommunication::MacAddress&
EspNowCommunication::getLocalMacAddress() const
{
    return localMacAddress_;
}


bool EspNowCommunication::hasUpstreamNode() const
{
    bool upstreamNodeSelected = false;

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        upstreamNodeSelected = upstreamNodeSelected_;
        xSemaphoreGive(discoveryMutex_);
    }

    return upstreamNodeSelected;
}


EspNowCommunication::MacAddress
EspNowCommunication::getUpstreamNodeMacAddress() const
{
    MacAddress upstreamNodeMacAddress{};

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        upstreamNodeMacAddress = upstreamNodeMacAddress_;
        xSemaphoreGive(discoveryMutex_);
    }

    return upstreamNodeMacAddress;
}


bool EspNowCommunication::hasCentralMacAddress() const
{
    bool centralMacAddressKnown = false;

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        centralMacAddressKnown = centralMacAddressKnown_;
        xSemaphoreGive(discoveryMutex_);
    }

    return centralMacAddressKnown;
}


EspNowCommunication::MacAddress
EspNowCommunication::getCentralMacAddress() const
{
    MacAddress centralMacAddress{};

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        centralMacAddress = centralMacAddress_;
        xSemaphoreGive(discoveryMutex_);
    }

    return centralMacAddress;
}


std::uint16_t
EspNowCommunication::getHopCountToCentral() const
{
    std::uint16_t hopCountToCentral = UNKNOWN_HOP_COUNT;

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        hopCountToCentral = hopCountToCentral_;
        xSemaphoreGive(discoveryMutex_);
    }

    return hopCountToCentral;
}


std::int8_t
EspNowCommunication::getUpstreamNodeSignalStrengthDbm() const
{
    std::int8_t upstreamNodeSignalStrengthDbm = -128;

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        upstreamNodeSignalStrengthDbm = upstreamNodeSignalStrengthDbm_;
        xSemaphoreGive(discoveryMutex_);
    }

    return upstreamNodeSignalStrengthDbm;
}


std::uint8_t
EspNowCommunication::getChannel() const
{
    return channel_;
}


bool EspNowCommunication::setChannel(std::uint8_t channel)
{
    if (initialized_ || channel == 0U || channel > 14U) {
        return false;
    }

    channel_ = channel;
    return true;
}


void EspNowCommunication::printConnectionInfo() const
{
    char thisNodeMac[18] = {};
    char upstreamNodeMac[18] = {};
    char centralNodeMac[18] = {};
    char rssiText[16] = {};
    char hopCountText[16] = {};

    bool upstreamNodeSelected = false;
    MacAddress upstreamNodeMacAddress{};
    std::int8_t upstreamNodeSignalStrengthDbm = -128;
    bool centralMacAddressKnown = false;
    MacAddress centralMacAddress{};
    std::uint16_t hopCountToCentral = UNKNOWN_HOP_COUNT;

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        upstreamNodeSelected = upstreamNodeSelected_;
        upstreamNodeMacAddress = upstreamNodeMacAddress_;
        upstreamNodeSignalStrengthDbm = upstreamNodeSignalStrengthDbm_;
        centralMacAddressKnown = centralMacAddressKnown_;
        centralMacAddress = centralMacAddress_;
        hopCountToCentral = hopCountToCentral_;
        xSemaphoreGive(discoveryMutex_);
    }

    formatMacAddressForDisplay(thisNodeMac, sizeof(thisNodeMac), localMacAddress_);

    if (upstreamNodeSelected) {
        formatMacAddressForDisplay(upstreamNodeMac, sizeof(upstreamNodeMac), upstreamNodeMacAddress);
        std::snprintf(rssiText, sizeof(rssiText), "%d dBm", static_cast<int>(upstreamNodeSignalStrengthDbm));
    } else {
        std::snprintf(upstreamNodeMac, sizeof(upstreamNodeMac), "%s", centralNode_ ? "NONE" : "NOT SELECTED");
        std::snprintf(rssiText, sizeof(rssiText), "%s", "N/A");
    }

    if (centralMacAddressKnown) {
        formatMacAddressForDisplay(centralNodeMac, sizeof(centralNodeMac), centralMacAddress);
    } else {
        std::snprintf(centralNodeMac, sizeof(centralNodeMac), "%s", "UNKNOWN");
    }

    if (hopCountToCentral != UNKNOWN_HOP_COUNT) {
        std::snprintf(hopCountText, sizeof(hopCountText), "%u", static_cast<unsigned int>(hopCountToCentral));
    } else {
        std::snprintf(hopCountText, sizeof(hopCountText), "%s", "UNKNOWN");
    }

    ESP_LOGI(TAG, "========== ESP-NOW DIRECT CONNECTION TABLE: %s ==========", localNodeName_.data());
    ESP_LOGI(TAG, "+----------------+-------------------+---------+---------------------+----------+-----------+-------------------+");
    ESP_LOGI(TAG, "| Node Name      | Node MAC          | Role    | Next Hop to Central | RSSI     | Hop Count | Central Node MAC  |");
    ESP_LOGI(TAG, "+----------------+-------------------+---------+---------------------+----------+-----------+-------------------+");
    ESP_LOGI(TAG, "| %-14s | %-17s | %-7s | %-19s | %-8s | %-9s | %-17s |",
             localNodeName_.data(), thisNodeMac, centralNode_ ? "CENTRAL" : "SMART",
             upstreamNodeMac, rssiText, hopCountText, centralNodeMac);

    if (directDownstreamNodesMutex_ != nullptr &&
        xSemaphoreTake(directDownstreamNodesMutex_, portMAX_DELAY) == pdTRUE)
    {
        for (const DirectDownstreamNodeInfo& directNode : directDownstreamNodes_)
        {
            char directNodeMac[18] = {};
            char directNodeRssi[16] = {};
            char directNodeHopCount[16] = {};

            formatMacAddressForDisplay(directNodeMac, sizeof(directNodeMac), directNode.macAddress);

            if (directNode.signalStrengthKnown) {
                std::snprintf(directNodeRssi, sizeof(directNodeRssi), "%d dBm",
                              static_cast<int>(directNode.signalStrengthDbm));
            } else {
                std::snprintf(directNodeRssi, sizeof(directNodeRssi), "%s", "N/A");
            }

            std::snprintf(directNodeHopCount, sizeof(directNodeHopCount), "%u",
                          static_cast<unsigned int>(directNode.hopCountToCentral));

            ESP_LOGI(TAG, "| %-14s | %-17s | %-7s | %-19s | %-8s | %-9s | %-17s |",
                     directNode.nodeName.data(), directNodeMac, "SMART", thisNodeMac,
                     directNodeRssi, directNodeHopCount, centralNodeMac);
        }

        xSemaphoreGive(directDownstreamNodesMutex_);
    }

    ESP_LOGI(TAG, "+----------------+-------------------+---------+---------------------+----------+-----------+-------------------+");
}



bool EspNowCommunication::initializeNVS()
{
    esp_err_t result =
        nvs_flash_init();

    if (result ==
            ESP_ERR_NVS_NO_FREE_PAGES ||
        result ==
            ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(
            TAG,
            "NVS requires erase before initialization"
        );

        result =
            nvs_flash_erase();

        if (result != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Could not erase NVS: %s",
                esp_err_to_name(result)
            );

            return false;
        }

        result =
            nvs_flash_init();
    }

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "NVS initialization failed: %s",
            esp_err_to_name(result)
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "NVS initialized"
    );

    return true;
}


bool EspNowCommunication::initializeWiFi()
{
    esp_err_t result =
        esp_netif_init();

    if (result != ESP_OK &&
        result != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(
            TAG,
            "esp_netif_init failed: %s",
            esp_err_to_name(result)
        );

        return false;
    }

    result =
        esp_event_loop_create_default();

    if (result != ESP_OK &&
        result != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(
            TAG,
            "Default event loop initialization failed: %s",
            esp_err_to_name(result)
        );

        return false;
    }

    /*
     * Without this, the Wi-Fi driver can still associate at the link
     * layer (esp_wifi_connect() succeeds, WIFI_EVENT_STA_CONNECTED
     * fires) but there is no netif for a DHCP client to attach to:
     * IP_EVENT_STA_GOT_IP never fires, the station never gets an IP
     * address, and every IP-layer operation (DNS, MQTT, NTP) fails
     * regardless of how correct its own configuration is. This must be
     * created exactly once, before esp_wifi_start(), for both roles —
     * Central actually joins infrastructure Wi-Fi through this netif
     * (see WiFiManager), Smart Nodes never associate with an AP so it
     * simply stays unused for them, harmlessly.
     */
    if (esp_netif_create_default_wifi_sta() == nullptr)
    {
        ESP_LOGE(
            TAG,
            "esp_netif_create_default_wifi_sta() failed"
        );

        return false;
    }

    wifi_init_config_t wifiConfiguration =
        WIFI_INIT_CONFIG_DEFAULT();

    result =
        esp_wifi_init(
            &wifiConfiguration
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi initialization failed: %s",
            esp_err_to_name(result)
        );

        return false;
    }

    if (esp_wifi_set_storage(
            WIFI_STORAGE_RAM
        ) != ESP_OK ||
        esp_wifi_set_mode(
            WIFI_MODE_STA
        ) != ESP_OK ||
        esp_wifi_start() != ESP_OK ||
        esp_wifi_set_channel(
            channel_,
            WIFI_SECOND_CHAN_NONE
        ) != ESP_OK ||
        /*
         * Modem sleep (the default STA power-save mode) intermittently
         * powers the radio down between DTIM beacons. ESP-NOW's own
         * traffic on the same radio then competes with that duty cycle,
         * so a Central Node that just associated with the infrastructure
         * AP misses its keepalive timing and gets kicked with
         * WIFI_REASON_AUTH_EXPIRE within milliseconds of reaching the
         * "run" state — before DHCP/DNS ever come up. Disabling power
         * save keeps the radio fully responsive to both ESP-NOW and the
         * AP association at once.
         */
        esp_wifi_set_ps(
            WIFI_PS_NONE
        ) != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not configure Wi-Fi for ESP-NOW"
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi station mode ready on channel %u",
        static_cast<unsigned int>(
            channel_
        )
    );

    return true;
}


bool EspNowCommunication::initializeEspNow()
{
    esp_err_t result =
        esp_now_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "ESP-NOW initialization failed: %s",
            esp_err_to_name(result)
        );

        return false;
    }

    result =
        esp_now_register_send_cb(
            sendCallback
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not register ESP-NOW send callback: %s",
            esp_err_to_name(result)
        );

        return false;
    }

    result =
        esp_now_register_recv_cb(
            receiveCallback
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not register ESP-NOW receive callback: %s",
            esp_err_to_name(result)
        );

        return false;
    }

    return true;
}


bool EspNowCommunication::addBroadcastPeer()
{
    if (esp_now_is_peer_exist(
            BROADCAST_MAC_ADDRESS.data()
        ))
    {
        return true;
    }

    esp_now_peer_info_t peerInformation{};

    std::memcpy(
        peerInformation.peer_addr,
        BROADCAST_MAC_ADDRESS.data(),
        MAC_ADDRESS_LENGTH
    );

    peerInformation.channel =
        channel_;

    peerInformation.ifidx =
        WIFI_IF_STA;

    peerInformation.encrypt =
        false;

    const esp_err_t result =
        esp_now_add_peer(
            &peerInformation
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not add broadcast peer: %s",
            esp_err_to_name(result)
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "Broadcast peer added: FF:FF:FF:FF:FF:FF"
    );

    return true;
}


bool EspNowCommunication::buildMessage(
    Message& message,
    const MacAddress& destinationMacAddress,
    MessageType messageType,
    const void* payload,
    std::size_t payloadLength
)
{
    if (!initialized_ ||
        payloadLength >
            MAX_PAYLOAD_SIZE ||
        (payloadLength > 0U &&
         payload == nullptr))
    {
        ESP_LOGW(
            TAG,
            "Cannot build message: initialized=%s payloadLength=%u maximum=%u",
            initialized_ ? "Yes" : "No",
            static_cast<unsigned int>(
                payloadLength
            ),
            static_cast<unsigned int>(
                MAX_PAYLOAD_SIZE
            )
        );

        return false;
    }

    message = {};

    message.header.protocolVersion =
        PROTOCOL_VERSION;

    message.header.messageType =
        messageType;

    message.header.payloadLength =
        static_cast<std::uint16_t>(
            payloadLength
        );

    message.header.messageId =
        nextMessageId_.fetch_add(1U);

    message.header.originMacAddress =
        localMacAddress_;

    message.header.destinationMacAddress =
        destinationMacAddress;

    message.header.hopsRemaining =
        DEFAULT_HOP_LIMIT;

    if (payloadLength > 0U)
    {
        std::memcpy(
            message.payload.data(),
            payload,
            payloadLength
        );
    }

    return true;
}


bool EspNowCommunication::sendMessageTo(
    const MacAddress& nextHopMacAddress,
    const Message& message,
    std::uint32_t timeoutMilliseconds
)
{
    if (!initialized_)
    {
        return false;
    }

    if (message.header.payloadLength > MAX_PAYLOAD_SIZE)
    {
        ESP_LOGW(
            TAG,
            "Message cannot be sent because payload length %u exceeds maximum %u",
            static_cast<unsigned int>(message.header.payloadLength),
            static_cast<unsigned int>(MAX_PAYLOAD_SIZE)
        );

        return false;
    }

    if (!isBroadcastMacAddress(
            nextHopMacAddress
        ) &&
        !hasPeer(
            nextHopMacAddress
        ))
    {
        if (!addPeer(
                nextHopMacAddress
            ))
        {
            return false;
        }
    }

    if (xSemaphoreTake(
            sendMutex_,
            toTicks(
                timeoutMilliseconds
            )
        ) != pdTRUE)
    {
        ESP_LOGW(
            TAG,
            "Timed out waiting for ESP-NOW send mutex"
        );

        return false;
    }

    while (xSemaphoreTake(
               sendCompleted_,
               0U
           ) == pdTRUE)
    {
    }

    lastSendStatus_ =
        ESP_NOW_SEND_FAIL;

    const std::size_t wireLength =
        sizeof(MessageHeader) +
        message.header.payloadLength;

    const esp_err_t result =
        esp_now_send(
            nextHopMacAddress.data(),
            reinterpret_cast<
                const std::uint8_t*
            >(&message),
            wireLength
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_now_send failed: %s",
            esp_err_to_name(result)
        );

        xSemaphoreGive(
            sendMutex_
        );

        return false;
    }

    const bool callbackReceived =
        xSemaphoreTake(
            sendCompleted_,
            toTicks(
                timeoutMilliseconds
            )
        ) == pdTRUE;

    const bool delivered =
        callbackReceived &&
        lastSendStatus_ ==
            ESP_NOW_SEND_SUCCESS;

    if (!callbackReceived)
    {
        ESP_LOGW(
            TAG,
            "Timed out waiting for ESP-NOW send callback"
        );
    }

    if (delivered)
    {
        char nextHopMacAddressText[18]{};
        formatMacAddressForDisplay(
            nextHopMacAddressText,
            sizeof(nextHopMacAddressText),
            nextHopMacAddress
        );
        ESP_LOGD(
            TAG,
            "Message sent: messageId=%u nextHop=%s bytes=%u",
            static_cast<unsigned int>(
                message.header.messageId
            ),
            nextHopMacAddressText,
            static_cast<unsigned int>(
                wireLength
            )
        );
    }
    else
    {
        ESP_LOGW(
            TAG,
            "ESP-NOW MAC-layer delivery failed: messageId=%u",
            static_cast<unsigned int>(
                message.header.messageId
            )
        );
    }

    xSemaphoreGive(
        sendMutex_
    );

    return delivered;
}


void EspNowCommunication::processRawReceivedPacket(
    const RawReceivedPacket& packet
)
{
    refreshDirectDownstreamNodeSignalStrength(
        packet.senderMacAddress,
        packet.signalStrengthDbm
    );

    if (packet.dataLength <
            sizeof(MessageHeader) ||
        packet.dataLength >
            MAX_MESSAGE_SIZE)
    {
        ESP_LOGW(
            TAG,
            "Received packet has invalid length=%u",
            static_cast<unsigned int>(
                packet.dataLength
            )
        );

        return;
    }

    Message message{};

    std::memcpy(
        &message,
        packet.data.data(),
        packet.dataLength
    );

    if (message.header.protocolVersion !=
        PROTOCOL_VERSION)
    {
        ESP_LOGW(
            TAG,
            "Received unsupported protocol version=%u",
            static_cast<unsigned int>(
                message.header.protocolVersion
            )
        );

        return;
    }

    if (message.header.payloadLength >
        MAX_PAYLOAD_SIZE)
    {
        ESP_LOGW(
            TAG,
            "Received payload length exceeds maximum"
        );

        return;
    }

    const std::size_t expectedLength =
        sizeof(MessageHeader) +
        message.header.payloadLength;

    if (expectedLength >
        packet.dataLength)
    {
        ESP_LOGW(
            TAG,
            "Received packet is shorter than its declared payload length"
        );

        return;
    }

    if (message.header.messageType ==
        MessageType::DISCOVERY_REQUEST)
    {
        processDiscoveryRequest(
            packet,
            message
        );

        return;
    }

    if (message.header.messageType ==
        MessageType::DISCOVERY_RESPONSE)
    {
        processDiscoveryResponse(
            packet,
            message
        );

        return;
    }

    if (message.header.messageType ==
        MessageType::HANDSHAKE)
    {
        processConnectionHandshake(
            packet,
            message
        );

        return;
    }

    ReceivedMessage receivedMessage{};

    receivedMessage.senderMacAddress =
        packet.senderMacAddress;

    receivedMessage.signalStrengthDbm =
        packet.signalStrengthDbm;

    receivedMessage.message =
        message;

    if (xQueueSend(
            applicationMessages_,
            &receivedMessage,
            0U
        ) != pdTRUE)
    {
        ESP_LOGW(
            TAG,
            "Application receive queue is full; message dropped"
        );
    }
}


void EspNowCommunication::processDiscoveryRequest(
    const RawReceivedPacket& packet,
    const Message& message
)
{
    std::uint16_t localHopCountToCentral = UNKNOWN_HOP_COUNT;
    MacAddress centralMacAddress{};
    bool routeAvailable = false;

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        localHopCountToCentral = hopCountToCentral_;
        centralMacAddress = centralMacAddress_;
        routeAvailable = centralMacAddressKnown_ &&
                         localHopCountToCentral != UNKNOWN_HOP_COUNT;
        xSemaphoreGive(discoveryMutex_);
    }

    // Only a Node that already has a route to Central may advertise
    // itself as a possible upstream Node — this is what makes discovery
    // build a tree instead of letting unconnected Nodes pick each other.
    if (!routeAvailable)
    {
        return;
    }

    if (macAddressesMatch(
            message.header.originMacAddress,
            localMacAddress_
        ))
    {
        return;
    }

    if (message.header.payloadLength !=
        sizeof(DiscoveryRequest))
    {
        return;
    }

    DiscoveryRequest request{};

    std::memcpy(
        &request,
        message.payload.data(),
        sizeof(request)
    );

    DiscoveryResponse response{
        request.discoveryId,
        localHopCountToCentral,
        centralMacAddress
    };

    // Physically broadcast so we don't have to add every discovering Node
    // to the peer table; the logical destination is still the requester.
    const bool responseSent = sendTo(
        BROADCAST_MAC_ADDRESS,
        message.header.originMacAddress,
        MessageType::DISCOVERY_RESPONSE,
        response,
        50U
    );

    if (!responseSent)
    {
        ESP_LOGW(TAG, "Discovery response could not be sent within 50 ms");
        return;
    }

    char requesterMacAddressText[18]{};
    formatMacAddressForDisplay(
        requesterMacAddressText,
        sizeof(requesterMacAddressText),
        packet.senderMacAddress
    );

    ESP_LOGD(
        TAG,
        "Discovery response sent to requester via broadcast: requester=%s localHop=%u",
        requesterMacAddressText,
        static_cast<unsigned int>(
            localHopCountToCentral
        )
    );
}


void EspNowCommunication::processDiscoveryResponse(
    const RawReceivedPacket& packet,
    const Message& message
)
{
    if (!macAddressesMatch(
            message.header.destinationMacAddress,
            localMacAddress_
        ))
    {
        return;
    }

    if (message.header.payloadLength !=
        sizeof(DiscoveryResponse))
    {
        return;
    }

    DiscoveryResponse response{};

    std::memcpy(
        &response,
        message.payload.data(),
        sizeof(response)
    );

    if (xSemaphoreTake(
            discoveryMutex_,
            portMAX_DELAY
        ) != pdTRUE)
    {
        return;
    }

    const bool discoveryMatches =
        discoveryInProgress_ &&
        response.discoveryId ==
            activeDiscoveryId_;

    if (!discoveryMatches)
    {
        xSemaphoreGive(
            discoveryMutex_
        );

        return;
    }

    if (response.hopCountToCentral ==
        UNKNOWN_HOP_COUNT)
    {
        xSemaphoreGive(
            discoveryMutex_
        );

        return;
    }

    // A Node that already has a route must not choose a candidate at the
    // same or deeper level — that could create a cycle.
    if (hopCountToCentral_ !=
            UNKNOWN_HOP_COUNT &&
        response.hopCountToCentral >=
            hopCountToCentral_)
    {
        xSemaphoreGive(
            discoveryMutex_
        );

        return;
    }

    // Lowest hop count wins; ties broken by strongest RSSI.
    const bool shorterRouteToCentral =
        !discoveryCandidateFound_ ||
        response.hopCountToCentral <
            bestUpstreamCandidateHopCount_;

    const bool sameRouteLengthButStrongerSignal =
        discoveryCandidateFound_ &&
        response.hopCountToCentral ==
            bestUpstreamCandidateHopCount_ &&
        packet.signalStrengthDbm >
            bestUpstreamCandidateSignalStrengthDbm_;

    if (shorterRouteToCentral ||
        sameRouteLengthButStrongerSignal)
    {
        discoveryCandidateFound_ =
            true;

        bestUpstreamCandidateMacAddress_ =
            message.header.originMacAddress;

        bestUpstreamCandidateSignalStrengthDbm_ =
            packet.signalStrengthDbm;

        bestUpstreamCandidateHopCount_ =
            response.hopCountToCentral;

        bestUpstreamCandidateCentralMacAddress_ =
            response.centralMacAddress;

        char candidateMacAddressText[18]{};
        formatMacAddressForDisplay(
            candidateMacAddressText,
            sizeof(candidateMacAddressText),
            bestUpstreamCandidateMacAddress_
        );

        ESP_LOGI(
            TAG,
            "Better Upstream Node candidate: MAC=%s candidateHopToCentral=%u RSSI=%d dBm",
            candidateMacAddressText,
            static_cast<unsigned int>(
                bestUpstreamCandidateHopCount_
            ),
            static_cast<int>(
                bestUpstreamCandidateSignalStrengthDbm_
            )
        );
    }

    xSemaphoreGive(
        discoveryMutex_
    );
}



bool EspNowCommunication::sendConnectionHandshakeToUpstreamNode()
{
    MacAddress upstreamNodeMacAddress{};
    MacAddress centralMacAddress{};
    std::uint16_t hopCountToCentral = UNKNOWN_HOP_COUNT;
    bool routeAvailable = false;

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        upstreamNodeMacAddress = upstreamNodeMacAddress_;
        centralMacAddress = centralMacAddress_;
        hopCountToCentral = hopCountToCentral_;
        routeAvailable = upstreamNodeSelected_ &&
                         centralMacAddressKnown_ &&
                         hopCountToCentral != UNKNOWN_HOP_COUNT;
        xSemaphoreGive(discoveryMutex_);
    }

    if (!routeAvailable)
    {
        ESP_LOGW(TAG, "Cannot send connection handshake because the Upstream Node route is incomplete");
        return false;
    }

    ConnectionHandshake handshake{};
    std::snprintf(handshake.nodeName.data(), handshake.nodeName.size(), "%s", localNodeName_.data());
    handshake.hopCountToCentral = hopCountToCentral;
    handshake.centralMacAddress = centralMacAddress;

    char upstreamNodeMacAddressText[18]{};
    formatMacAddressForDisplay(
        upstreamNodeMacAddressText,
        sizeof(upstreamNodeMacAddressText),
        upstreamNodeMacAddress
    );

    ESP_LOGI(TAG, "Sending connection handshake: name=%s upstream=%s",
             localNodeName_.data(), upstreamNodeMacAddressText);

    return sendTo(
        upstreamNodeMacAddress,
        upstreamNodeMacAddress,
        MessageType::HANDSHAKE,
        handshake,
        1000U
    );
}


void EspNowCommunication::processConnectionHandshake(
    const RawReceivedPacket& packet,
    const Message& message
)
{
    if (!isMessageForThisNode(message)) return;

    if (message.header.payloadLength != sizeof(ConnectionHandshake)) {
        ESP_LOGW(TAG, "Connection handshake has invalid payload length");
        return;
    }

    ConnectionHandshake handshake{};
    std::memcpy(&handshake, message.payload.data(), sizeof(handshake));
    handshake.nodeName[NODE_NAME_LENGTH - 1U] = '\0';

    if (!macAddressesMatch(packet.senderMacAddress, message.header.originMacAddress)) {
        ESP_LOGW(TAG, "Ignoring forwarded connection handshake because direct attachment must come from the Node itself");
        return;
    }

    std::uint16_t localHopCountToCentral = UNKNOWN_HOP_COUNT;
    MacAddress centralMacAddress{};
    bool routeAvailable = false;

    if (discoveryMutex_ != nullptr &&
        xSemaphoreTake(discoveryMutex_, portMAX_DELAY) == pdTRUE)
    {
        localHopCountToCentral = hopCountToCentral_;
        centralMacAddress = centralMacAddress_;
        routeAvailable = centralMacAddressKnown_ &&
                         localHopCountToCentral != UNKNOWN_HOP_COUNT;
        xSemaphoreGive(discoveryMutex_);
    }

    if (!routeAvailable) {
        ESP_LOGW(TAG, "Ignoring downstream handshake because this Node has no valid route to Central");
        return;
    }

    const std::uint16_t expectedDownstreamHop =
        static_cast<std::uint16_t>(localHopCountToCentral + 1U);

    if (handshake.hopCountToCentral != expectedDownstreamHop) {
        ESP_LOGW(TAG, "Rejected direct downstream Node %s: hop=%u expected=%u",
                 handshake.nodeName.data(),
                 static_cast<unsigned int>(handshake.hopCountToCentral),
                 static_cast<unsigned int>(expectedDownstreamHop));
        return;
    }

    if (!macAddressesMatch(handshake.centralMacAddress, centralMacAddress)) {
        ESP_LOGW(TAG, "Rejected direct downstream Node %s because its Central MAC does not match this tree",
                 handshake.nodeName.data());
        return;
    }

    if (!addPeer(message.header.originMacAddress)) {
        ESP_LOGW(TAG, "espnow: peer registration failed for direct downstream Node %s",
                 handshake.nodeName.data());
    }

    const bool newNode = registerOrUpdateDirectDownstreamNode(
        handshake.nodeName.data(),
        message.header.originMacAddress,
        packet.signalStrengthDbm,
        true,
        handshake.hopCountToCentral
    );

    char directNodeMacAddressText[18]{};
    formatMacAddressForDisplay(
        directNodeMacAddressText,
        sizeof(directNodeMacAddressText),
        message.header.originMacAddress
    );

    ESP_LOGI(TAG, "Direct downstream Node registered: name=%s MAC=%s hop=%u RSSI=%d dBm",
             handshake.nodeName.data(),
             directNodeMacAddressText,
             static_cast<unsigned int>(handshake.hopCountToCentral),
             static_cast<int>(packet.signalStrengthDbm));

    if (newNode) printConnectionInfo();
}


bool EspNowCommunication::registerOrUpdateDirectDownstreamNode(
    const char* nodeName,
    const MacAddress& nodeMacAddress,
    std::int8_t signalStrengthDbm,
    bool signalStrengthKnown,
    std::uint16_t hopCountToCentral
)
{
    if (directDownstreamNodesMutex_ == nullptr) return false;

    if (xSemaphoreTake(directDownstreamNodesMutex_, portMAX_DELAY) != pdTRUE) return false;

    bool newNode = true;

    for (DirectDownstreamNodeInfo& directNode : directDownstreamNodes_) {
        if (!macAddressesMatch(directNode.macAddress, nodeMacAddress)) continue;

        std::snprintf(directNode.nodeName.data(), directNode.nodeName.size(), "%s",
                      nodeName != nullptr && nodeName[0] != '\0' ? nodeName : "Unnamed");
        directNode.signalStrengthDbm = signalStrengthDbm;
        directNode.signalStrengthKnown = signalStrengthKnown;
        directNode.hopCountToCentral = hopCountToCentral;
        newNode = false;
        break;
    }

    if (newNode) {
        DirectDownstreamNodeInfo directNode{};
        std::snprintf(directNode.nodeName.data(), directNode.nodeName.size(), "%s",
                      nodeName != nullptr && nodeName[0] != '\0' ? nodeName : "Unnamed");
        directNode.macAddress = nodeMacAddress;
        directNode.signalStrengthDbm = signalStrengthDbm;
        directNode.signalStrengthKnown = signalStrengthKnown;
        directNode.hopCountToCentral = hopCountToCentral;
        directDownstreamNodes_.push_back(directNode);
    }

    xSemaphoreGive(directDownstreamNodesMutex_);
    return newNode;
}


void EspNowCommunication::refreshDirectDownstreamNodeSignalStrength(
    const MacAddress& nodeMacAddress,
    std::int8_t signalStrengthDbm
)
{
    if (directDownstreamNodesMutex_ == nullptr) return;

    if (xSemaphoreTake(directDownstreamNodesMutex_, portMAX_DELAY) != pdTRUE) return;

    for (DirectDownstreamNodeInfo& directNode : directDownstreamNodes_) {
        if (macAddressesMatch(directNode.macAddress, nodeMacAddress)) {
            directNode.signalStrengthDbm = signalStrengthDbm;
            directNode.signalStrengthKnown = true;
            break;
        }
    }

    xSemaphoreGive(directDownstreamNodesMutex_);
}


void EspNowCommunication::serviceTaskEntry(
    void* parameter
)
{
    EspNowCommunication* communication =
        static_cast<
            EspNowCommunication*
        >(parameter);

    if (communication == nullptr)
    {
        vTaskDelete(nullptr);
        return;
    }

    communication->serviceLoop();
}


void EspNowCommunication::serviceLoop()
{
    RawReceivedPacket packet{};

    while (true)
    {
        if (xQueueReceive(
                rawReceivedPackets_,
                &packet,
                portMAX_DELAY
            ) == pdTRUE)
        {
            processRawReceivedPacket(
                packet
            );
        }
    }
}


TickType_t EspNowCommunication::toTicks(
    std::uint32_t timeoutMilliseconds
)
{
    if (timeoutMilliseconds ==
        WAIT_FOREVER)
    {
        return portMAX_DELAY;
    }

    return pdMS_TO_TICKS(
        timeoutMilliseconds
    );
}


bool EspNowCommunication::macAddressesMatch(
    const MacAddress& left,
    const MacAddress& right
)
{
    return left == right;
}


bool EspNowCommunication::isBroadcastMacAddress(
    const MacAddress& macAddress
)
{
    return macAddressesMatch(
        macAddress,
        BROADCAST_MAC_ADDRESS
    );
}


void EspNowCommunication::sendCallback(
    const esp_now_send_info_t* sendInformation,
    esp_now_send_status_t status
)
{
    if (instance_ == nullptr ||
        sendInformation == nullptr)
    {
        return;
    }

    instance_->lastSendStatus_ =
        status;

    if (instance_->sendCompleted_ !=
        nullptr)
    {
        xSemaphoreGive(
            instance_->sendCompleted_
        );
    }
}


void EspNowCommunication::receiveCallback(
    const esp_now_recv_info_t* receiveInformation,
    const std::uint8_t* data,
    int dataLength
)
{
    if (instance_ == nullptr ||
        receiveInformation == nullptr ||
        receiveInformation->src_addr ==
            nullptr ||
        data == nullptr ||
        dataLength <= 0 ||
        dataLength >
            static_cast<int>(
                MAX_MESSAGE_SIZE
            ))
    {
        return;
    }

    RawReceivedPacket packet{};

    std::copy_n(
        receiveInformation->src_addr,
        MAC_ADDRESS_LENGTH,
        packet.senderMacAddress.begin()
    );

    packet.signalStrengthDbm =
        -128;

    if (receiveInformation->rx_ctrl !=
        nullptr)
    {
        packet.signalStrengthDbm =
            receiveInformation
                ->rx_ctrl
                ->rssi;
    }

    packet.dataLength =
        static_cast<std::size_t>(
            dataLength
        );

    std::memcpy(
        packet.data.data(),
        data,
        packet.dataLength
    );

    if (xQueueSend(
            instance_->rawReceivedPackets_,
            &packet,
            0U
        ) != pdTRUE)
    {
        ESP_LOGW(
            TAG,
            "Raw ESP-NOW receive queue is full; packet dropped"
        );
    }
}


} // namespace kilowatts
