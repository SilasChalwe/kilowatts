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

    /** One Load's persisted user configuration. */
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
     * whatever is currently held in memory.
     */
    bool loadPersisted();


    /**
     * Records/updates the user's configuration for one Load. An enabled
     * schedule is rejected when either time is out of range or the start
     * and end times are identical.
     */
    bool setConfiguration(const ConfigurationEntry& entry);


    /** Returns how many Loads currently have a stored configuration entry. */
    std::size_t getNumberOfEntries() const;

    /** Returns nullptr when entryIndex does not exist. */
    const ConfigurationEntry* getEntry(std::size_t entryIndex) const;

    /** Finds the stored entry for {macAddress, relayPin}. */
    bool findConfiguration(const Load::MacAddress& macAddress, std::uint8_t relayPin, ConfigurationEntry& entry) const;


    /** Applies stored priority, mode and schedule to the matching Load. */
    bool applyToLoad(Load& load) const;


    /** Persists every in-memory entry to NVS on ESP32 builds. */
    bool persist() const;


private:

    static bool isValidSchedule(const AutoSchedule& schedule);

    ConfigurationEntry* findMutableEntry(const Load::MacAddress& macAddress, std::uint8_t relayPin);

    std::vector<ConfigurationEntry> entries_;
};


} // namespace kilowatts

#endif // KILOWATTS_LOAD_CONFIGURATION_STORE_H
