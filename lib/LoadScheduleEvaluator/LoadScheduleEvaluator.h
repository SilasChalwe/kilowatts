/**
 * @file LoadScheduleEvaluator.h
 * @brief Declares evaluation of an Auto Load's schedule against real
 *        local time.
 *
 * LoadScheduleEvaluator's one responsibility: evaluate an Auto Load's
 * configured AutoSchedule against real local time obtained from
 * CurrentTimeProvider, and produce the schedule terms Chapter 4 defines
 * (a_i, d_i and the resulting future-schedule penalty r_i, Section
 * 4.6.3.3 / Equation 4.32) for the caller to hand to BestFirstSearch.
 *
 * A Load's stored priority (Load::getPriority()) is never read or
 * modified here. Priority and schedule are two separate terms in the
 * Best-First evaluation (h(i) = w_Q(1 - q_i) + w_T r_i, Equation 4.32) —
 * this class only ever produces the schedule half, r_i; it never produces
 * or adjusts a priority value, and BestFirstSearch is the only place the
 * two terms are combined.
 *
 * Only Auto Loads are evaluated. Fixed::ON is already required to remain
 * ON and Fixed::OFF is already required to remain OFF, so a schedule can
 * never override Fixed Load semantics and a Fixed Load is never converted
 * into an Auto Load here.
 *
 * LoadScheduleEvaluator does not own the clock (CurrentTimeProvider does),
 * does not implement NTP itself, does not modify the system clock, does
 * not read INA219, and does not run Best-First Search.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#ifndef KILOWATTS_LOAD_SCHEDULE_EVALUATOR_H
#define KILOWATTS_LOAD_SCHEDULE_EVALUATOR_H

#include "CurrentTimeProvider.h"
#include "Load.h"

namespace kilowatts {


/**
 * Result of evaluating one Auto Load's schedule for the current planning
 * cycle (Section 4.6.3.3).
 *
 * hasEnabledSchedule is a_i: true when the Load has an AutoSchedule
 * enabled, false otherwise.
 *
 * isScheduledTimeDue is d_i: true once real local time has reached or
 * passed the configured hour/minute, false otherwise. Always false when
 * hasEnabledSchedule is false, and also false whenever CurrentTimeProvider
 * does not currently have valid time — an unavailable clock is never
 * treated as "due".
 *
 * futureSchedulePenalty is r_i = a_i(1 - d_i) (Equation 4.32): zero for a
 * Load with no enabled schedule, zero again once a configured schedule
 * becomes due, and one for a Load whose configured schedule has not yet
 * arrived. This is the exact value BestFirstSearch multiplies by w_T when
 * computing h(i) — it is not a priority value and is never combined with
 * W_i here.
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
