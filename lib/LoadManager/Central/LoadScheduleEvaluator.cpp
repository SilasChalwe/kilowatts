/**
 * @file LoadScheduleEvaluator.cpp
 * @brief Implements evaluation of an Auto Load's schedule against real
 *        local time.
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

    // An unscheduled Load and a Load whose schedule has already arrived
    // both receive no penalty; only a Load with a schedule still in the
    // future receives the penalty applied later by BestFirstSearch.
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

    // An unavailable clock can never confirm a schedule is due, regardless
    // of why time isn't available yet (no NTP sync, no manual entry, etc).
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
