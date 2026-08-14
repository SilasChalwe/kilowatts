/**
 * @file CommissioningPackets.h
 * @brief Wire-format structures for the commissioning lifecycle, sent
 *        between Kilowatts ESP32 firmware over ESP-NOW.
 *
 * These are transport structures only, the same kind of thing
 * lib/NodeReportPackets already declares for measurement/relay traffic -
 * kept in their own file rather than added to NodeReportPackets.h because
 * that file's NodeReportPacket is already close to
 * EspNowCommunication::MAX_PAYLOAD_SIZE (it carries up to
 * MAX_LOADS_PER_NODE_PACKET LoadReportPacket entries) and is sent on a hot
 * ~2s cycle; commissioning packets are small, sent rarely (on a lifecycle
 * change, or once per watchdog cycle for IdentityReportPacket), and belong
 * to a distinct concern, so keeping them separate avoids risking the
 * already-tested NodeReportPacket layout.
 *
 * The owning Node for every packet below is always the surrounding
 * EspNowCommunication::Message header's origin/destination MAC address,
 * exactly like RelayCommandPacket already does in NodeReportPackets.h -
 * never repeated inside these structs.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#ifndef KILOWATTS_COMMISSIONING_PACKETS_H
#define KILOWATTS_COMMISSIONING_PACKETS_H

#include "EspNowCommunication.h"
#include "NodeLifecycle.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace kilowatts {

/** Board-declared GPIO capability slots carried in an infrequent identity report. */
static constexpr std::size_t MAX_RELAY_GPIO_CAPABILITIES = 8U;


/**
 * Sent by a Node toward Central to announce/refresh its identity - role,
 * current lifecycle state, firmware version and chip model. Sent
 * periodically at a slow (diagnostics-scale) cadence and immediately after
 * any local lifecycle change, deliberately not on NodeReportPacket's hot
 * ~2s cycle (see file-level comment above). Central's
 * NodeCommissioningRegistry::recordDiscovered() consumes this.
 */
struct IdentityReportPacket {
    std::uint8_t role;             // raw NodeRole byte
    std::uint8_t lifecycleState;   // raw NodeLifecycleState byte
    char firmwareVersion[12];
    char chipModel[16];
    std::uint8_t relayCapabilityCount;
    std::array<std::uint8_t, MAX_RELAY_GPIO_CAPABILITIES> relayPins;
};


/**
 * Sent by Central to a Node to assign/change its friendly name and
 * complete (or repeat) commissioning. commandId is chosen by Central and
 * echoed back in the matching CommissionAckPacket so a delayed/duplicate
 * acknowledgement can be matched to the command that produced it - the
 * same convention RelayCommandPacket/RelayCommandAcknowledgementPacket
 * already use.
 */
struct CommissionCommandPacket {
    std::uint32_t commandId;
    char friendlyName[20];
};


/**
 * success = 0 means the Node rejected the command (for example an empty
 * friendlyName). resultingState always reflects the Node's own true
 * lifecycle state after processing this command, whether accepted or
 * rejected - never only meaningful on success - so Central can always
 * trust it as the Node's honest current state (see
 * NodeCommissioningRegistry::applyCommissionResult()).
 */
struct CommissionAckPacket {
    std::uint32_t commandId;
    std::uint8_t success;
    std::uint8_t resultingState;   // raw NodeLifecycleState byte
};


/** Sent by Central to a Node, instructing it to erase its local commissioning identity and return to UNCOMMISSIONED. */
struct DecommissionCommandPacket {
    std::uint32_t commandId;
};


struct DecommissionAckPacket {
    std::uint32_t commandId;
    std::uint8_t success;
};


static_assert(std::is_trivially_copyable<IdentityReportPacket>::value,
              "IdentityReportPacket must be safe to send and receive over ESP-NOW");
static_assert(std::is_trivially_copyable<CommissionCommandPacket>::value,
              "CommissionCommandPacket must be safe to send and receive over ESP-NOW");
static_assert(std::is_trivially_copyable<CommissionAckPacket>::value,
              "CommissionAckPacket must be safe to send and receive over ESP-NOW");
static_assert(std::is_trivially_copyable<DecommissionCommandPacket>::value,
              "DecommissionCommandPacket must be safe to send and receive over ESP-NOW");
static_assert(std::is_trivially_copyable<DecommissionAckPacket>::value,
              "DecommissionAckPacket must be safe to send and receive over ESP-NOW");

static_assert(sizeof(IdentityReportPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "IdentityReportPacket is too large to fit inside one ESP-NOW message");
static_assert(sizeof(CommissionCommandPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "CommissionCommandPacket is too large to fit inside one ESP-NOW message");
static_assert(sizeof(CommissionAckPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "CommissionAckPacket is too large to fit inside one ESP-NOW message");
static_assert(sizeof(DecommissionCommandPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "DecommissionCommandPacket is too large to fit inside one ESP-NOW message");
static_assert(sizeof(DecommissionAckPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "DecommissionAckPacket is too large to fit inside one ESP-NOW message");


} // namespace kilowatts

#endif // KILOWATTS_COMMISSIONING_PACKETS_H
