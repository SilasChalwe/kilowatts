/**
 * @file NodeCommissioningRegistry.h
 * @brief Central's authoritative record of every Node's identity and
 *        commissioning lifecycle, persisted across a reboot.
 *
 * Deliberately separate from CentralNodeRegistry, which owns planning-time
 * domain data (Node/Load objects, routing, last-seen) converted from
 * NodeReportPacket traffic. Neither class duplicates the other's data; a
 * caller needing both (e.g. to publish state/nodes) joins them by MAC
 * address.
 *
 * Never performs ESP-NOW communication itself, never decides when a
 * commissioning command should be sent, and never validates GPIO, battery
 * I2C, Branch or Load state. Smart-node load configuration is owned
 * locally by SmartNodeConfigurationStore and cleared by that Node when it
 * confirms decommissioning.
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
     * Live runtime facts, deliberately not identity/lifecycle data.
     * Excluded from PersistedCommissioningRecord: these are transient
     * readings, so a reboot always rebuilds them fresh rather than
     * trusting a stale flash copy.
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
     * A MAC never seen before creates a new UNCOMMISSIONED record (returns
     * true). An existing record has firmwareVersion/chipModel/last-seen
     * refreshed and returns false - except a DECOMMISSIONED record, which
     * moves back to UNCOMMISSIONED here: the only place NodeLifecycle
     * allows that transition, since rediscovering a physically present
     * device makes it eligible for recommissioning.
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
     * no ESP-NOW round trip is needed for a Node to commission itself. A
     * no-op (returns false) if a record for macAddress already exists.
     */
    bool registerSelf(const MacAddress& macAddress, NodeRole role, const char* friendlyName,
                       const char* firmwareVersion, const char* chipModel,
                       const std::uint8_t* relayPins, std::size_t relayCapabilityCount,
                       std::uint32_t nowMilliseconds);


    /**
     * Overwrites the live runtime Diagnostics for an existing record.
     * Returns false when no record exists for macAddress.
     */
    bool updateDiagnostics(const MacAddress& macAddress, const Diagnostics& diagnostics);


    /**
     * Begins commissioning (first time) or renames (already commissioned)
     * macAddress. Rejected when no record exists, friendlyName is invalid,
     * or lifecycleState is neither {UNCOMMISSIONED, DISCOVERED} (first
     * commissioning) nor {COMMISSIONED, OPERATIONAL} (rename) - e.g. a
     * command already in flight or a DECOMMISSIONED record.
     *
     * On acceptance, pendingFriendlyName is set and syncState becomes
     * PENDING; the caller must still send CommissionCommandPacket over
     * ESP-NOW and later call applyCommissionResult() with the reply.
     */
    bool requestCommissioning(const MacAddress& macAddress, const char* friendlyName);


    /**
     * Applies a Node's own CommissionAckPacket. lifecycleState is always
     * set to resultingState - the Node's own report of its state is
     * authoritative over anything Central assumed. On success,
     * friendlyName is committed from pendingFriendlyName and syncState
     * becomes SYNCED; otherwise it's discarded and syncState becomes
     * FAILED. Returns false when no record exists for macAddress.
     */
    bool applyCommissionResult(const MacAddress& macAddress, bool success, NodeLifecycleState resultingState);


    /**
     * Immediately marks macAddress DECOMMISSIONED at Central without
     * waiting for the Node's own DecommissionAckPacket - Central is
     * authoritative for whether it plans around a Node. The caller still
     * sends DecommissionCommandPacket as a best-effort notification (see
     * NodeIdentityStore). Returns false when no record exists, or the
     * current lifecycleState cannot legally reach DECOMMISSIONED.
     */
    bool decommission(const MacAddress& macAddress);


    std::size_t getCount() const;

    /** Returns nullptr when recordIndex does not exist. */
    const CommissioningRecord* getRecord(std::size_t recordIndex) const;

    /** Returns nullptr when no record exists for macAddress. */
    const CommissioningRecord* findByMac(const MacAddress& macAddress) const;


    /**
     * Loads every persisted record from NVS (ESP32 target only), replacing
     * whatever is currently in memory. Only COMMISSIONED/OPERATIONAL/
     * DECOMMISSIONED records are ever persisted; transient states are
     * always rebuilt from real ESP-NOW discovery after a reboot.
     *
     * Returns false, leaving in-memory records unchanged, on a host build,
     * when nothing has been persisted, or when the persisted blob is
     * corrupt/malformed/an unsupported schema version.
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
