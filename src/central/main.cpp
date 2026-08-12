#if defined(DEVICE_ROLE_CENTRAL)

/**
 * @file main.cpp
 * @brief Simple Central Node ESP-NOW receive/tree test.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */

#include "EspNowCommunication.h"
#include "Load.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "esp_log.h"

using namespace kilowatts;

static const char *TAG = "CENTRAL_MAIN";
static constexpr std::size_t MAX_LOADS_PER_NODE = 5U;
static constexpr std::size_t MAX_TEST_NODES = 3U;

struct LoadPacket {
    char name[16];
    std::uint8_t relayPin;
    std::uint8_t mode;
    std::uint16_t priority;
    float runningWatts;
    float startupWatts;
};

struct NodePacket {
    char nodeName[16];
    EspNowCommunication::MacAddress nodeMacAddress;
    EspNowCommunication::MacAddress upstreamNodeMacAddress;
    std::uint16_t hopCountToCentral;
    std::uint8_t numberOfLoads;
    std::array<LoadPacket, MAX_LOADS_PER_NODE> loads;
};

static_assert(std::is_trivially_copyable<NodePacket>::value, "NodePacket must be safe to receive");
static_assert(sizeof(NodePacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE, "NodePacket is too large for ESP-NOW");

const char *modeText(std::uint8_t mode)
{
    switch (static_cast<LoadMode::Value>(mode)) {
        case LoadMode::Value::FIXED_OFF: return "FIXED_OFF";
        case LoadMode::Value::FIXED_ON: return "FIXED_ON";
        case LoadMode::Value::AUTO_OFF: return "AUTO_OFF";
        case LoadMode::Value::AUTO_ON: return "AUTO_ON";
        default: return "UNKNOWN";
    }
}

void formatMac(const EspNowCommunication::MacAddress& mac, char text[18])
{
    std::snprintf(text, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void printNode(const NodePacket& node)
{
    char nodeMac[18] = {}, upstreamMac[18] = {};
    formatMac(node.nodeMacAddress, nodeMac);
    formatMac(node.upstreamNodeMacAddress, upstreamMac);

    ESP_LOGI(TAG, "NODE %-12s MAC=%s nextHop=%s hop=%u loads=%u", node.nodeName, nodeMac, upstreamMac,
             static_cast<unsigned int>(node.hopCountToCentral), static_cast<unsigned int>(node.numberOfLoads));

    ESP_LOGI(TAG, "+----------------+-------+------------+----------+----------+");
    ESP_LOGI(TAG, "| Load           | Relay | Mode       | RunningW | StartupW |");
    ESP_LOGI(TAG, "+----------------+-------+------------+----------+----------+");

    for (std::size_t i = 0; i < node.numberOfLoads && i < MAX_LOADS_PER_NODE; ++i) {
        const LoadPacket& load = node.loads[i];
        ESP_LOGI(TAG, "| %-14s | %-5u | %-10s | %-8.1f | %-8.1f |", load.name,
                 static_cast<unsigned int>(load.relayPin), modeText(load.mode),
                 static_cast<double>(load.runningWatts), static_cast<double>(load.startupWatts));
    }

    ESP_LOGI(TAG, "+----------------+-------+------------+----------+----------+");
}

void printChildren(const EspNowCommunication::MacAddress& parent, const std::array<NodePacket, MAX_TEST_NODES>& nodes, std::size_t count, std::size_t depth)
{
    if (depth > count) return;

    for (std::size_t i = 0; i < count; ++i) {
        if (nodes[i].upstreamNodeMacAddress != parent) continue;
        char mac[18] = {};
        formatMac(nodes[i].nodeMacAddress, mac);
        ESP_LOGI(TAG, "%*s|-- %s [%s] hop=%u", static_cast<int>(depth * 4U), "", nodes[i].nodeName, mac,
                 static_cast<unsigned int>(nodes[i].hopCountToCentral));
        printChildren(nodes[i].nodeMacAddress, nodes, count, depth + 1U);
    }
}

void storeNode(const NodePacket& packet, std::array<NodePacket, MAX_TEST_NODES>& nodes, std::size_t& count)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (nodes[i].nodeMacAddress == packet.nodeMacAddress) { nodes[i] = packet; return; }
    }
    if (count < MAX_TEST_NODES) nodes[count++] = packet;
}

extern "C" void app_main()
{
    EspNowCommunication communication(1U);
    communication.setLocalNodeName("Central");

    if (!communication.initialize() || !communication.setAsCentralNode()) { ESP_LOGE(TAG, "Central initialization failed"); return; }

    std::array<NodePacket, MAX_TEST_NODES> receivedNodes{};
    std::size_t receivedNodeCount = 0U;

    while (true) {
        EspNowCommunication::ReceivedMessage received{};
        if (!communication.receive(received)) continue;
        if (received.message.header.messageType != EspNowCommunication::MessageType::NODE_REPORT) continue;

        if (received.message.header.payloadLength != sizeof(NodePacket)) {
            ESP_LOGW(TAG, "Wrong NODE_REPORT size=%u", static_cast<unsigned int>(received.message.header.payloadLength));
            continue;
        }

        NodePacket packet{};
        std::memcpy(&packet, received.message.payload.data(), sizeof(packet));
        storeNode(packet, receivedNodes, receivedNodeCount);

        char senderMac[18] = {};
        formatMac(received.senderMacAddress, senderMac);
        ESP_LOGI(TAG, "NODE_REPORT received from immediate ESP-NOW sender %s", senderMac);
        printNode(packet);

        char centralMac[18] = {};
        formatMac(communication.getLocalMacAddress(), centralMac);
        ESP_LOGI(TAG, "================ RECEIVED NODE TREE ================");
        ESP_LOGI(TAG, "Central [%s] hop=0", centralMac);
        printChildren(communication.getLocalMacAddress(), receivedNodes, receivedNodeCount, 1U);
        ESP_LOGI(TAG, "====================================================");
    }
}


#endif // DEVICE_ROLE_CENTRAL