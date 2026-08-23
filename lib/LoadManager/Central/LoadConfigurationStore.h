/**
 * @file LoadConfigurationStore.h
 * @brief Declares NVS persistence of user-configured Load priority, mode
 *        and Auto schedule, surviving a Central Node reboot.
 *
 * Once a user has configured a Load via MQTT, Central must not let a
 * later, unrelated ESP-NOW NodeReportPacket silently overwrite that
 * choice; the planning cycle reapplies this store's persisted
 * configuration onto the matching Load object every cycle instead.
 */

#ifndef KILOWATTS_LOAD_CONFIGURATION_STORE_H
#define KILOWATTS_LOAD_CONFIGURATION_STORE_H

#include "Load.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


class LoadConfigurationStore {

public:

    /**
     * One Load's persisted user configuration, identified by exactly the
     * same {Node MAC address, relay pin} addressing as Load::Id.
     */
    struct ConfigurationEntry {
        Load::MacAddress macAddress;
        std::uint8_t relayPin;
        std::uint16_t priority;
        LoadMode::Value mode;
        AutoSchedule schedule;
    };


    LoadConfigurationStore();


    /**
     * Loads every persisted entry from NVS (ESP32 target only), replacing
     * whatever is currently held in memory. Returns false, leaving the
     * in-memory entries unchanged, on a host build, when nothing has ever
     * been persisted, or when the persisted blob is corrupt/malformed.
     */
    bool loadPersisted();


    /**
     * Records/updates the user's configuration for the Load identified by
     * entry.macAddress + entry.relayPin. Updates the in-memory copy only
     * — call persist() to write it to NVS. Rejected (returns false) when
     * entry.schedule.enabled is true and hour/minute is out of range.
     */
    bool setConfiguration(const ConfigurationEntry& entry);


    /** Returns how many Loads currently have a stored configuration entry. */
    std::size_t getNumberOfEntries() const;

    /** Returns nullptr when entryIndex does not exist. */
    const ConfigurationEntry* getEntry(std::size_t entryIndex) const;

    /**
     * Finds the stored entry for {macAddress, relayPin}.
     * Returns false when no entry exists for that Load.
     */
    bool findConfiguration(const Load::MacAddress& macAddress, std::uint8_t relayPin, ConfigurationEntry& entry) const;


    /**
     * Applies the stored entry for load's own identity (if any): priority,
     * then mode, then schedule — applying a schedule to a Fixed Load is a
     * harmless no-op per Load::setSchedule()'s own rule. Returns false,
     * leaving load unchanged, when no stored entry exists for its identity.
     */
    bool applyToLoad(Load& load) const;


    /**
     * Persists every in-memory entry to NVS (ESP32 target only) as one
     * blob for a later loadPersisted() to restore. Returns false on a
     * host build or NVS write failure; in-memory entries are unaffected.
     */
    bool persist() const;


private:

    static bool isValidSchedule(const AutoSchedule& schedule);

    ConfigurationEntry* findMutableEntry(const Load::MacAddress& macAddress, std::uint8_t relayPin);

    std::vector<ConfigurationEntry> entries_;
};


} // namespace kilowatts

#endif // KILOWATTS_LOAD_CONFIGURATION_STORE_H
