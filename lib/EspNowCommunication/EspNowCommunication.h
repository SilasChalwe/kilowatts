/**
 * @file EspNowCommunication.h
 * @brief Declares dynamic ESP-NOW communication for the Kilowatts tree network.
 *
 * A Smart Node does not need a manually configured upstream Node MAC
 * address. It broadcasts a discovery request, receives responses from
 * nearby Nodes that already have a route to the Central Node, and selects
 * the best Upstream Node by lowest hop count first, then RSSI on ties.
 */

#ifndef KILOWATTS_ESP_NOW_COMMUNICATION_H
#define KILOWATTS_ESP_NOW_COMMUNICATION_H

#include "ChipInfo.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "esp_now.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"


namespace kilowatts {


class EspNowCommunication {

public:

    static constexpr std::size_t MAC_ADDRESS_LENGTH = 6U;

    /** Maximum stored display length for a human-readable Node name. */
    static constexpr std::size_t NODE_NAME_LENGTH = 20U;


    static constexpr std::size_t MAX_MESSAGE_SIZE =
        ESP_NOW_MAX_DATA_LEN;


    static constexpr std::uint32_t WAIT_FOREVER =
        UINT32_MAX;


    static constexpr std::uint8_t PROTOCOL_VERSION =
        1U;


    /**
     * A message is discarded instead of being forwarded forever.
     */
    static constexpr std::uint8_t DEFAULT_HOP_LIMIT =
        16U;


    /**
     * A Node with UNKNOWN_HOP_COUNT does not yet have
     * a known route to the Central Node.
     */
    static constexpr std::uint16_t UNKNOWN_HOP_COUNT =
        UINT16_MAX;


    // ChipInfo remains responsible for reading the actual hardware MAC
    // address; EspNowCommunication only reuses its representation.
    using MacAddress =
        ChipInfo::MacAddress;


    static const MacAddress BROADCAST_MAC_ADDRESS;


    /**
     * IDENTITY_REPORT/COMMISSION_COMMAND/COMMISSION_ACK/
     * DECOMMISSION_COMMAND/DECOMMISSION_ACK (see lib/CommissioningPackets)
     * were appended for the commissioning lifecycle without renumbering
     * 1-8 — values are append-only since they cross the wire.
     */
    enum class MessageType : std::uint8_t {
        NODE_REPORT = 1U,
        LOAD_REPORT = 2U,
        RELAY_COMMAND = 3U,
        ACKNOWLEDGEMENT = 4U,
        ERROR_MESSAGE = 5U,
        HANDSHAKE = 6U,
        DISCOVERY_REQUEST = 7U,
        DISCOVERY_RESPONSE = 8U,
        IDENTITY_REPORT = 9U,
        COMMISSION_COMMAND = 10U,
        COMMISSION_ACK = 11U,
        DECOMMISSION_COMMAND = 12U,
        DECOMMISSION_ACK = 13U,
        NODE_REPORT_ACK = 14U,
        FACTORY_RESET_COMMAND = 18U,
        FACTORY_RESET_ACK = 19U,
        CONFIGURE_LOAD_COMMAND = 20U,
        CONFIGURE_LOAD_ACK = 21U
    };


    /**
     * Header carried by every Kilowatts ESP-NOW message.
     *
     * The immediate sender of one radio hop is NOT stored here — ESP-NOW
     * supplies that address separately when a packet is received.
     */
    struct MessageHeader {
        std::uint8_t protocolVersion;
        MessageType messageType;
        std::uint16_t payloadLength;
        std::uint32_t messageId;

        MacAddress originMacAddress;
        MacAddress destinationMacAddress;

        std::uint8_t hopsRemaining;
    };


    static constexpr std::size_t MAX_PAYLOAD_SIZE =
        MAX_MESSAGE_SIZE - sizeof(MessageHeader);


    /** Only header.payloadLength bytes of payload are valid. */
    struct Message {
        MessageHeader header;

        std::array<
            std::uint8_t,
            MAX_PAYLOAD_SIZE
        > payload;
    };


    /**
     * senderMacAddress/signalStrengthDbm describe the immediate radio hop;
     * message.header.originMacAddress is the Node that created the data.
     */
    struct ReceivedMessage {
        MacAddress senderMacAddress;
        std::int8_t signalStrengthDbm;
        Message message;
    };


    /** All Nodes participating in discovery must use the same Wi-Fi channel. */
    explicit EspNowCommunication(
        std::uint8_t channel = 1U
    );


    ~EspNowCommunication();


    EspNowCommunication(
        const EspNowCommunication&
    ) = delete;


    EspNowCommunication& operator=(
        const EspNowCommunication&
    ) = delete;


    /**
     * Sets the human-readable name of THIS Node.
     *
     * Set this before initialize() so connection logs and the
     * connection handshake carry the correct Node name.
     */
    void setLocalNodeName(
        const char* nodeName
    );


    /**
     * Returns the human-readable name of THIS Node.
     */
    const char* getLocalNodeName() const;


    /**
     * Registers one directly attached downstream Node without a real
     * received radio packet backing it (RSSI shown as N/A in the
     * connection table) — for tests; a real downstream ESP32 registers
     * itself automatically when its connection handshake is received.
     */
    bool registerDirectDownstreamNode(
        const char* nodeName,
        const MacAddress& nodeMacAddress,
        std::uint16_t hopCountToCentral
    );


    /**
     * Same as above, but with a caller-supplied RSSI value so a test can
     * exercise the connection table's signal-strength display too.
     */
    bool registerDirectDownstreamNode(
        const char* nodeName,
        const MacAddress& nodeMacAddress,
        std::int8_t signalStrengthDbm,
        std::uint16_t hopCountToCentral
    );


    /**
     * Initializes NVS, Wi-Fi station mode, ESP-NOW,
     * callbacks, queues and the broadcast peer.
     *
     * The local MAC address is obtained from ChipInfo.
     */
    bool initialize();


    /**
     * Marks THIS ESP32 as the Central Node.
     *
     * The Central Node has hop count 0 and therefore
     * can answer discovery requests immediately.
     */
    bool setAsCentralNode();


    /**
     * Broadcasts a discovery request and selects the responding Node with
     * the lowest hop count to Central (RSSI breaks ties). If this Node
     * already has a route, only a strictly lower hop count is accepted,
     * which prevents it from selecting one of its own descendants.
     */
    bool discoverUpstreamNode(
        std::uint32_t discoveryWindowMilliseconds = 1500U
    );


    /**
     * Adds one normal unicast ESP-NOW peer.
     */
    bool addPeer(
        const MacAddress& peerMacAddress
    );


    bool removePeer(
        const MacAddress& peerMacAddress
    );


    bool hasPeer(
        const MacAddress& peerMacAddress
    ) const;


    /**
     * Sends a new message created by THIS ESP32.
     *
     * nextHopMacAddress:
     *     ESP32 receiving the next physical radio hop.
     *
     * destinationMacAddress:
     *     final Node for which the message is intended.
     */
    bool sendTo(
        const MacAddress& nextHopMacAddress,
        const MacAddress& destinationMacAddress,
        MessageType messageType,
        const void* payload,
        std::size_t payloadLength,
        std::uint32_t timeoutMilliseconds = 1000U
    );


    /**
     * Typed send helper.
     *
     * Only simple packet structures may be sent directly.
     * A live C++ Node object containing std::vector or a Load
     * containing std::string must first be converted into a
     * transmission packet structure.
     */
    template <
        typename Payload,
        typename = std::enable_if_t<!std::is_pointer<Payload>::value>
    >
    bool sendTo(
        const MacAddress& nextHopMacAddress,
        const MacAddress& destinationMacAddress,
        MessageType messageType,
        const Payload& payload,
        std::uint32_t timeoutMilliseconds = 1000U
    )
    {
        static_assert(
            std::is_trivially_copyable<Payload>::value,
            "ESP-NOW payload must be a trivially copyable packet structure"
        );

        return sendTo(
            nextHopMacAddress,
            destinationMacAddress,
            messageType,
            &payload,
            sizeof(Payload),
            timeoutMilliseconds
        );
    }


    /** Routes via the already-selected upstream Node; no MAC address needed. */
    bool sendToCentral(
        MessageType messageType,
        const void* payload,
        std::size_t payloadLength,
        std::uint32_t timeoutMilliseconds = 1000U
    );


    template <
        typename Payload,
        typename = std::enable_if_t<!std::is_pointer<Payload>::value>
    >
    bool sendToCentral(
        MessageType messageType,
        const Payload& payload,
        std::uint32_t timeoutMilliseconds = 1000U
    )
    {
        static_assert(
            std::is_trivially_copyable<Payload>::value,
            "ESP-NOW payload must be a trivially copyable packet structure"
        );

        return sendToCentral(
            messageType,
            &payload,
            sizeof(Payload),
            timeoutMilliseconds
        );
    }


    /** Preserves original source, destination and message ID; decrements hopsRemaining. */
    bool forwardMessageTo(
        const MacAddress& nextHopMacAddress,
        const Message& message,
        std::uint32_t timeoutMilliseconds = 1000U
    );


    /**
     * Receives the next non-discovery application message.
     *
     * Discovery traffic is handled internally.
     */
    bool receive(
        ReceivedMessage& receivedMessage,
        std::uint32_t timeoutMilliseconds = WAIT_FOREVER
    );


    /** True when the final destination is THIS Node or broadcast. */
    bool isMessageForThisNode(
        const Message& message
    ) const;


    bool isInitialized() const;


    const MacAddress& getLocalMacAddress() const;


    bool hasUpstreamNode() const;


    const MacAddress& getUpstreamNodeMacAddress() const;


    bool hasCentralMacAddress() const;


    const MacAddress& getCentralMacAddress() const;


    std::uint16_t getHopCountToCentral() const;


    std::int8_t getUpstreamNodeSignalStrengthDbm() const;


    std::uint8_t getChannel() const;


    /** Prints THIS Node and its directly attached downstream Nodes only. */
    void printConnectionInfo() const;


private:

    struct DiscoveryRequest {
        std::uint32_t discoveryId;
    };


    struct DiscoveryResponse {
        std::uint32_t discoveryId;

        std::uint16_t hopCountToCentral;

        MacAddress centralMacAddress;
    };


    /** Carries the Node name so the Upstream Node can display it in its connection table. */
    struct ConnectionHandshake {
        std::array<char, NODE_NAME_LENGTH> nodeName;
        std::uint16_t hopCountToCentral;
        MacAddress centralMacAddress;
    };


    struct DirectDownstreamNodeInfo {
        std::array<char, NODE_NAME_LENGTH> nodeName;
        MacAddress macAddress;
        std::int8_t signalStrengthDbm;
        bool signalStrengthKnown;
        std::uint16_t hopCountToCentral;
    };


    /**
     * Raw frame copied out of the ESP-NOW receive callback.
     *
     * The callback performs only short work and places the data
     * into this queue for the service task.
     */
    struct RawReceivedPacket {
        MacAddress senderMacAddress;

        std::int8_t signalStrengthDbm;

        std::size_t dataLength;

        std::array<
            std::uint8_t,
            MAX_MESSAGE_SIZE
        > data;
    };


    bool initializeNVS();


    bool initializeWiFi();


    bool initializeEspNow();


    bool addBroadcastPeer();


    bool buildMessage(
        Message& message,
        const MacAddress& destinationMacAddress,
        MessageType messageType,
        const void* payload,
        std::size_t payloadLength
    );


    bool sendMessageTo(
        const MacAddress& nextHopMacAddress,
        const Message& message,
        std::uint32_t timeoutMilliseconds
    );


    void processRawReceivedPacket(
        const RawReceivedPacket& packet
    );


    void processDiscoveryRequest(
        const RawReceivedPacket& packet,
        const Message& message
    );


    void processDiscoveryResponse(
        const RawReceivedPacket& packet,
        const Message& message
    );


    bool sendConnectionHandshakeToUpstreamNode();


    void processConnectionHandshake(
        const RawReceivedPacket& packet,
        const Message& message
    );


    /** Returns true when a new downstream Node was added (vs. updated). */
    bool registerOrUpdateDirectDownstreamNode(
        const char* nodeName,
        const MacAddress& nodeMacAddress,
        std::int8_t signalStrengthDbm,
        bool signalStrengthKnown,
        std::uint16_t hopCountToCentral
    );


    void refreshDirectDownstreamNodeSignalStrength(
        const MacAddress& nodeMacAddress,
        std::int8_t signalStrengthDbm
    );


    static void serviceTaskEntry(
        void* parameter
    );


    void serviceLoop();


    static TickType_t toTicks(
        std::uint32_t timeoutMilliseconds
    );


    static bool macAddressesMatch(
        const MacAddress& left,
        const MacAddress& right
    );


    static bool isBroadcastMacAddress(
        const MacAddress& macAddress
    );


    static void sendCallback(
        const esp_now_send_info_t* sendInformation,
        esp_now_send_status_t status
    );


    static void receiveCallback(
        const esp_now_recv_info_t* receiveInformation,
        const std::uint8_t* data,
        int dataLength
    );


    ChipInfo chipInfo_;

    std::array<char, NODE_NAME_LENGTH> localNodeName_;

    MacAddress localMacAddress_;


    std::uint8_t channel_;


    bool initialized_;


    bool centralNode_;

    bool upstreamNodeSelected_;

    MacAddress upstreamNodeMacAddress_;

    std::int8_t upstreamNodeSignalStrengthDbm_;

    bool centralMacAddressKnown_;

    MacAddress centralMacAddress_;

    std::uint16_t hopCountToCentral_;


    /** Immediate children only, not the full downstream subtree. */
    std::vector<DirectDownstreamNodeInfo> directDownstreamNodes_;


    bool discoveryInProgress_;

    std::uint32_t activeDiscoveryId_;

    bool discoveryCandidateFound_;

    MacAddress bestUpstreamCandidateMacAddress_;

    std::int8_t bestUpstreamCandidateSignalStrengthDbm_;

    std::uint16_t bestUpstreamCandidateHopCount_;

    MacAddress bestUpstreamCandidateCentralMacAddress_;


    std::uint32_t nextMessageId_;


    SemaphoreHandle_t sendCompleted_;

    SemaphoreHandle_t sendMutex_;

    SemaphoreHandle_t discoveryMutex_;

    /** Protects directDownstreamNodes_. */
    SemaphoreHandle_t directDownstreamNodesMutex_;


    QueueHandle_t rawReceivedPackets_;

    QueueHandle_t applicationMessages_;


    TaskHandle_t serviceTask_;


    esp_now_send_status_t lastSendStatus_;


    // ESP-NOW callbacks are static, so only one instance is supported per ESP32.
    static EspNowCommunication* instance_;
};


} // namespace kilowatts

#endif // KILOWATTS_ESP_NOW_COMMUNICATION_H
