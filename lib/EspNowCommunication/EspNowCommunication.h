/**
 * @file EspNowCommunication.h
 * @brief Declares dynamic ESP-NOW communication for the Kilowatts tree network.
 *
 * One EspNowCommunication object represents the ESP-NOW capability
 * of THIS ESP32.
 *
 * THIS ESP32 obtains its own Wi-Fi station MAC address from ChipInfo.
 *
 * A Smart Node does not need a manually configured upstream Node MAC address.
 * It can broadcast a discovery request, receive responses from nearby
 * Nodes that already have a route to the Central Node, and select the
 * best Upstream Node using the lowest hop count first and RSSI when
 * candidate hop counts are equal.
 *
 * The selected peer becomes the Smart Node's upstream Node/next hop.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
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

    /**
     * One complete ESP32 MAC address contains 6 bytes.
     *
     * Example:
     * 24:6F:28:AA:BB:01
     *
     * macAddress[0] through macAddress[5] are the six bytes
     * of ONE MAC address.
     */
    static constexpr std::size_t MAC_ADDRESS_LENGTH = 6U;


    /**
     * Maximum stored display length for a human-readable Node name.
     *
     * Examples:
     * "Central"
     * "Sitting Room"
     * "Bedroom"
     * "Garage"
     */
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


    /**
     * Reuse the same MAC-address representation supplied by ChipInfo.
     *
     * ChipInfo remains responsible for reading the actual hardware
     * Wi-Fi station MAC address. EspNowCommunication only uses it.
     */
    using MacAddress =
        ChipInfo::MacAddress;


    /**
     * ESP-NOW broadcast address.
     *
     * FF:FF:FF:FF:FF:FF
     */
    static const MacAddress BROADCAST_MAC_ADDRESS;


    /**
     * Describes what the payload contains.
     *
     * The ESP-NOW transport therefore remains reusable for:
     * Node reports, Load reports, relay commands, acknowledgements,
     * errors, connection handshakes, discovery and commissioning.
     *
     * IDENTITY_REPORT/COMMISSION_COMMAND/COMMISSION_ACK/
     * DECOMMISSION_COMMAND/DECOMMISSION_ACK (see lib/CommissioningPackets)
     * were appended for the commissioning lifecycle without renumbering
     * 1-8, the same "append, never renumber" convention already used for
     * BestFirstSearch's rejection-reason codes.
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
        DEV_SESSION_COMMAND = 15U,
        DEV_SENSOR_INPUT_COMMAND = 16U,
        DEV_ACK = 17U,
        FACTORY_RESET_COMMAND = 18U,
        FACTORY_RESET_ACK = 19U,
        CONFIGURE_LOAD_COMMAND = 20U,
        CONFIGURE_LOAD_ACK = 21U
    };


    /**
     * Header carried by every Kilowatts ESP-NOW message.
     *
     * originMacAddress:
     *     Node that originally created the message.
     *
     * destinationMacAddress:
     *     Final Node for which the message is intended.
     *
     * The immediate sender of one radio hop is NOT stored here.
     * ESP-NOW gives that address separately when the packet is received.
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


    /**
     * Complete application message.
     *
     * Only header.payloadLength bytes of payload are valid.
     */
    struct Message {
        MessageHeader header;

        std::array<
            std::uint8_t,
            MAX_PAYLOAD_SIZE
        > payload;
    };


    /**
     * Message returned to the application.
     *
     * senderMacAddress:
     *     ESP32 that physically sent the most recent radio hop.
     *
     * signalStrengthDbm:
     *     RSSI of that received radio hop.
     *
     * message.header.originMacAddress:
     *     Node that originally created the information.
     */
    struct ReceivedMessage {
        MacAddress senderMacAddress;
        std::int8_t signalStrengthDbm;
        Message message;
    };


    /**
     * Creates the ESP-NOW manager for THIS ESP32.
     *
     * No upstream Node/peer MAC address is supplied here.
     *
     * All Nodes participating in discovery must currently use
     * the same Wi-Fi channel.
     */
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
     * Registers one directly attached downstream Node whose
     * relationship is already known by the application.
     *
     * This is used by the current behaviour test to represent
     * downstream reports that we are pretending were already received.
     *
     * A real downstream ESP32 is registered automatically when its
     * connection handshake is physically received.
     *
     * Because this overload does not represent a real received radio
     * packet, RSSI is shown as N/A in the connection table.
     */
    bool registerDirectDownstreamNode(
        const char* nodeName,
        const MacAddress& nodeMacAddress,
        std::uint16_t hopCountToCentral
    );


    /**
     * Registers one directly attached downstream Node with a supplied
     * signal-strength value.
     *
     * This overload is useful for behaviour tests where the downstream
     * Node is simulated but we still want the connection table to show
     * an RSSI value.
     *
     * In the real system, RSSI comes automatically from the ESP-NOW
     * receive metadata when the downstream Node physically communicates.
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
     * Dynamically discovers the nearest valid upstream Node.
     *
     * Discovery flow:
     *
     * 1. Broadcast DISCOVERY_REQUEST.
     * 2. Nodes that already know a route to Central respond.
     * 3. Ignore Nodes that do not already have a route to Central.
     * 4. Prefer the candidate with the lowest hop count to Central.
     * 5. If candidate hop counts are equal, prefer the stronger RSSI.
     * 6. The selected Node becomes this Node's Upstream Node.
     *
     * If this Smart Node already has a route, only Nodes with
     * a LOWER hop count are accepted as upstream candidates.
     * This prevents selecting one of its own descendants.
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
    template <typename Payload>
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


    /**
     * Sends a message upward through the selected upstream Node
     * toward the Central Node.
     *
     * No MAC address needs to be passed by the caller after
     * discoverUpstreamNode() has selected the Upstream Node.
     */
    bool sendToCentral(
        MessageType messageType,
        const void* payload,
        std::size_t payloadLength,
        std::uint32_t timeoutMilliseconds = 1000U
    );


    template <typename Payload>
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


    /**
     * Forwards an existing message without changing:
     * - original source,
     * - final destination,
     * - message ID.
     *
     * hopsRemaining is reduced by one.
     */
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


    /**
     * Returns true when the final destination is:
     * - THIS Node, or
     * - broadcast.
     */
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


    /**
     * Prints THIS Node and its directly attached downstream Nodes.
     *
     * Examples:
     *
     * Central table:
     *     Central
     *     Sitting Room
     *
     * Sitting Room table:
     *     Sitting Room
     *     Bedroom
     *     Garage
     *
     * A Node that is not directly attached is not printed in this
     * direct-connection table.
     */
    void printConnectionInfo() const;


private:

    /**
     * Payload sent when looking for an Upstream Node.
     */
    struct DiscoveryRequest {
        std::uint32_t discoveryId;
    };


    /**
     * Payload sent by a Node that already has a route to Central.
     */
    struct DiscoveryResponse {
        std::uint32_t discoveryId;

        std::uint16_t hopCountToCentral;

        MacAddress centralMacAddress;
    };


    /**
     * Internal connection message sent after a Smart Node
     * selects its Upstream Node.
     *
     * The human-readable Node name is carried so the Upstream
     * Node can display a meaningful direct-connection table.
     */
    struct ConnectionHandshake {
        std::array<char, NODE_NAME_LENGTH> nodeName;
        std::uint16_t hopCountToCentral;
        MacAddress centralMacAddress;
    };


    /**
     * Information for one Node that is directly downstream
     * from THIS ESP32.
     */
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


    /**
     * Sends the connection handshake to the selected Upstream Node.
     */
    bool sendConnectionHandshakeToUpstreamNode();


    /**
     * Processes a received connection handshake.
     */
    void processConnectionHandshake(
        const RawReceivedPacket& packet,
        const Message& message
    );


    /**
     * Adds or updates one directly attached downstream Node.
     *
     * Returns true when a new downstream Node was added.
     */
    bool registerOrUpdateDirectDownstreamNode(
        const char* nodeName,
        const MacAddress& nodeMacAddress,
        std::int8_t signalStrengthDbm,
        bool signalStrengthKnown,
        std::uint16_t hopCountToCentral
    );


    /**
     * Refreshes RSSI when THIS ESP32 later hears another real
     * packet directly from a registered downstream Node.
     */
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


    /**
     * Supplies the real Wi-Fi station MAC address of THIS ESP32.
     *
     * ESP-NOW does not read the local MAC address separately.
     */
    ChipInfo chipInfo_;


    /**
     * Human-readable name of THIS ESP32 Node.
     */
    std::array<char, NODE_NAME_LENGTH> localNodeName_;


    /**
     * Real Wi-Fi station MAC address of THIS ESP32.
     */
    MacAddress localMacAddress_;


    std::uint8_t channel_;


    bool initialized_;


    /**
     * Tree information.
     */
    bool centralNode_;

    bool upstreamNodeSelected_;

    MacAddress upstreamNodeMacAddress_;

    std::int8_t upstreamNodeSignalStrengthDbm_;

    bool centralMacAddressKnown_;

    MacAddress centralMacAddress_;

    std::uint16_t hopCountToCentral_;


    /**
     * Nodes directly downstream from THIS ESP32.
     *
     * Central may therefore contain Sitting Room.
     * Sitting Room may independently contain Bedroom and Garage.
     */
    std::vector<DirectDownstreamNodeInfo> directDownstreamNodes_;


    /**
     * Discovery state.
     */
    bool discoveryInProgress_;

    std::uint32_t activeDiscoveryId_;

    bool discoveryCandidateFound_;

    MacAddress bestUpstreamCandidateMacAddress_;

    std::int8_t bestUpstreamCandidateSignalStrengthDbm_;

    std::uint16_t bestUpstreamCandidateHopCount_;

    MacAddress bestUpstreamCandidateCentralMacAddress_;


    /**
     * Message IDs created by this Node.
     */
    std::uint32_t nextMessageId_;


    /**
     * ESP-NOW / FreeRTOS synchronization.
     */
    SemaphoreHandle_t sendCompleted_;

    SemaphoreHandle_t sendMutex_;

    SemaphoreHandle_t discoveryMutex_;

    /**
     * Protects THIS Node's direct downstream Node list.
     */
    SemaphoreHandle_t directDownstreamNodesMutex_;


    QueueHandle_t rawReceivedPackets_;

    QueueHandle_t applicationMessages_;


    TaskHandle_t serviceTask_;


    esp_now_send_status_t lastSendStatus_;


    /**
     * ESP-NOW callbacks are static.
     *
     * The firmware therefore uses one communication manager
     * instance on each ESP32.
     */
    static EspNowCommunication* instance_;
};


} // namespace kilowatts

#endif // KILOWATTS_ESP_NOW_COMMUNICATION_H
