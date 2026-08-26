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

    result.hasEnabledSchedule = schedule.enabled;
    result.isScheduleActive =
        schedule.enabled &&
        isScheduleActive(schedule, currentTimeProvider);

    return true;
}


bool LoadScheduleEvaluator::isTimeWithinSchedule(
    const AutoSchedule& schedule,
    std::uint8_t currentHour,
    std::uint8_t currentMinute)
{
    if (!schedule.enabled ||
        schedule.startHour > 23U ||
        schedule.startMinute > 59U ||
        schedule.endHour > 23U ||
        schedule.endMinute > 59U ||
        currentHour > 23U ||
        currentMinute > 59U) {
        return false;
    }

    const std::uint16_t currentMinutes =
        static_cast<std::uint16_t>(currentHour) * 60U + currentMinute;

    const std::uint16_t startMinutes =
        static_cast<std::uint16_t>(schedule.startHour) * 60U +
        schedule.startMinute;

    const std::uint16_t endMinutes =
        static_cast<std::uint16_t>(schedule.endHour) * 60U +
        schedule.endMinute;

    if (startMinutes == endMinutes) {
        return false;
    }

    if (startMinutes < endMinutes) {
        return currentMinutes >= startMinutes &&
               currentMinutes < endMinutes;
    }

    // A later start than end represents a window crossing midnight.
    return currentMinutes >= startMinutes ||
           currentMinutes < endMinutes;
}


bool LoadScheduleEvaluator::isScheduleActive(
    const AutoSchedule& schedule,
    const CurrentTimeProvider& currentTimeProvider) const
{
    std::uint8_t currentHour = 0U;
    std::uint8_t currentMinute = 0U;

    if (!currentTimeProvider.getCurrentLocalHour(currentHour) ||
        !currentTimeProvider.getCurrentLocalMinute(currentMinute)) {
        return false;
    }

    return isTimeWithinSchedule(
        schedule,
        currentHour,
        currentMinute);
}


} // namespace kilowatts
