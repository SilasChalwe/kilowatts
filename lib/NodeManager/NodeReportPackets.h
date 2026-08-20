/**
 * @file NodeReportPackets.h
 * @brief ESP-NOW packets used by Central and Smart Nodes.
 */
#ifndef KILOWATTS_NODE_REPORT_PACKETS_H
#define KILOWATTS_NODE_REPORT_PACKETS_H

#include "EspNowCommunication.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kilowatts {

static constexpr std::size_t MAX_LOADS_PER_NODE_PACKET = 3U;

enum class LoadAvailability : std::uint8_t {
    AVAILABLE = 0U,
    FAULTED = 1U,
    UNAVAILABLE = 2U
};

/** One load connected to the reporting Node/branch. */
struct LoadReportPacket {
    char name[16];
    std::uint8_t relayPin;
    std::uint8_t mode;
    std::uint16_t priority;
    float startupWatts;
    float nominalVoltageVolts;
    float nominalCurrentAmps;
    std::uint8_t confirmedRelayState;
    std::uint8_t confirmedRelayStateValid;
    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleHour;
    std::uint8_t scheduleMinute;
    std::uint8_t availability;
};

/**
 * One Node report. In the planning/topology model the Node itself is the
 * branch/location; its loads are the relay channels listed below.
 */
struct NodeReportPacket {
    char nodeName[20];
    EspNowCommunication::MacAddress nodeMacAddress;
    EspNowCommunication::MacAddress upstreamNodeMacAddress;
    std::uint16_t hopCountToCentral;
    std::uint8_t numberOfLoads;
    std::uint16_t reportSequenceId;
    std::uint8_t pageIndex;
    std::uint8_t totalPages;
    std::array<LoadReportPacket, MAX_LOADS_PER_NODE_PACKET> loads;
};

enum class NodeReportAckStatus : std::uint8_t {
    ACCEPTED = 0U,
    REJECTED = 1U
};

struct NodeReportAcknowledgementPacket {
    std::uint16_t reportSequenceId;
    std::uint8_t status;
};

enum class RelayCommandState : std::uint8_t {
    OFF = 0U,
    ON = 1U
};

enum class RelayCommandFailureReason : std::uint8_t {
    NONE = 0U,
    RELAY_PIN_NOT_REGISTERED = 1U,
    GPIO_WRITE_FAILED = 2U,
    READBACK_MISMATCH = 3U,
    SAFETY_RECHECK_FAILED = 4U
};

struct RelayCommandPacket {
    std::uint8_t relayPin;
    std::uint8_t desiredState;
    std::uint32_t commandId;
};

struct RelayCommandAcknowledgementPacket {
    std::uint8_t relayPin;
    std::uint32_t commandId;
    std::uint8_t requestedState;
    std::uint8_t confirmedState;
    std::uint8_t success;
    std::uint8_t failureReason;
};

static_assert(std::is_trivially_copyable<NodeReportPacket>::value,
              "NodeReportPacket must be safe for ESP-NOW");
static_assert(std::is_trivially_copyable<NodeReportAcknowledgementPacket>::value,
              "NodeReportAcknowledgementPacket must be safe for ESP-NOW");
static_assert(std::is_trivially_copyable<RelayCommandPacket>::value,
              "RelayCommandPacket must be safe for ESP-NOW");
static_assert(std::is_trivially_copyable<RelayCommandAcknowledgementPacket>::value,
              "RelayCommandAcknowledgementPacket must be safe for ESP-NOW");
static_assert(sizeof(NodeReportPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "NodeReportPacket is too large for ESP-NOW");

} // namespace kilowatts

#endif // KILOWATTS_NODE_REPORT_PACKETS_H
