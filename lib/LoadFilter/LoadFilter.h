/**
 * @file LoadFilter.h
 * @brief Declares the separation of known Loads into Fixed and Auto groups.
 *
 * Before Best-First Search can run, every Load Central currently knows
 * about (its own Loads plus every remote Node's Loads, as assembled by
 * CentralNodeRegistry) must be separated according to its existing
 * LoadMode:
 *
 *     FIXED_ON            -> Fixed ON collection
 *     FIXED_OFF           -> Fixed OFF collection
 *     AUTO_ON / AUTO_OFF  -> Auto candidate collection
 *
 * A Fixed Load's ON/OFF state is authoritative and is not reconsidered
 * here. An Auto Load's current ON/OFF state is only this planning
 * cycle's starting point: whether it ends up ON or OFF is decided later
 * by Best-First Search, not by LoadFilter. AUTO_ON and AUTO_OFF are
 * therefore both Auto candidate Loads.
 *
 * LoadFilter stores pointers to the existing Load objects rather than
 * copying them, so every Load keeps the identity (owning Node MAC
 * address + relay pin) that CentralNodeRegistry already established.
 * A LoadFilter must not outlive the Loads it points to.
 *
 * LoadFilter classifies Loads only — that is its one responsibility. It
 * does not calculate Fixed ON Running Power, Total Available Power or
 * Power Available for Auto Loads (see AvailablePowerManager for that). It
 * does not discover Nodes, receive ESP-NOW packets, calculate RSSI or Hop
 * Count, calculate battery State of Charge, run Best-First Search,
 * control relays, or publish MQTT.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */

#ifndef KILOWATTS_LOAD_FILTER_H
#define KILOWATTS_LOAD_FILTER_H

#include "Load.h"

#include <cstddef>
#include <vector>

namespace kilowatts {


class LoadFilter {

public:

    LoadFilter();


    /**
     * Clears every classified Load.
     *
     * Call this before re-traversing CentralNodeRegistry so a fresh
     * classification pass never mixes in Loads left over from an earlier
     * NODE_REPORT.
     */
    void reset();


    /**
     * Classifies one Load by its current LoadMode:
     *
     * FIXED_ON  -> added to the Fixed ON collection.
     * FIXED_OFF -> added to the Fixed OFF collection.
     * AUTO_ON / AUTO_OFF -> added to the Auto candidate collection.
     *
     * The Load is stored by pointer, not copied, so the caller must keep
     * it alive for as long as this LoadFilter is used.
     *
     * Returns true once the Load has been classified into one of the
     * three collections above.
     */
    bool addLoad(const Load& load);


    std::size_t getNumberOfFixedOnLoads() const;

    std::size_t getNumberOfFixedOffLoads() const;

    std::size_t getNumberOfAutoCandidateLoads() const;


    /** Returns nullptr when loadIndex does not exist. */
    const Load* getFixedOnLoad(std::size_t loadIndex) const;

    const Load* getFixedOffLoad(std::size_t loadIndex) const;

    const Load* getAutoCandidateLoad(std::size_t loadIndex) const;


private:

    std::vector<const Load*> fixedOnLoads_;

    std::vector<const Load*> fixedOffLoads_;

    std::vector<const Load*> autoCandidateLoads_;
};


} // namespace kilowatts

#endif // KILOWATTS_LOAD_FILTER_H
