/**
 * @file NodeIdentityStore.h
 * @brief A Smart Node's own local, persisted commissioning identity.
 *
 * The Smart-Node-local counterpart to Central's authoritative
 * NodeCommissioningRegistry - each Smart Node stores only its own
 * identity, never the whole installation.
 *
 * Unlike NodeCommissioningRegistry, this class never observes DISCOVERED
 * or CONFIGURING for itself - those only describe what Central currently
 * knows *about* a Node from the outside. From a Node's own point of view
 * it is simply UNCOMMISSIONED or COMMISSIONED/OPERATIONAL;
 * applyCommission() resolves synchronously in one call, with no
 * in-between state to persist. DECOMMISSIONED is likewise never stored
 * locally: applyDecommission() resets straight back to UNCOMMISSIONED,
 * since only Central needs that historical distinction (to avoid
 * recreating a fake Node from a stale command).
 */

#ifndef KILOWATTS_NODE_IDENTITY_STORE_H
#define KILOWATTS_NODE_IDENTITY_STORE_H

#include "NodeLifecycle.h"

#include <cstddef>

namespace kilowatts {


class NodeIdentityStore {

public:

    static constexpr std::size_t FRIENDLY_NAME_BUFFER_SIZE = 20U;


    NodeIdentityStore();


    /**
     * Loads the persisted identity from NVS (ESP32 target only), replacing
     * whatever this object currently holds.
     *
     * Returns false, and leaves this object's in-memory state at its
     * default (UNCOMMISSIONED, empty friendly name), on a host build, when
     * nothing has ever been persisted, or when the persisted record is
     * corrupt/malformed/an unsupported schema version.
     */
    bool loadPersisted();


    NodeLifecycleState getLifecycleState() const;

    /** Never null; empty ("") when this Node has never been commissioned. */
    const char* getFriendlyName() const;


    /**
     * Applies a received CommissionCommandPacket's friendlyName - the
     * Node-local half of the operation
     * NodeCommissioningRegistry::requestCommissioning() performs on
     * Central, so the acceptance rule mirrors it: accepted only when
     * friendlyName is valid and lifecycleState is UNCOMMISSIONED (first
     * commissioning) or COMMISSIONED/OPERATIONAL (rename).
     *
     * Unlike Central's two-phase pending/confirm flow, this resolves
     * immediately in one call - a Node is its own authority for whether
     * it accepted the change. The caller should persist() this result and
     * reply with CommissionAckPacket{success, getLifecycleState()} either
     * way. Returns false, with no change made, when rejected.
     */
    bool applyCommission(const char* friendlyName);


    /**
     * Erases this Node's local commissioning identity and returns it to
     * UNCOMMISSIONED (never DECOMMISSIONED - see the file-level comment).
     * Always succeeds; the caller should persist() afterward.
     */
    void applyDecommission();


    /**
     * Persists the current identity to NVS (ESP32 target only).
     *
     * Returns false on a host build, or when the underlying NVS write
     * failed; the in-memory state is unaffected either way.
     */
    bool persist() const;


private:

    NodeLifecycleState lifecycleState_;
    char friendlyName_[FRIENDLY_NAME_BUFFER_SIZE];
};


} // namespace kilowatts

#endif // KILOWATTS_NODE_IDENTITY_STORE_H
