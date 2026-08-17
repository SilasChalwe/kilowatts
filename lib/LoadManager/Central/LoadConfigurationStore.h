/**
 * @file LoadConfigurationStore.h
 * @brief Declares NVS persistence of user-configured Load priority, mode
 *        and Auto schedule, surviving a Central Node reboot.
 *
 * LoadConfigurationStore's one responsibility: remember the user's own
 * choices (priority, Fixed/Auto x ON/OFF mode, Auto schedule) for every
 * Load Central has ever been told about, across a reboot. Once a user has
 * configured a Load (through an MQTT command — see MqttManager), Central
 * must not let a later, unrelated ESP-NOW NodeReportPacket silently
 * overwrite that choice; Central's own planning cycle applies this
 * store's persisted configuration onto the matching Load object every
 * cycle instead.
 *
 * This class never decides Best-First Search results, never calculates
 * power, never performs ESP-NOW/MQTT itself, and never actuates a relay
 * — it purely remembers and reapplies user configuration onto an
 * already-existing kilowatts::Load object supplied by the caller
 * (typically found via CentralNodeRegistry).
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
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
     * Loads every persisted entry from NVS (ESP32 target only) into
     * memory, replacing whatever was previously held in memory.
     *
     * Returns false, and leaves this object's in-memory entries
     * unchanged, on a host build, when nothing has ever been persisted,
     * or when the persisted blob is corrupt/malformed — a corrupt record
     * is never silently accepted.
     */
    bool loadPersisted();


    /**
     * Records/updates the user's configuration for the Load identified by
     * entry.macAddress + entry.relayPin (an existing entry for the same
     * Load is replaced, not duplicated). This updates the in-memory copy
     * only — call persist() to write it to NVS.
     *
     * Rejected (returns false, no change made) when entry.schedule.enabled
     * is true and hour/minute is out of range.
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
     * Applies the stored entry for load's own identity (if any) onto
     * load: priority, then mode, then schedule (schedule is applied last
     * and only takes effect when the resulting mode is Auto, matching
     * Load::setSchedule()'s own rule — applying it to a Fixed Load is a
     * harmless no-op, not an error).
     *
     * Returns false, leaving load completely unchanged, when no stored
     * entry exists for load's identity — a Load Central has not yet
     * received a user configuration for keeps whatever configuration its
     * own NodeReportPacket last carried.
     */
    bool applyToLoad(Load& load) const;


    /**
     * Persists every in-memory entry to NVS (ESP32 target only) as one
     * blob, so a later loadPersisted() (for example after a reboot)
     * restores exactly this state.
     *
     * Returns false on a host build, or when the underlying NVS write
     * failed; the in-memory entries are unaffected either way.
     */
    bool persist() const;


private:

    static bool isValidSchedule(const AutoSchedule& schedule);

    ConfigurationEntry* findMutableEntry(const Load::MacAddress& macAddress, std::uint8_t relayPin);

    std::vector<ConfigurationEntry> entries_;
};


} // namespace kilowatts

#endif // KILOWATTS_LOAD_CONFIGURATION_STORE_H
