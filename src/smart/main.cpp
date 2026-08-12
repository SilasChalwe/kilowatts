#if defined(DEVICE_ROLE_SMART)

/**
 * @file main.cpp
 * @brief Smart Node end-to-end Node/Load forwarding behaviour test.
 *
 * The Smart Node first establishes its own route to Central. Only after
 * that do we simulate two downstream NODE_REPORT messages that this Smart
 * Node has already received. The reports are stored, organized by MAC
 * relationship, printed, and then forwarded toward Central.
 *
 * No BestFirstSearch is used in this test.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */

#include "EspNowCommunication.h"
#include "Load.h"
#include "Node.h"

#include <array>
#include <cstdio>
#include <type_traits>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace kilowatts;

static const char *TAG = "SMART_MAIN";
static constexpr std::size_t MAX_LOADS_PER_NODE = 5U;
static constexpr std::size_t NUMBER_OF_TEST_NODE_REPORTS = 3U;

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

static_assert(std::is_trivially_copyable<NodePacket>::value, "NodePacket must be safe to transmit");
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

NodePacket makeNodePacket(const char *name, const Node& node, const EspNowCommunication::MacAddress& upstreamMac, std::uint16_t hopCount)
{
    NodePacket packet{};
    std::snprintf(packet.nodeName, sizeof(packet.nodeName), "%s", name);
    packet.nodeMacAddress = node.getMacAddress();
    packet.upstreamNodeMacAddress = upstreamMac;
    packet.hopCountToCentral = hopCount;
    packet.numberOfLoads = static_cast<std::uint8_t>(node.getNumberOfLoads());

    for (std::size_t i = 0; i < node.getNumberOfLoads() && i < MAX_LOADS_PER_NODE; ++i) {
        const Load *load = node.getLoad(i);
        if (load == nullptr) continue;
        std::snprintf(packet.loads[i].name, sizeof(packet.loads[i].name), "%s", load->getName().c_str());
        packet.loads[i].relayPin = load->getRelayPin();
        packet.loads[i].mode = static_cast<std::uint8_t>(load->getMode());
        packet.loads[i].priority = load->getPriority();
        packet.loads[i].runningWatts = load->getPower().runningWatts;
        packet.loads[i].startupWatts = load->getPower().startupWatts;
    }

    return packet;
}

void storeNodeReport(const NodePacket& report, std::array<NodePacket, NUMBER_OF_TEST_NODE_REPORTS>& reports, std::size_t& count)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (reports[i].nodeMacAddress == report.nodeMacAddress) {
            reports[i] = report;
            return;
        }
    }

    if (count < reports.size()) reports[count++] = report;
}

void printNodeAndLoads(const NodePacket& node, std::size_t depth)
{
    char nodeMac[18] = {}, upstreamMac[18] = {};
    formatMac(node.nodeMacAddress, nodeMac);
    formatMac(node.upstreamNodeMacAddress, upstreamMac);
    const int indent = static_cast<int>(depth * 4U);

    ESP_LOGI(TAG, "%*sNODE %s [%s] nextHop=%s hop=%u", indent, "", node.nodeName, nodeMac, upstreamMac, static_cast<unsigned int>(node.hopCountToCentral));

    for (std::size_t i = 0; i < node.numberOfLoads && i < MAX_LOADS_PER_NODE; ++i) {
        const LoadPacket& load = node.loads[i];
        ESP_LOGI(TAG, "%*s  LOAD %-14s relay=%u mode=%-10s running=%.1fW startup=%.1fW priority=%u",
                 indent, "", load.name, static_cast<unsigned int>(load.relayPin), modeText(load.mode),
                 static_cast<double>(load.runningWatts), static_cast<double>(load.startupWatts),
                 static_cast<unsigned int>(load.priority));
    }
}

void printOrganizedSubtree(const NodePacket& node, const std::array<NodePacket, NUMBER_OF_TEST_NODE_REPORTS>& reports, std::size_t count, std::size_t depth)
{
    printNodeAndLoads(node, depth);

    for (std::size_t i = 0; i < count; ++i) {
        if (reports[i].upstreamNodeMacAddress == node.nodeMacAddress) {
            printOrganizedSubtree(reports[i], reports, count, depth + 1U);
        }
    }
}

void sendNodeReport(EspNowCommunication& communication, const NodePacket& report)
{
    const bool sent = communication.sendToCentral(EspNowCommunication::MessageType::NODE_REPORT, report);
    ESP_LOGI(TAG, "%s NODE_REPORT %s", report.nodeName, sent ? "SENT" : "FAILED");
}

extern "C" void app_main()
{
    EspNowCommunication communication(1U);
    communication.setLocalNodeName("Sitting Room");

    if (!communication.initialize()) { ESP_LOGE(TAG, "ESP-NOW initialization failed"); return; }

    const auto& localMac = communication.getLocalMacAddress();

    Node thisSmartNode(localMac);
    thisSmartNode.addLoad(Load({localMac, 16U}, "Light", {12.0F, 12.0F}, 1U, LoadMode::Fixed::ON));
    thisSmartNode.addLoad(Load({localMac, 17U}, "Router", {10.0F, 10.0F}, 2U, LoadMode::Fixed::ON));
    thisSmartNode.addLoad(Load({localMac, 18U}, "Fan", {18.0F, 25.0F}, 3U, LoadMode::Auto::ON));
    thisSmartNode.addLoad(Load({localMac, 19U}, "WaterPump", {35.0F, 55.0F}, 4U, LoadMode::Auto::OFF));
    thisSmartNode.addLoad(Load({localMac, 21U}, "ChargingPort", {15.0F, 15.0F}, 5U, LoadMode::Auto::ON));

    /*
     * A Smart Node must first have a valid route toward Central before
     * downstream Nodes should use it as their Upstream Node.
     */
    while (!communication.discoverUpstreamNode(2000U)) {
        ESP_LOGW(TAG, "No route to Central yet. Downstream collection has not started.");
        vTaskDelay(pdMS_TO_TICKS(2000U));
    }

    const std::uint16_t localHop = communication.getHopCountToCentral();
    std::array<NodePacket, NUMBER_OF_TEST_NODE_REPORTS> collectedReports{};
    std::size_t collectedReportCount = 0U;

    /*
     * Store Sitting Room first because it is the root of the local
     * subtree that will be forwarded toward Central.
     */
    const NodePacket localReport = makeNodePacket("Sitting Room", thisSmartNode, communication.getUpstreamNodeMacAddress(), localHop);
    storeNodeReport(localReport, collectedReports, collectedReportCount);

    /*
     * SIMULATED RECEIVE 1:
     * Pretend this report has just arrived from a downstream Smart Node.
     */
    const EspNowCommunication::MacAddress downstreamMac1 = {0x02, 0x00, 0x00, 0x00, 0x00, 0x11};
    Node downstreamNode1(downstreamMac1);
    downstreamNode1.addLoad(Load({downstreamMac1, 16U}, "BedroomLight", {8.0F, 8.0F}, 1U, LoadMode::Fixed::ON));
    downstreamNode1.addLoad(Load({downstreamMac1, 17U}, "DeskFan", {14.0F, 20.0F}, 2U, LoadMode::Auto::ON));
    storeNodeReport(makeNodePacket("Bedroom", downstreamNode1, localMac, localHop + 1U), collectedReports, collectedReportCount);
    ESP_LOGI(TAG, "Simulated downstream NODE_REPORT received from Bedroom");

    /*
     * SIMULATED RECEIVE 2:
     * Pretend this report has just arrived from another downstream Smart Node.
     */
    const EspNowCommunication::MacAddress downstreamMac2 = {0x02, 0x00, 0x00, 0x00, 0x00, 0x12};
    Node downstreamNode2(downstreamMac2);
    downstreamNode2.addLoad(Load({downstreamMac2, 16U}, "GarageLight", {9.0F, 9.0F}, 1U, LoadMode::Fixed::ON));
    downstreamNode2.addLoad(Load({downstreamMac2, 17U}, "GaragePump", {22.0F, 32.0F}, 2U, LoadMode::Auto::OFF));
    storeNodeReport(makeNodePacket("Garage", downstreamNode2, localMac, localHop + 1U), collectedReports, collectedReportCount);
    ESP_LOGI(TAG, "Simulated downstream NODE_REPORT received from Garage");

    /*
     * The two simulated reports represent Nodes that are directly
     * attached to Sitting Room. Register those relationships in
     * EspNowCommunication so they appear in Sitting Room's direct
     * connection table.
     *
     * RSSI is N/A because no real radio packet came from the dummy MACs.
     */
    communication.registerDirectDownstreamNode("Bedroom", downstreamMac1, localHop + 1U);
    communication.registerDirectDownstreamNode("Garage", downstreamMac2, localHop + 1U);

    communication.printConnectionInfo();


    /*
     * Collection is complete. NOW organize and print.
     *
     * printOrganizedSubtree() does not depend on receive order.
     * A child is found by:
     *
     * child.upstreamNodeMacAddress == parent.nodeMacAddress
     */
    ESP_LOGI(TAG, "================ ORGANIZED SMART NODE SUBTREE ================");
    printOrganizedSubtree(collectedReports[0], collectedReports, collectedReportCount, 0U);
    ESP_LOGI(TAG, "================================================================");

    /*
     * Forward the collected reports toward Central only after they have
     * been stored and their relationships have been verified locally.
     */
    for (std::size_t i = 0; i < collectedReportCount; ++i) {
        sendNodeReport(communication, collectedReports[i]);
        vTaskDelay(pdMS_TO_TICKS(300U));
    }

    while (true) vTaskDelay(pdMS_TO_TICKS(5000U));
}

#endif // DEVICE_ROLE_SMART