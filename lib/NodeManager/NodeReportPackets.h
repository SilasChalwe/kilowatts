/**
 * @file NodeReportPackets.h
 * @brief Wire-format structures sent between Kilowatts ESP32 firmware over
 *        ESP-NOW.
 *
 * These are the byte layouts carried inside an EspNowCommunication Message
 * payload - a transport representation only, not the Node/Load domain
 * object used for planning (see Node.h/Load.h). Declared once here and
 * included by both src/central/main.cpp and src/smart/main.cpp so the two
 * sides can't silently drift apart on byte layout.
 *
 * ESP-NOW v1.0 (the Central Node's classic ESP32 has no Wi-Fi 6 radio)
 * caps one physical message at ESP_NOW_MAX_DATA_LEN = 250 bytes, so
 * EspNowCommunication::MAX_PAYLOAD_SIZE is a hard ceiling on every struct
 * below, enforced by the static_asserts at the bottom of this file.
 * `pageIndex`/`totalPages` are reserved wire fields; the deployed firmware
 * accepts and emits only page 0 of 1 until multi-page reassembly exists.
 */

#ifndef KILOWATTS_NODE_REPORT_PACKETS_H
#define KILOWATTS_NODE_REPORT_PACKETS_H

#include "EspNowCommunication.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kilowatts {


/**
 * One complete Node report is kept small enough to fit inside one ESP-NOW
 * message (see the file-level comment above). The current firmware supports
 * at most this many configured Loads per Smart Node; it must not emit a
 * partial/multi-page report until Central-side reassembly exists.
 */
static constexpr std::size_t MAX_LOADS_PER_NODE_PACKET = 3U;


/**
 * Raw byte value for a Load's health as reported by its owning Node.
 * Carried as std::uint8_t on the wire, the same convention LoadMode
 * already uses, so the struct stays trivially copyable.
 */
enum class LoadAvailability : std::uint8_t {
    AVAILABLE = 0U,   // Load is healthy and may be commanded normally.
    FAULTED = 1U,      // A relay command failed to confirm; needs maintenance.
    UNAVAILABLE = 2U   // Configuration invalid, or the owning hardware is not responding.
};


/**
 * Wire layout for one Load inside a NodeReportPacket.
 *
 * mode is the configured/commanded intent (raw LoadMode::Value byte);
 * confirmedRelayState is the last physically read-back relay state (see
 * RelayController::readBackState()). The two are deliberately separate
 * fields, and a receiver must never treat one as the other.
 *
 * confirmedRelayStateValid is this Load's own
 * Load::isConfirmedRelayStateValid(). A receiver must only treat
 * confirmedRelayState as meaningful when this is 1; when it is 0 the
 * receiver must preserve whatever confirmed state/validity it already
 * held rather than overwriting it with this report's meaningless OFF
 * default.
 *
 * branchMaximumCurrentAmps is carried per-Load rather than in a separate
 * Branch packet because one relay channel feeds exactly one Load in the
 * current hardware design, so Branch and Load share one relay-pin
 * identity; Central derives BranchId as {NodeReportPacket::nodeMacAddress,
 * LoadReportPacket::relayPin}.
 *
 * nominalVoltageVolts/nominalCurrentAmps are installer-entered/nameplate
 * ratings, not a live sensor reading - the hardware has one INA219 at
 * Central's battery bus and none per Load. Central derives planned
 * running power as V x A; the optimiser and UI must label it an estimate.
 */
struct LoadReportPacket {
    char name[16];
    std::uint8_t relayPin;
    std::uint8_t mode;
    std::uint16_t priority;
    float startupWatts;
    float branchMaximumCurrentAmps;
    float nominalVoltageVolts;
    float nominalCurrentAmps;
    std::uint8_t confirmedRelayState;   // 0 = OFF, 1 = ON (last GPIO read-back)
    std::uint8_t confirmedRelayStateValid; // Load::isConfirmedRelayStateValid(): 1 = trustworthy, 0 = unknown
    std::uint8_t scheduleEnabled;       // AutoSchedule::enabled
    std::uint8_t scheduleHour;          // AutoSchedule::hour   (0-23)
    std::uint8_t scheduleMinute;        // AutoSchedule::minute (0-59)
    std::uint8_t availability;          // raw LoadAvailability byte
};


/**
 * Wire layout for one complete Node report.
 *
 * upstreamNodeMacAddress is this Node's Next Hop to Central: the immediate
 * ESP32 it sends toward when a message needs to travel further toward
 * Central. It is not necessarily Central's own MAC address.
 *
 * `pageIndex`/`totalPages` are reserved for a future protocol revision.
 * The current implementation requires pageIndex = 0 and totalPages = 1;
 * Central rejects any other values rather than applying an incomplete
 * report.
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


/**
 * Application-level acknowledgement of one NodeReportPacket, sent by
 * Central back to the reporting Node once it has actually decoded and
 * applied the report to its registry - MAC-layer ESP-NOW delivery success
 * alone never proves Central understood/accepted a report, only that a
 * radio frame arrived. reportSequenceId lets the reporting Node match this
 * ACK to the exact NodeReportPacket it sent.
 */
enum class NodeReportAckStatus : std::uint8_t {
    ACCEPTED = 0U,
    REJECTED = 1U
};

struct NodeReportAcknowledgementPacket {
    std::uint16_t reportSequenceId;
    std::uint8_t status;   // raw NodeReportAckStatus byte
};


/**
 * Logical relay state carried on the wire by RelayCommandPacket and
 * RelayCommandAcknowledgementPacket, kept as its own named byte constant
 * set (rather than reusing LoadMode) because a relay command only ever
 * expresses ON/OFF — it never carries Fixed/Auto, which is a Load
 * configuration concept, not a physical actuation concept.
 */
enum class RelayCommandState : std::uint8_t {
    OFF = 0U,
    ON = 1U
};


/**
 * Reason a RelayCommandAcknowledgementPacket reports failure. NONE is used
 * only when success = 1.
 */
enum class RelayCommandFailureReason : std::uint8_t {
    NONE = 0U,
    RELAY_PIN_NOT_REGISTERED = 1U,
    GPIO_WRITE_FAILED = 2U,
    READBACK_MISMATCH = 3U,
    SAFETY_RECHECK_FAILED = 4U
};


/**
 * Sent by Central to the Node that owns a Branch, commanding one relay
 * pin to a desired state. The destination Node itself is already
 * identified by the surrounding EspNowCommunication message header
 * (Message::header::destinationMacAddress) — this packet only needs to
 * say *which relay on that Node* and *what state*, never a Node MAC
 * address again. commandId is chosen by Central and echoed back in the
 * matching RelayCommandAcknowledgementPacket so a delayed/duplicate
 * acknowledgement can be matched to the command that produced it.
 */
struct RelayCommandPacket {
    std::uint8_t relayPin;
    std::uint8_t desiredState;   // raw RelayCommandState byte
    std::uint32_t commandId;
};


/**
 * Sent by a Smart/Central Node back to Central after RelayController has
 * attempted a RelayCommandPacket. confirmedState is only meaningful when
 * RelayController::readBackState() itself succeeded; success = 0 means
 * the requested state must not be trusted as the physical truth.
 */
struct RelayCommandAcknowledgementPacket {
    std::uint8_t relayPin;
    std::uint32_t commandId;
    std::uint8_t requestedState;    // raw RelayCommandState byte
    std::uint8_t confirmedState;    // raw RelayCommandState byte (valid only when success = 1)
    std::uint8_t success;           // 1 when the command was applied and confirmed, else 0
    std::uint8_t failureReason;     // raw RelayCommandFailureReason byte
};


static_assert(
    std::is_trivially_copyable<NodeReportPacket>::value,
    "NodeReportPacket must be safe to send and receive over ESP-NOW"
);

static_assert(
    std::is_trivially_copyable<NodeReportAcknowledgementPacket>::value,
    "NodeReportAcknowledgementPacket must be safe to send and receive over ESP-NOW"
);

static_assert(
    std::is_trivially_copyable<RelayCommandPacket>::value,
    "RelayCommandPacket must be safe to send and receive over ESP-NOW"
);

static_assert(
    std::is_trivially_copyable<RelayCommandAcknowledgementPacket>::value,
    "RelayCommandAcknowledgementPacket must be safe to send and receive over ESP-NOW"
);

static_assert(
    sizeof(NodeReportPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
    "NodeReportPacket (one page) is too large to fit inside one ESP-NOW message"
);

static_assert(
    sizeof(RelayCommandPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
    "RelayCommandPacket is too large to fit inside one ESP-NOW message"
);

static_assert(
    sizeof(RelayCommandAcknowledgementPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
    "RelayCommandAcknowledgementPacket is too large to fit inside one ESP-NOW message"
);


} // namespace kilowatts

#endif // KILOWATTS_NODE_REPORT_PACKETS_H
