#ifndef KILOWATTS_BEST_FIRST_SEARCH_H
#define KILOWATTS_BEST_FIRST_SEARCH_H

#include "CurrentTimeProvider.h"
#include "Load.h"
#include "LoadScheduleEvaluator.h"

#include <vector>

namespace kilowatts {


class BestFirstSearch {

public:

    /**
     * Runs Best-First Search for the supplied Auto Loads.
     *
     * The vectors store pointers to existing Load objects; the Loads are
     * not copied or recreated.
     */
    BestFirstSearch(
        float availablePowerWatts,
        const std::vector<const Load*>& automaticLoads,
        const CurrentTimeProvider& currentTimeProvider
    );


    /** Returns the Auto Load combination selected by the search. */
    const std::vector<const Load*>& getBestCombination() const;


private:

    /** Runs the search and stores its result in bestCombination_. */
    void runSearch();


    /** Returns the sum of the power ratings in one combination. */
    static float calculateTotalPowerWatts(
        const std::vector<const Load*>& combination
    );


    /** Returns true when a combination does not exceed available power. */
    bool isWithinAvailablePower(
        const std::vector<const Load*>& combination
    ) const;


    /**
     * Calculates g(n).
     * One Load is added at each step, so g(n) is the number of selected
     * Loads in the combination.
     */
    static float calculateG(
        const std::vector<const Load*>& combination
    );


    /**
     * Calculates h(n) using the average normalized Load power plus the
     * average future-schedule penalty returned by LoadScheduleEvaluator.
     * The evaluator obtains real local time from currentTimeProvider_.
     */
    float calculateH(
        const std::vector<const Load*>& combination
    ) const;


    /** Calculates f(n) = g(n) + h(n). */
    static float calculateF(float g, float h);


    /** Power available to the Auto Load search. */
    float availablePowerWatts_;


    /** Existing provider of valid real local time for schedule evaluation. */
    const CurrentTimeProvider& currentTimeProvider_;


    /** Evaluates each Auto Load's schedule against currentTimeProvider_. */
    LoadScheduleEvaluator scheduleEvaluator_;


    /** Pointers to the existing Auto Loads supplied by LoadFilter. */
    std::vector<const Load*> automaticLoads_;


    /** Pointers to the Loads selected by Best-First Search. */
    std::vector<const Load*> bestCombination_;
};


} // namespace kilowatts

#endif // KILOWATTS_BEST_FIRST_SEARCH_H