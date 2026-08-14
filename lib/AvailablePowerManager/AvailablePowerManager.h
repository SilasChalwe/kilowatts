/**
 * @file AvailablePowerManager.h
 * @brief Declares the power-accounting step that runs before Auto Load
 *        selection.
 *
 * AvailablePowerManager prepares the power information Best-First Search
 * will eventually consume. It maintains three clearly named concepts:
 *
 * - Total Available Power: the amount of power currently available to the
 *   system for supplying Loads. AvailablePowerManager does not measure or
 *   calculate this itself — it is supplied by the caller. The real source
 *   will later come from the Central Node's power-source measurements
 *   (battery/solar); that is a separate, later phase.
 *
 * - Fixed ON Running Power: the total configured Running Power
 *   (LoadPower::runningWatts) already required by every Load currently
 *   classified as Fixed::ON. This is calculated here by traversing an
 *   already-classified LoadFilter, not measured — configured Running
 *   Power and live INA219 measurements (LoadMeasurements) serve different
 *   purposes, and this planning calculation always uses the former.
 *
 * - Power Available for Auto Loads: Total Available Power minus Fixed ON
 *   Running Power, clamped so it never becomes negative.
 *
 * AvailablePowerManager is not a hardware driver. It does not read INA219
 * registers, does not know how Total Available Power was physically
 * measured, does not calculate battery State of Charge, battery/solar
 * voltage or current, does not classify Loads (LoadFilter), and does not
 * run Best-First Search.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#ifndef KILOWATTS_AVAILABLE_POWER_MANAGER_H
#define KILOWATTS_AVAILABLE_POWER_MANAGER_H

#include "LoadFilter.h"

namespace kilowatts {


class AvailablePowerManager {

public:

    AvailablePowerManager();


    /**
     * Calculates Power Available for Auto Loads from an externally
     * supplied Total Available Power and the Fixed ON Loads already
     * classified by loadFilter.
     *
     * Fixed ON Running Power is recalculated from scratch on every call
     * by summing LoadPower::runningWatts across every Load currently in
     * loadFilter's Fixed ON collection.
     *
     * Rejected (returns false, all three values keep whatever they were
     * before this call) when totalAvailablePowerWatts is negative, NaN or
     * infinite. Zero is accepted.
     */
    bool calculateAvailablePower(
        float totalAvailablePowerWatts,
        const LoadFilter& loadFilter
    );


    /** Returns the unchanged value most recently supplied and accepted. */
    float getTotalAvailablePowerWatts() const;


    /** Returns the Running Power committed by every Fixed ON Load. */
    float getFixedOnRunningPowerWatts() const;


    /**
     * Returns Total Available Power minus Fixed ON Running Power, never
     * less than zero.
     */
    float getPowerAvailableForAutoLoadsWatts() const;


private:

    float totalAvailablePowerWatts_;

    float fixedOnRunningPowerWatts_;

    float powerAvailableForAutoLoadsWatts_;
};


} // namespace kilowatts

#endif // KILOWATTS_AVAILABLE_POWER_MANAGER_H
