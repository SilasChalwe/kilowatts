/**
 * @file NodeLifecycle.h
 * @brief Declares the device commissioning lifecycle shared by both
 *        Kilowatts firmware roles.
 *
 * A newly flashed ESP32 knows only its role (Central or Smart) at compile
 * time; it does not know its installation identity (friendly name, owning
 * Branches/Loads) until it has been discovered and commissioned. This
 * header defines that lifecycle as one small, explicit state machine so
 * "has this Node been configured yet" is never left implicit in scattered
 * booleans.
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
 *                     own Branches/Loads; this phase does not produce it).
 *   DECOMMISSIONED -> explicitly removed from the installation.
 *
 * This header only defines the state values and which transitions between
 * them are legal. It does not decide when a transition should happen (see
 * lib/NodeCommissioningRegistry for Central's authoritative record and
 * lib/NodeIdentityStore for a Smart Node's own local record), does not
 * perform ESP-NOW communication, and does not persist anything itself.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#ifndef KILOWATTS_NODE_LIFECYCLE_H
#define KILOWATTS_NODE_LIFECYCLE_H

#include <array>
#include <cstdint>

namespace kilowatts {


/** Same byte layout as Load::MacAddress / EspNowCommunication::MacAddress (both plain aliases of this same type), declared independently here so the commissioning libraries stay free of any ESP-IDF/domain-object dependency and remain host-testable. */
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
 * transition. FACTORY/UNCOMMISSIONED/DISCOVERED are the pre-commissioning
 * states a Node can freely move between as ESP-NOW discovery learns more
 * about it; CONFIGURING is only entered from UNCOMMISSIONED/DISCOVERED and
 * only resolves to COMMISSIONED or back to UNCOMMISSIONED (a failed
 * commissioning attempt); DECOMMISSIONED is reachable from any state except
 * FACTORY (nothing has ever been commissioned yet, so there is nothing to
 * decommission) and, once DECOMMISSIONED, a Node may only re-enter the
 * lifecycle at UNCOMMISSIONED (never straight back to COMMISSIONED without
 * being configured again). A state never legally transitions to itself
 * through this function — callers that merely refresh "last seen" data for
 * an unchanged state do not need to call this at all.
 */
bool isValidNodeLifecycleTransition(NodeLifecycleState from, NodeLifecycleState to);


/** Stable lowercase text for logs/JSON — never null, always a valid C string. */
const char* toText(NodeLifecycleState state);

/** Stable lowercase text for logs/JSON — never null, always a valid C string. */
const char* toText(NodeRole role);


} // namespace kilowatts

#endif // KILOWATTS_NODE_LIFECYCLE_H
