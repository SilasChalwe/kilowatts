/**
 * @file NodeLifecycle.h
 * @brief Device commissioning lifecycle shared by both Kilowatts firmware
 *        roles.
 *
 *   FACTORY        -> never yet booted with persistence available.
 *   UNCOMMISSIONED -> booted, no valid persisted commissioning record.
 *   DISCOVERED     -> Central has recorded this Node over ESP-NOW, but no
 *                     configuration command has been accepted for it yet.
 *   CONFIGURING    -> a commissioning command is in flight, awaiting the
 *                     Node's own validation/ACK.
 *   COMMISSIONED   -> the Node accepted and persisted its configuration.
 *   OPERATIONAL    -> commissioned and currently taking part in normal
 *                     planning (reserved for a later phase once a Node can
 *                     own Branches/Loads).
 *   DECOMMISSIONED -> explicitly removed from the installation.
 *
 * This header only defines the state values and which transitions between
 * them are legal; it does not decide when a transition should happen (see
 * NodeCommissioningRegistry for Central's authoritative record and
 * NodeIdentityStore for a Smart Node's own local record).
 */

#ifndef KILOWATTS_NODE_LIFECYCLE_H
#define KILOWATTS_NODE_LIFECYCLE_H

#include <array>
#include <cstdint>

namespace kilowatts {


/**
 * Same byte layout as Load::MacAddress / EspNowCommunication::MacAddress,
 * declared independently so the commissioning libraries stay free of any
 * ESP-IDF/domain-object dependency and remain host-testable.
 */
using MacAddress = std::array<std::uint8_t, 6>;


enum class NodeRole : std::uint8_t {
    CENTRAL = 0U,
    SMART = 1U
};


enum class NodeLifecycleState : std::uint8_t {
    FACTORY = 0U,
    UNCOMMISSIONED = 1U,
    DISCOVERED = 2U,
    CONFIGURING = 3U,
    COMMISSIONED = 4U,
    OPERATIONAL = 5U,
    DECOMMISSIONED = 6U
};


/**
 * Returns true when moving from "from" to "to" is a legal lifecycle
 * transition. DECOMMISSIONED is reachable from any state except FACTORY,
 * and can only re-enter the lifecycle at UNCOMMISSIONED (never straight
 * back to COMMISSIONED). A state never legally transitions to itself
 * through this function - callers that merely refresh "last seen" data
 * for an unchanged state don't need to call this at all.
 */
bool isValidNodeLifecycleTransition(NodeLifecycleState from, NodeLifecycleState to);


/** Stable lowercase text for logs/JSON — never null, always a valid C string. */
const char* toText(NodeLifecycleState state);

/** Stable lowercase text for logs/JSON — never null, always a valid C string. */
const char* toText(NodeRole role);


} // namespace kilowatts

#endif // KILOWATTS_NODE_LIFECYCLE_H
