/**
 * @file LoadFilter.h
 * @brief Declares the separation of known Loads into Fixed and Auto groups.
 *
 * A Fixed Load's ON/OFF state is authoritative and is not reconsidered
 * here. An Auto Load's current ON/OFF state is only this planning cycle's
 * starting point — whether it ends up ON or OFF is decided later by
 * Best-First Search, not by LoadFilter — so AUTO_ON and AUTO_OFF are both
 * classified as Auto candidates.
 *
 * LoadFilter stores pointers to the existing Load objects rather than
 * copying them; a LoadFilter must not outlive the Loads it points to.
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
