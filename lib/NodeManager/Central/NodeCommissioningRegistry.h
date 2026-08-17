/**
 * @file NodeCommissioningRegistry.h
 * @brief Declares Central's authoritative record of every Node's identity
 *        and commissioning lifecycle.
 *
 * NodeCommissioningRegistry's one responsibility: remember which physical
 * Nodes Central has ever heard from, what lifecycle state each one is
 * currently in, and their commissioned friendly name - across a reboot.
 * This is deliberately a different concern from CentralNodeRegistry, which
 * already owns planning-time domain data (real Node/Load objects, route/
 * Hop Count/last-seen) converted from received NodeReportPacket traffic;
 * this class never duplicates that data, and CentralNodeRegistry never
 * duplicates this class's identity/lifecycle data. A caller that needs
 * both (for example to publish state/nodes) joins the two by MAC address.
 *
 * This class never performs ESP-NOW communication itself (see
 * CommissioningPackets/EspNowCommunication), never decides *when* a
 * commissioning command should be sent, and never validates anything about
 * GPIO, battery I2C, Branches or Loads. Smart-node load configuration is
 * owned locally by SmartNodeConfigurationStore and is cleared by that Node
 * when it confirms decommissioning.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#ifndef KILOWATTS_NODE_COMMISSIONING_REGISTRY_H
#define KILOWATTS_NODE_COMMISSIONING_REGISTRY_H

#include "NodeLifecycle.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


class NodeCommissioningRegistry {

public:

    static constexpr std::size_t FRIENDLY_NAME_BUFFER_SIZE = 20U;
    static constexpr std::size_t FIRMWARE_VERSION_BUFFER_SIZE = 12U;
    static constexpr std::size_t CHIP_MODEL_BUFFER_SIZE = 16U;
    static constexpr std::size_t MAX_RELAY_GPIO_CAPABILITIES = 8U;

    /** Whether commissioning config Central holds for this Node currently matches what the Node itself has confirmed applying. */
    enum class SyncState : std::uint8_t {
        SYNCED = 0U,
        PENDING = 1U,
        FAILED = 2U
    };

    static constexpr std::size_t RESET_REASON_BUFFER_SIZE = 16U;

    /**
     * Live runtime facts (see ChipInfo) - deliberately not identity/
     * lifecycle data. Refreshed by every IdentityReportPacket (or, for
     * Central's own record, at boot) via updateDiagnostics(), and
     * deliberately excluded from PersistedCommissioningRecord: these are
     * transient readings, not durable commissioning facts, so a reboot
     * always rebuilds them fresh rather than trusting a stale flash copy.
     */
    struct Diagnostics {
        std::uint32_t freeHeapBytes = 0U;
        std::uint32_t minFreeHeapBytes = 0U;
        std::uint32_t flashSizeBytes = 0U;
        std::uint32_t psramSizeBytes = 0U;
        std::uint16_t siliconRevision = 0U;
        std::uint8_t cpuCores = 0U;
        char resetReason[RESET_REASON_BUFFER_SIZE] = {};
    };

    struct CommissioningRecord {
        MacAddress macAddress;
        NodeRole role;
        NodeLifecycleState lifecycleState;

        /** Committed only once the Node's own CommissionAckPacket confirms success (see applyCommissionResult()). */
        char friendlyName[FRIENDLY_NAME_BUFFER_SIZE];

        /** The name a beginCommissioning()/requestCommissioning()-in-flight command asked for; meaningless once syncState is not PENDING. */
        char pendingFriendlyName[FRIENDLY_NAME_BUFFER_SIZE];

        char firmwareVersion[FIRMWARE_VERSION_BUFFER_SIZE];
        char chipModel[CHIP_MODEL_BUFFER_SIZE];

        /** Board-safe relay outputs reported by the flashed Node image. */
        std::uint8_t relayCapabilityCount;
        std::array<std::uint8_t, MAX_RELAY_GPIO_CAPABILITIES> relayPins;

        std::uint32_t discoveredAtMilliseconds;
        SyncState syncState;

        /** Zero-valued until the first updateDiagnostics() call. */
        Diagnostics diagnostics;
    };


    NodeCommissioningRegistry();


    /**
     * Records a real IdentityReportPacket heard from macAddress.
     *
     * A MAC address never seen before creates a new UNCOMMISSIONED record
     * and this returns true. An existing record has its
     * firmwareVersion/chipModel/discoveredAtMilliseconds (last-seen)
     * refreshed and this returns false - except a DECOMMISSIONED record,
     * which is moved to UNCOMMISSIONED instead (rediscovering a physically
     * present, previously decommissioned device makes it eligible for
     * re-commissioning again; NodeLifecycle only allows DECOMMISSIONED to
     * exit toward UNCOMMISSIONED, so this is the one place that happens).
     * A record currently CONFIGURING/COMMISSIONED/OPERATIONAL keeps its
     * lifecycleState untouched by this call either way.
     */
    bool recordDiscovered(const MacAddress& macAddress, NodeRole role,
                           const char* firmwareVersion, const char* chipModel,
                           std::uint32_t nowMilliseconds);

    /** Same discovery update, including the Node's board-declared relay GPIO inventory. */
    bool recordDiscovered(const MacAddress& macAddress, NodeRole role,
                          const char* firmwareVersion, const char* chipModel,
                          const std::uint8_t* relayPins, std::size_t relayCapabilityCount,
                          std::uint32_t nowMilliseconds);


    /**
     * Central registers its own local identity directly as COMMISSIONED -
     * no ESP-NOW round trip is needed for a Node to commission itself.
     * A no-op (returns false) if a record for macAddress already exists.
     * relayPins/relayCapabilityCount is Central's own board-declared relay
     * GPIO inventory (see CentralNodeConfig::VERIFIED_RELAY_GPIO_PINS),
     * the same board-safety contract recordDiscovered() already carries
     * for Smart Nodes.
     */
    bool registerSelf(const MacAddress& macAddress, NodeRole role, const char* friendlyName,
                       const char* firmwareVersion, const char* chipModel,
                       const std::uint8_t* relayPins, std::size_t relayCapabilityCount,
                       std::uint32_t nowMilliseconds);


    /**
     * Overwrites the live runtime Diagnostics for an existing record - see
     * Diagnostics's own comment for why this is separate from
     * recordDiscovered()/registerSelf() and never persisted.
     *
     * Returns false when no record exists for macAddress.
     */
    bool updateDiagnostics(const MacAddress& macAddress, const Diagnostics& diagnostics);


    /**
     * Begins commissioning (first time) or renaming (already commissioned)
     * macAddress with friendlyName. Rejected (returns false, no change
     * made) when: no record exists for macAddress (a Node must be
     * discovered before it can be commissioned - see NodeCommissioningRegistry's
     * own file comment), friendlyName is empty or does not fit
     * FRIENDLY_NAME_BUFFER_SIZE, or the current lifecycleState is neither
     * {UNCOMMISSIONED, DISCOVERED} (first commissioning) nor
     * {COMMISSIONED, OPERATIONAL} (rename) - for example a commissioning
     * command already in flight (CONFIGURING) or a DECOMMISSIONED record.
     *
     * On acceptance: a first commissioning moves lifecycleState to
     * CONFIGURING; a rename leaves lifecycleState unchanged. Either way,
     * pendingFriendlyName is set and syncState becomes PENDING - the
     * caller must still send the matching CommissionCommandPacket over
     * ESP-NOW and later call applyCommissionResult() with the Node's own
     * reply.
     */
    bool requestCommissioning(const MacAddress& macAddress, const char* friendlyName);


    /**
     * Applies a Node's own CommissionAckPacket. lifecycleState is always
     * set to resultingState - this class trusts the Node's own report of
     * its true local state over anything Central assumed, exactly like
     * the commissioning flow requires (the Node itself is authoritative
     * for whether it actually applied the change). When success is true,
     * friendlyName is committed from pendingFriendlyName and syncState
     * becomes SYNCED; otherwise pendingFriendlyName is discarded,
     * friendlyName is left exactly as it was, and syncState becomes
     * FAILED.
     *
     * Returns false when no record exists for macAddress.
     */
    bool applyCommissionResult(const MacAddress& macAddress, bool success, NodeLifecycleState resultingState);


    /**
     * Immediately marks macAddress DECOMMISSIONED at Central - Central is
     * authoritative for whether it plans around a Node, so this does not
     * wait for the Node's own DecommissionAckPacket (the caller still
     * sends DecommissionCommandPacket over ESP-NOW as a best-effort
     * notification so the Node resets its own local identity too, see
     * NodeIdentityStore). friendlyName/pendingFriendlyName are cleared.
     *
     * Returns false when no record exists for macAddress, or the current
     * lifecycleState cannot legally reach DECOMMISSIONED (see
     * NodeLifecycle::isValidNodeLifecycleTransition() - only FACTORY
     * cannot).
     */
    bool decommission(const MacAddress& macAddress);


    std::size_t getCount() const;

    /** Returns nullptr when recordIndex does not exist. */
    const CommissioningRecord* getRecord(std::size_t recordIndex) const;

    /** Returns nullptr when no record exists for macAddress. */
    const CommissioningRecord* findByMac(const MacAddress& macAddress) const;


    /**
     * Loads every persisted record from NVS (ESP32 target only) into
     * memory, replacing whatever was previously held in memory. Only
     * records that had reached COMMISSIONED/OPERATIONAL/DECOMMISSIONED are
     * ever persisted (see persist()) - a transient UNCOMMISSIONED/
     * DISCOVERED/CONFIGURING record is always rebuilt fresh from real
     * ESP-NOW discovery after a reboot rather than restored from a stale
     * snapshot.
     *
     * Returns false, and leaves this object's in-memory records unchanged,
     * on a host build, when nothing has ever been persisted, or when the
     * persisted blob is corrupt/malformed/an unsupported schema version -
     * a corrupt record is never silently accepted, and this never
     * reconstructs demo records as a fallback.
     */
    bool loadPersisted();


    /**
     * Persists every in-memory record whose lifecycleState is
     * COMMISSIONED, OPERATIONAL or DECOMMISSIONED to NVS (ESP32 target
     * only) as one schema-versioned blob, so a later loadPersisted()
     * restores exactly this state.
     *
     * Returns false on a host build, or when the underlying NVS write
     * failed; the in-memory records are unaffected either way.
     */
    bool persist() const;


private:

    static bool isValidFriendlyName(const char* friendlyName);
    static void copyTruncated(char* destination, std::size_t destinationSize, const char* source);

    CommissioningRecord* findMutable(const MacAddress& macAddress);

    std::vector<CommissioningRecord> records_;
};


} // namespace kilowatts

#endif // KILOWATTS_NODE_COMMISSIONING_REGISTRY_H
