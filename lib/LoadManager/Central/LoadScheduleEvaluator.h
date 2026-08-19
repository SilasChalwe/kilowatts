/**
 * @file LoadScheduleEvaluator.h
 * @brief Declares evaluation of an Auto Load's schedule against real
 *        local time.
 *
 * A Load's stored priority (Load::getPriority()) is never read or
 * modified here — priority and schedule are separate terms in
 * BestFirstSearch's evaluation, and BestFirstSearch is the only place
 * the two are combined.
 *
 * Only Auto Loads are evaluated; a schedule never overrides Fixed Load
 * semantics and a Fixed Load is never converted into an Auto Load here.
 */

#ifndef KILOWATTS_LOAD_SCHEDULE_EVALUATOR_H
#define KILOWATTS_LOAD_SCHEDULE_EVALUATOR_H

#include "CurrentTimeProvider.h"
#include "Load.h"

namespace kilowatts {


/**
 * Result of evaluating one Auto Load's schedule for the current planning
 * cycle.
 *
 * isScheduledTimeDue is always false when hasEnabledSchedule is false,
 * and also false whenever CurrentTimeProvider does not currently have
 * valid time — an unavailable clock is never treated as "due".
 *
 * futureSchedulePenalty is zero for a Load with no enabled schedule, zero
 * again once a configured schedule becomes due, and one for a Load whose
 * configured schedule has not yet arrived. It is not a priority value and
 * is never combined with priority here — only BestFirstSearch does that.
 */
struct LoadScheduleEvaluation {
    bool hasEnabledSchedule;
    bool isScheduledTimeDue;
    float futureSchedulePenalty;
};


class LoadScheduleEvaluator {

public:

    /**
     * Evaluates load's schedule against real local time obtained from
     * currentTimeProvider and fills result with a_i, d_i and r_i.
     *
     * Returns false, and leaves result unchanged, when load is not an
     * Auto Load — a Fixed Load's ON/OFF semantics are never
     * reconsidered by a schedule, so it is never evaluated here.
     */
    bool evaluateSchedule(
        const Load& load,
        const CurrentTimeProvider& currentTimeProvider,
        LoadScheduleEvaluation& result
    ) const;


private:

    /**
     * Compares schedule's hour/minute against real local time obtained
     * from currentTimeProvider. Returns false (never "due") when
     * currentTimeProvider does not currently have valid current time,
     * regardless of which mode/source that time would have come from.
     */
    bool isScheduledTimeDue(
        const AutoSchedule& schedule,
        const CurrentTimeProvider& currentTimeProvider
    ) const;
};


} // namespace kilowatts

#endif // KILOWATTS_LOAD_SCHEDULE_EVALUATOR_H
