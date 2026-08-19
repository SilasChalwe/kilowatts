/**
 * @file test_node.cpp
 * @brief Host-native correctness tests for Node's Load ownership/lookup,
 *        including removeLoadByRelayPin().
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 */

#include "Node.h"

#include <cstdio>

using kilowatts::Load;
using kilowatts::LoadMode;
using kilowatts::LoadPower;
using kilowatts::Node;

namespace {

std::size_t passedChecks = 0U;
std::size_t failedChecks = 0U;

const char* recordResult(bool passed) {
    if (passed) {
        ++passedChecks;
        return "PASS";
    }

    ++failedChecks;
    return "FAIL";
}

bool reportCheck(const char* name, bool passed) {
    std::printf("%-78s %s\n", name, recordResult(passed));
    return passed;
}

void printSection(const char* title) {
    std::printf("\n======================================================================\n");
    std::printf("%s\n", title);
    std::printf("======================================================================\n");
}

const Node::MacAddress NODE_MAC = {0x02, 0x00, 0x00, 0x00, 0xE0, 0x01};
const Node::MacAddress OTHER_MAC = {0x02, 0x00, 0x00, 0x00, 0xE0, 0x02};

void testAddAndLookup() {
    printSection("TEST 1 - ADD / LOOKUP");

    Node node(NODE_MAC);
    reportCheck("A new Node starts with zero Loads", node.getNumberOfLoads() == 0U);

    Load light(Load::Id{NODE_MAC, 16U}, "Light", LoadPower{12.0F, 12.0F}, 8U, LoadMode::Fixed::ON);
    reportCheck("addLoad() accepts a Load whose MAC matches this Node", node.addLoad(light));
    reportCheck("getNumberOfLoads() becomes 1", node.getNumberOfLoads() == 1U);

    Load wrongMac(Load::Id{OTHER_MAC, 17U}, "Fan", LoadPower{18.0F, 25.0F}, 6U, LoadMode::Auto::ON);
    reportCheck("addLoad() rejects a Load whose MAC does not match this Node", !node.addLoad(wrongMac));
    reportCheck("A rejected addLoad() does not change getNumberOfLoads()", node.getNumberOfLoads() == 1U);

    Load duplicatePin(Load::Id{NODE_MAC, 16U}, "Duplicate", LoadPower{5.0F, 5.0F}, 1U, LoadMode::Fixed::OFF);
    reportCheck("addLoad() rejects a second Load on an already-used relay pin", !node.addLoad(duplicatePin));

    Load fan(Load::Id{NODE_MAC, 17U}, "Fan", LoadPower{18.0F, 25.0F}, 6U, LoadMode::Auto::ON);
    reportCheck("addLoad() accepts a second Load on a different relay pin", node.addLoad(fan));
    reportCheck("getNumberOfLoads() becomes 2", node.getNumberOfLoads() == 2U);

    const Load* foundByPin = node.getLoadByRelayPin(17U);
    reportCheck("getLoadByRelayPin() finds the matching Load", foundByPin != nullptr && foundByPin->getName() == "Fan");
    reportCheck("getLoadByRelayPin() returns nullptr for an unused pin", node.getLoadByRelayPin(99U) == nullptr);

    reportCheck("getLoad() returns nullptr for an out-of-range index", node.getLoad(99U) == nullptr);
}

void testRemoveByRelayPin() {
    printSection("TEST 2 - REMOVE BY RELAY PIN");

    Node node(NODE_MAC);
    node.addLoad(Load(Load::Id{NODE_MAC, 16U}, "Light", LoadPower{12.0F, 12.0F}, 8U, LoadMode::Fixed::ON));
    node.addLoad(Load(Load::Id{NODE_MAC, 17U}, "Fan", LoadPower{18.0F, 25.0F}, 6U, LoadMode::Auto::ON));
    node.addLoad(Load(Load::Id{NODE_MAC, 18U}, "WaterPump", LoadPower{35.0F, 55.0F}, 4U, LoadMode::Auto::OFF));

    reportCheck("removeLoadByRelayPin() on an unused pin returns false", !node.removeLoadByRelayPin(99U));
    reportCheck("A no-op removeLoadByRelayPin() leaves getNumberOfLoads() unchanged", node.getNumberOfLoads() == 3U);

    reportCheck("removeLoadByRelayPin() on an existing pin returns true", node.removeLoadByRelayPin(17U));
    reportCheck("getNumberOfLoads() decreases by 1", node.getNumberOfLoads() == 2U);
    reportCheck("The removed Load is no longer found by relay pin", node.getLoadByRelayPin(17U) == nullptr);
    reportCheck("Loads that were not removed are still present (Light)", node.getLoadByRelayPin(16U) != nullptr);
    reportCheck("Loads that were not removed are still present (WaterPump)", node.getLoadByRelayPin(18U) != nullptr);

    reportCheck("removeLoadByRelayPin() on an already-removed pin returns false", !node.removeLoadByRelayPin(17U));

    reportCheck("removeLoadByRelayPin() the remaining Loads drains the Node to zero (16)", node.removeLoadByRelayPin(16U));
    reportCheck("removeLoadByRelayPin() the remaining Loads drains the Node to zero (18)", node.removeLoadByRelayPin(18U));
    reportCheck("A fully drained Node reports zero Loads", node.getNumberOfLoads() == 0U);

    reportCheck("The relay pin of a fully removed Load can be reused by a new Load",
                node.addLoad(Load(Load::Id{NODE_MAC, 16U}, "Replacement", LoadPower{3.0F, 3.0F}, 1U, LoadMode::Fixed::OFF)));
    reportCheck("The reused pin now finds only the new Load",
                node.getLoadByRelayPin(16U) != nullptr && node.getLoadByRelayPin(16U)->getName() == "Replacement");
}

} // namespace

int main() {
    std::printf("KILOWATTS NODE HOST TEST REPORT\n");

    testAddAndLookup();
    testRemoveByRelayPin();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
