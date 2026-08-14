/**
 * @file LoadScheduleEvaluator.cpp
 * @brief Implements evaluation of an Auto Load's schedule against real
 *        local time.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "LoadScheduleEvaluator.h"

namespace kilowatts {


bool LoadScheduleEvaluator::evaluateSchedule(
    const Load& load,
    const CurrentTimeProvider& currentTimeProvider,
    LoadScheduleEvaluation& result) const
{
    if (!load.isAuto()) {
        return false;
    }

    const AutoSchedule schedule = load.getSchedule();

    const bool hasEnabledSchedule = schedule.enabled;

    const bool scheduleIsDue =
        hasEnabledSchedule &&
        isScheduledTimeDue(schedule, currentTimeProvider);

    /*
     * r_i = a_i(1 - d_i) (Equation 4.32). An unscheduled Load and a Load
     * whose schedule has already arrived both receive r_i = 0; only a
     * Load with a schedule still in the future receives the r_i = 1
     * penalty applied later by BestFirstSearch.
     */
    const float futureSchedulePenalty =
        (hasEnabledSchedule && !scheduleIsDue) ? 1.0F : 0.0F;

    result.hasEnabledSchedule = hasEnabledSchedule;
    result.isScheduledTimeDue = scheduleIsDue;
    result.futureSchedulePenalty = futureSchedulePenalty;

    return true;
}


bool LoadScheduleEvaluator::isScheduledTimeDue(
    const AutoSchedule& schedule,
    const CurrentTimeProvider& currentTimeProvider) const
{
    std::uint8_t currentHour = 0U;
    std::uint8_t currentMinute = 0U;

    /*
     * Unavailable current time can never confirm a schedule is due — that
     * would be reporting an apparently valid schedule time when the
     * system does not actually have real time right now. This does not
     * care whether that unavailability is because Automatic mode has not
     * synchronized yet or because Manual mode has no entry yet.
     */
    if (!currentTimeProvider.getCurrentLocalHour(currentHour) ||
        !currentTimeProvider.getCurrentLocalMinute(currentMinute)) {
        return false;
    }

    if (currentHour > schedule.hour) {
        return true;
    }

    return currentHour == schedule.hour && currentMinute >= schedule.minute;
}


} // namespace kilowatts
