
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


bool containsLoad(
    const std::vector<const Load*>& combination,
    const Load* load
)
{
    return std::find(combination.begin(), combination.end(), load) !=
           combination.end();
}


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


bool hasBetterSearchMerit(
    const SearchState& first,
    const SearchState& second
)
{
    if (first.f != second.f) {
        return first.f < second.f;
    }

    // Priority is kept separate from g(n), h(n) and f(n). A larger
    // accumulated priority wins only when two states have the same f(n).
    if (first.totalPriority != second.totalPriority) {
        return first.totalPriority > second.totalPriority;
    }

    if (first.combination.size() != second.combination.size()) {
        return first.combination.size() > second.combination.size();
    }

    return first.totalPowerWatts < second.totalPowerWatts;
}


bool isBetterReturnedCombination(
    const SearchState& candidate,
    const std::vector<const Load*>& currentBest
)
{
    if (candidate.combination.size() != currentBest.size()) {
        return candidate.combination.size() > currentBest.size();
    }

    return calculateTotalPriority(candidate.combination) >
           calculateTotalPriority(currentBest);
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

    std::vector<SearchState> openStates{startState};
    std::vector<std::vector<const Load*>> closedCombinations;

    while (!openStates.empty()) {
        std::stable_sort(
            openStates.begin(),
            openStates.end(),
            hasBetterSearchMerit
        );

        const SearchState currentState = openStates.front();
        openStates.erase(openStates.begin());

        if (isBetterReturnedCombination(currentState, bestCombination_)) {
            bestCombination_ = currentState.combination;
        }

        bool hasFeasibleChild = false;

        for (const Load* load : automaticLoads_) {
            if (load == nullptr || !load->isAuto() ||
                containsLoad(currentState.combination, load)) {
                continue;
            }

            std::vector<const Load*> childCombination =
                currentState.combination;
            childCombination.push_back(load);

            if (!isWithinAvailablePower(childCombination)) {
                continue;
            }

            hasFeasibleChild = true;

            if (combinationAlreadyKnown(
                    childCombination,
                    openStates,
                    closedCombinations)) {
                continue;
            }

            const float childG = calculateG(childCombination);
            const float childH = calculateH(childCombination);

            openStates.push_back(SearchState{
                childCombination,
                calculateTotalPowerWatts(childCombination),
                childG,
                childH,
                calculateF(childG, childH),
                calculateTotalPriority(childCombination)
            });
        }

        if (!hasFeasibleChild) {
            bestCombination_ = currentState.combination;
            return;
        }

        closedCombinations.push_back(currentState.combination);
    }
}


float BestFirstSearch::calculateTotalPowerWatts(
    const std::vector<const Load*>& combination
)
{
    float totalPowerWatts = 0.0F;

    for (const Load* load : combination) {
        assert(load != nullptr);

        if (load != nullptr) {
            totalPowerWatts += load->getPowerRatingWatts();
        }
    }

    return totalPowerWatts;
}


bool BestFirstSearch::isWithinAvailablePower(
    const std::vector<const Load*>& combination
) const
{
    const float totalPowerWatts = calculateTotalPowerWatts(combination);

    return std::isfinite(totalPowerWatts) &&
           totalPowerWatts <= availablePowerWatts_;
}


float BestFirstSearch::calculateG(
    const std::vector<const Load*>& combination
)
{
    return static_cast<float>(combination.size());
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

        const bool scheduleEvaluated = scheduleEvaluator_.evaluateSchedule(
            *load,
            currentTimeProvider_,
            scheduleEvaluation
        );

        assert(scheduleEvaluated);

        // Dividing power by the paper's 1 W normalization value leaves
        // the numerical watt value dimensionless. The evaluator's r_i
        // penalty is then added as the schedule term for this Load.
        totalHeuristicCost += load->getPowerRatingWatts();

        if (scheduleEvaluated) {
            totalHeuristicCost += scheduleEvaluation.futureSchedulePenalty;
        }
    }

    return totalHeuristicCost /
           static_cast<float>(combination.size());
}


float BestFirstSearch::calculateF(float g, float h)
{
    return g + h;
}


} // namespace kilowatts