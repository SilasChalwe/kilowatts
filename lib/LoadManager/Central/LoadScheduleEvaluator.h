/**
 * @file LoadScheduleEvaluator.h
 * @brief Declares evaluation of an Auto Load's schedule against real
 *        local time.
 *
 * A Load's stored priority is never modified here. The evaluator only
 * reports whether the configured schedule window is active for the
 * current planning cycle; BestFirstSearch applies any temporary planning
 * preference derived from that result.
 *
 * Only Auto Loads are evaluated; a schedule never overrides Fixed Load
 * semantics and a Fixed Load is never converted into an Auto Load here.
 */

#ifndef KILOWATTS_LOAD_SCHEDULE_EVALUATOR_H
#define KILOWATTS_LOAD_SCHEDULE_EVALUATOR_H

#include "CurrentTimeProvider.h"
#include "Load.h"

namespace kilowatts {


/** Result of evaluating one Auto Load's schedule for a planning cycle. */
struct LoadScheduleEvaluation {
    bool hasEnabledSchedule;
    bool isScheduleActive;
};


class LoadScheduleEvaluator {

public:

    /**
     * Evaluates load's schedule against real local time obtained from
     * currentTimeProvider.
     *
     * Returns false, and leaves result unchanged, when load is not Auto.
     * When current time is unavailable, an enabled schedule is reported
     * as inactive rather than guessing whether its window is active.
     */
    bool evaluateSchedule(
        const Load& load,
        const CurrentTimeProvider& currentTimeProvider,
        LoadScheduleEvaluation& result
    ) const;


    /**
     * Pure time-window check used by evaluateSchedule() and host tests.
     * The interval is start-inclusive and end-exclusive. Windows that
     * cross midnight are supported.
     */
    static bool isTimeWithinSchedule(
        const AutoSchedule& schedule,
        std::uint8_t currentHour,
        std::uint8_t currentMinute
    );


private:

    /** Reads current local time and checks whether the schedule is active. */
    bool isScheduleActive(
        const AutoSchedule& schedule,
        const CurrentTimeProvider& currentTimeProvider
    ) const;
};


} // namespace kilowatts

#endif // KILOWATTS_LOAD_SCHEDULE_EVALUATOR_H
