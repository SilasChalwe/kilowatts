/**
 * @file AvailablePowerManager.cpp
 * @brief Implements the power-accounting step that runs before Auto Load
 *        selection.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "AvailablePowerManager.h"

#include <cmath>

namespace kilowatts {


namespace {

bool isValidTotalAvailablePower(float totalAvailablePowerWatts)
{
    return std::isfinite(totalAvailablePowerWatts) && totalAvailablePowerWatts >= 0.0F;
}


float calculateFixedOnRunningPowerWatts(const LoadFilter& loadFilter)
{
    float fixedOnRunningPowerWatts = 0.0F;

    for (std::size_t i = 0; i < loadFilter.getNumberOfFixedOnLoads(); ++i) {
        const Load* fixedOnLoad = loadFilter.getFixedOnLoad(i);
        if (fixedOnLoad != nullptr) {
            fixedOnRunningPowerWatts += fixedOnLoad->getPower().runningWatts;
        }
    }

    return fixedOnRunningPowerWatts;
}

} // namespace


AvailablePowerManager::AvailablePowerManager()
    : totalAvailablePowerWatts_(0.0F),
      fixedOnRunningPowerWatts_(0.0F),
      powerAvailableForAutoLoadsWatts_(0.0F)
{
}


bool AvailablePowerManager::calculateAvailablePower(
    float totalAvailablePowerWatts,
    const LoadFilter& loadFilter)
{
    if (!isValidTotalAvailablePower(totalAvailablePowerWatts)) {
        return false;
    }

    const float fixedOnRunningPowerWatts = calculateFixedOnRunningPowerWatts(loadFilter);

    float powerAvailableForAutoLoadsWatts = totalAvailablePowerWatts - fixedOnRunningPowerWatts;
    if (powerAvailableForAutoLoadsWatts < 0.0F) {
        powerAvailableForAutoLoadsWatts = 0.0F;
    }

    totalAvailablePowerWatts_ = totalAvailablePowerWatts;
    fixedOnRunningPowerWatts_ = fixedOnRunningPowerWatts;
    powerAvailableForAutoLoadsWatts_ = powerAvailableForAutoLoadsWatts;

    return true;
}


float AvailablePowerManager::getTotalAvailablePowerWatts() const
{
    return totalAvailablePowerWatts_;
}


float AvailablePowerManager::getFixedOnRunningPowerWatts() const
{
    return fixedOnRunningPowerWatts_;
}


float AvailablePowerManager::getPowerAvailableForAutoLoadsWatts() const
{
    return powerAvailableForAutoLoadsWatts_;
}


} // namespace kilowatts
