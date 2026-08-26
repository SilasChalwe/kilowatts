#include "BestFirstSearch.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>


namespace kilowatts {


namespace {


struct SearchState {
    std::vector<const Load*> combination;
    float totalPowerWatts;
    float g;
    float h;
    float f;
    std::uint64_t totalPriority;
};


// Checks whether a load already exists in the current configuration.
bool containsLoad(
    const std::vector<const Load*>& combination,
    const Load* load
)
{
    return std::find(combination.begin(), combination.end(), load) !=
           combination.end();
}


// Compares two configurations without depending on insertion order.
bool representsSameCombination(
    const std::vector<const Load*>& first,
    const std::vector<const Load*>& second
)
{
    if (first.size() != second.size()) {
        return false;
    }

    for (const Load* load : first) {
        if (!containsLoad(second, load)) {
            return false;
        }
    }

    return true;
}


// Prevents duplicate configurations from being added to the search space.
bool combinationAlreadyKnown(
    const std::vector<const Load*>& combination,
    const std::vector<SearchState>& openStates,
    const std::vector<std::vector<const Load*>>& closedCombinations
)
{
    for (const SearchState& state : openStates) {
        if (representsSameCombination(combination, state.combination)) {
            return true;
        }
    }

    for (const std::vector<const Load*>& closed : closedCombinations) {
        if (representsSameCombination(combination, closed)) {
            return true;
        }
    }

    return false;
}


// Calculates the accumulated priority of a load configuration.
std::uint64_t calculateTotalPriority(
    const std::vector<const Load*>& combination
)
{
    std::uint64_t totalPriority = 0U;

    for (const Load* load : combination) {
        assert(load != nullptr);

        if (load != nullptr) {
            totalPriority += load->getPriority();
        }
    }

    return totalPriority;
}


// Orders search states by priority, search cost, load count,
// and total power demand.
bool hasBetterSearchMerit(
    const SearchState& first,
    const SearchState& second
)
{
    if (first.totalPriority != second.totalPriority) {
        return first.totalPriority > second.totalPriority;
    }

    if (first.f != second.f) {
        return first.f < second.f;
    }

    if (first.combination.size() != second.combination.size()) {
        return first.combination.size() > second.combination.size();
    }

    return first.totalPowerWatts < second.totalPowerWatts;
}


// Compares feasible configurations when selecting the final result.
bool isBetterReturnedCombination(
    const SearchState& candidate,
    const SearchState& currentBest
)
{
    if (candidate.totalPriority != currentBest.totalPriority) {
        return candidate.totalPriority > currentBest.totalPriority;
    }

    if (candidate.combination.size() != currentBest.combination.size()) {
        return candidate.combination.size() >
               currentBest.combination.size();
    }

    return candidate.totalPowerWatts <
           currentBest.totalPowerWatts;
}


} // namespace


BestFirstSearch::BestFirstSearch(
    float availablePowerWatts,
    const std::vector<const Load*>& automaticLoads,
    const CurrentTimeProvider& currentTimeProvider
)
    : availablePowerWatts_(availablePowerWatts),
      currentTimeProvider_(currentTimeProvider),
      scheduleEvaluator_(),
      automaticLoads_(automaticLoads),
      bestCombination_()
{
    assert(std::isfinite(availablePowerWatts_));
    assert(availablePowerWatts_ >= 0.0F);

    for (const Load* load : automaticLoads_) {
        assert(load != nullptr);
        assert(load == nullptr || load->isAuto());
    }

    runSearch();
}


const std::vector<const Load*>& BestFirstSearch::getBestCombination() const
{
    return bestCombination_;
}


void BestFirstSearch::runSearch()
{
    bestCombination_.clear();

    if (!std::isfinite(availablePowerWatts_) ||
        availablePowerWatts_ < 0.0F) {
        return;
    }

    SearchState startState{
        {},
        0.0F,
        calculateG({}),
        calculateH({}),
        0.0F,
        0U
    };

    startState.f = calculateF(startState.g, startState.h);

    SearchState bestState = startState;

    std::vector<SearchState> openStates{
        startState
    };

    std::vector<std::vector<const Load*>> closedCombinations;

    while (!openStates.empty()) {

        // Expand the highest-ranked available search state.
        std::stable_sort(
            openStates.begin(),
            openStates.end(),
            hasBetterSearchMerit
        );

        const SearchState currentState = openStates.front();
        openStates.erase(openStates.begin());

        // Track the best feasible configuration found so far.
        if (isBetterReturnedCombination(currentState, bestState)) {
            bestState = currentState;
        }

        for (const Load* load : automaticLoads_) {

            // Skip invalid, manual, or already selected loads.
            if (load == nullptr ||
                !load->isAuto() ||
                containsLoad(currentState.combination, load)) {
                continue;
            }

            std::vector<const Load*> childCombination =
                currentState.combination;

            childCombination.push_back(load);

            // Reject configurations that exceed available power.
            if (!isWithinAvailablePower(childCombination)) {
                continue;
            }

            // Avoid evaluating the same configuration more than once.
            if (combinationAlreadyKnown(
                    childCombination,
                    openStates,
                    closedCombinations)) {
                continue;
            }

            const float childG =
                calculateG(childCombination);

            const float childH =
                calculateH(childCombination);

            SearchState childState{
                childCombination,
                calculateTotalPowerWatts(childCombination),
                childG,
                childH,
                calculateF(childG, childH),
                calculateTotalPriority(childCombination)
            };

            // Update the current best result before queuing the state.
            if (isBetterReturnedCombination(childState, bestState)) {
                bestState = childState;
            }

            openStates.push_back(childState);
        }

        // Mark the expanded configuration as processed.
        closedCombinations.push_back(currentState.combination);
    }

    // Store the highest-ranked feasible configuration.
    bestCombination_ = bestState.combination;
}


float BestFirstSearch::calculateTotalPowerWatts(
    const std::vector<const Load*>& combination
)
{
    float totalPowerWatts = 0.0F;

    for (const Load* load : combination) {
        assert(load != nullptr);

        if (load != nullptr) {
            totalPowerWatts +=
                load->getPowerRatingWatts();
        }
    }

    return totalPowerWatts;
}


bool BestFirstSearch::isWithinAvailablePower(
    const std::vector<const Load*>& combination
) const
{
    const float totalPowerWatts =
        calculateTotalPowerWatts(combination);

    return std::isfinite(totalPowerWatts) &&
           totalPowerWatts <= availablePowerWatts_;
}


float BestFirstSearch::calculateG(
    const std::vector<const Load*>& combination
)
{
    // Represents the search depth of the current configuration.
    return static_cast<float>(
        combination.size()
    );
}


float BestFirstSearch::calculateH(
    const std::vector<const Load*>& combination
) const
{
    if (combination.empty()) {
        return 0.0F;
    }

    float totalHeuristicCost = 0.0F;

    for (const Load* load : combination) {
        assert(load != nullptr);

        if (load == nullptr) {
            continue;
        }

        LoadScheduleEvaluation scheduleEvaluation{
            false,
            false,
            0.0F
        };

        const bool scheduleEvaluated =
            scheduleEvaluator_.evaluateSchedule(
                *load,
                currentTimeProvider_,
                scheduleEvaluation
            );

        assert(scheduleEvaluated);

        // Include power demand in the heuristic cost.
        totalHeuristicCost +=
            load->getPowerRatingWatts();

        // Include schedule penalty when schedule evaluation succeeds.
        if (scheduleEvaluated) {
            totalHeuristicCost +=
                scheduleEvaluation.futureSchedulePenalty;
        }
    }

    // Return the average heuristic cost of the configuration.
    return totalHeuristicCost /
           static_cast<float>(
               combination.size()
           );
}


float BestFirstSearch::calculateF(
    float g,
    float h
)
{
    // Combines path cost and heuristic cost.
    return g + h;
}


} // namespace kilowatts