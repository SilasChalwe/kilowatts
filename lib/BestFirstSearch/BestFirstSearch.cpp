#include "BestFirstSearch.h"

#include <algorithm>
#include <cmath>

namespace kilowatts {

BestFirstSearch::BestFirstSearch()
    : weightsConfigured_(false),
      searchStarted_(false),
      searchCompleted_(false),
      weights_{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0U},
      planningState_{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
      finalRemainingPowerWatts_(0.0F),
      selectedAutoLoadPowerWatts_(0.0F),
      plannedOnPowerWatts_(0.0F),
      batteryStressTerm_(0.0F)
{
}

bool BestFirstSearch::setSearchScoreWeights(const Weights& weights)
{
    if (searchStarted_ || !areWeightsValid(weights)) return false;
    weights_ = weights;
    weightsConfigured_ = true;
    return true;
}

bool BestFirstSearch::startSearch(const ElectricalPlanningState& state)
{
    if (!weightsConfigured_ || !areWeightsValid(weights_) || !isPlanningStateValid(state)) return false;
    if (searchStarted_ && !searchCompleted_) return false;

    resetSearch();
    planningState_ = state;
    finalRemainingPowerWatts_ = state.powerPassedToBestFirstWatts;
    plannedOnPowerWatts_ = state.fixedOnLoadPowerWatts;

    const float range = state.warningStateOfChargePercent - state.minimumStateOfChargePercent;
    batteryStressTerm_ = clamp(
        (state.warningStateOfChargePercent - state.stateOfChargePercent) / range,
        0.0F,
        1.0F);

    searchStarted_ = true;
    return true;
}

bool BestFirstSearch::addLoad(const Load& load, float scheduleFuturePenalty)
{
    if (!searchStarted_ || searchCompleted_ || isLoadAlreadyAdded(load) ||
        !std::isfinite(scheduleFuturePenalty) ||
        scheduleFuturePenalty < 0.0F || scheduleFuturePenalty > 1.0F) {
        return false;
    }

    const LoadPower power = load.getPower();
    if (!std::isfinite(power.runningWatts) || power.runningWatts <= 0.0F ||
        !std::isfinite(power.startupWatts) || power.startupWatts < power.runningWatts ||
        load.getPriority() > weights_.maximumAllowedPriority) {
        return false;
    }

    loads_.push_back(&load);
    schedulePenalties_.push_back(scheduleFuturePenalty);
    runningPowerRatios_.push_back(0.0F);
    startupPowerRatios_.push_back(0.0F);
    priorityRatios_.push_back(0.0F);
    physicalCosts_.push_back(0.0F);
    heuristicCosts_.push_back(0.0F);
    finalScores_.push_back(0.0F);
    selected_.push_back(0U);
    rejectionReasons_.push_back(NONE);
    return true;
}

bool BestFirstSearch::run()
{
    if (!searchStarted_ || searchCompleted_) return false;

    openSet_.clear();
    admittedOrder_.clear();
    std::fill(selected_.begin(), selected_.end(), 0U);
    std::fill(rejectionReasons_.begin(), rejectionReasons_.end(), NONE);

    for (std::size_t index = 0U; index < loads_.size(); ++index) {
        if (!isLoadValid(index)) return false;
        calculateAndStoreScores(index);
        if (!std::isfinite(finalScores_[index])) return false;
        insertIntoOpenSet(index);
    }

    while (!openSet_.empty()) {
        const std::size_t index = extractBestFromOpenSet();
        const std::uint8_t reason = checkLoadFeasibility(index);
        if (reason == NONE) {
            selectLoad(index);
        } else {
            rejectionReasons_[index] = reason;
        }
    }

    searchCompleted_ = true;
    return true;
}

void BestFirstSearch::resetSearch()
{
    searchStarted_ = false;
    searchCompleted_ = false;
    planningState_ = ElectricalPlanningState{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    finalRemainingPowerWatts_ = 0.0F;
    selectedAutoLoadPowerWatts_ = 0.0F;
    plannedOnPowerWatts_ = 0.0F;
    batteryStressTerm_ = 0.0F;

    loads_.clear();
    schedulePenalties_.clear();
    runningPowerRatios_.clear();
    startupPowerRatios_.clear();
    priorityRatios_.clear();
    physicalCosts_.clear();
    heuristicCosts_.clear();
    finalScores_.clear();
    selected_.clear();
    rejectionReasons_.clear();
    openSet_.clear();
    admittedOrder_.clear();
}

std::size_t BestFirstSearch::getNumberOfLoadsAdded() const { return loads_.size(); }

const Load* BestFirstSearch::getLoad(std::size_t index) const
{
    return index < loads_.size() ? loads_[index] : nullptr;
}

bool BestFirstSearch::isLoadSelectedToBeOn(std::size_t index) const
{
    return index < selected_.size() && selected_[index] != 0U;
}

std::uint8_t BestFirstSearch::getLoadSelectionRejectionReason(std::size_t index) const
{
    return index < rejectionReasons_.size() ? rejectionReasons_[index] : POWER_LIMIT_EXCEEDED;
}

std::size_t BestFirstSearch::getNumberOfAdmittedLoads() const { return admittedOrder_.size(); }

const Load* BestFirstSearch::getAdmittedLoad(std::size_t position) const
{
    return position < admittedOrder_.size() ? getLoad(admittedOrder_[position]) : nullptr;
}

std::uint8_t BestFirstSearch::checkFeasibility(const FeasibilityInputs& in)
{
    if (in.stateOfChargePercent <= in.minimumStateOfChargePercent) return LOW_BATTERY;
    if (in.candidateRunningPowerWatts > in.remainingPowerWatts) return POWER_LIMIT_EXCEEDED;
    if (in.plannedOnPowerWatts + in.candidatePeakPowerWatts > in.maximumBatteryPowerWatts) {
        return BATTERY_CURRENT_LIMIT;
    }
    if (in.batteryBusVoltageVolts <= 0.0F) return MAIN_LIMIT_EXCEEDED;

    const float startupCurrentAmps =
        (in.plannedOnPowerWatts + in.candidatePeakPowerWatts) / in.batteryBusVoltageVolts;
    return startupCurrentAmps > in.maximumMainCurrentAmps ? MAIN_LIMIT_EXCEEDED : NONE;
}

float BestFirstSearch::getPowerBeforeFixedLoadsWatts() const { return planningState_.powerBeforeFixedLoadsWatts; }
float BestFirstSearch::getPowerPassedToBestFirstWatts() const { return planningState_.powerPassedToBestFirstWatts; }
float BestFirstSearch::getFinalRemainingPowerWatts() const { return finalRemainingPowerWatts_; }
float BestFirstSearch::getSelectedAutoLoadPowerWatts() const { return selectedAutoLoadPowerWatts_; }
float BestFirstSearch::getPlannedOnPowerWatts() const { return plannedOnPowerWatts_; }
float BestFirstSearch::getBatteryStressTerm() const { return batteryStressTerm_; }

float BestFirstSearch::getLoadRunningPowerRatio(std::size_t index) const
{
    return index < runningPowerRatios_.size() ? runningPowerRatios_[index] : 0.0F;
}

float BestFirstSearch::getLoadStartupPowerRatio(std::size_t index) const
{
    return index < startupPowerRatios_.size() ? startupPowerRatios_[index] : 0.0F;
}

float BestFirstSearch::getLoadPriorityRatio(std::size_t index) const
{
    return index < priorityRatios_.size() ? priorityRatios_[index] : 0.0F;
}

float BestFirstSearch::getLoadPhysicalCost(std::size_t index) const
{
    return index < physicalCosts_.size() ? physicalCosts_[index] : 0.0F;
}

float BestFirstSearch::getLoadHeuristicCost(std::size_t index) const
{
    return index < heuristicCosts_.size() ? heuristicCosts_[index] : 0.0F;
}

float BestFirstSearch::getLoadFinalScore(std::size_t index) const
{
    return index < finalScores_.size() ? finalScores_[index] : 0.0F;
}

bool BestFirstSearch::areWeightsValid(const Weights& w) const
{
    return isFiniteNonNegative(w.runningPowerWeight) &&
           isFiniteNonNegative(w.startupPowerWeight) &&
           isFiniteNonNegative(w.batteryStressWeight) &&
           isFiniteNonNegative(w.priorityWeight) &&
           isFiniteNonNegative(w.scheduleWeight) &&
           w.maximumAllowedPriority > 0U;
}

bool BestFirstSearch::isPlanningStateValid(const ElectricalPlanningState& s) const
{
    return isFinitePercentage(s.stateOfChargePercent) &&
           isFinitePercentage(s.minimumStateOfChargePercent) &&
           isFinitePercentage(s.warningStateOfChargePercent) &&
           s.warningStateOfChargePercent > s.minimumStateOfChargePercent &&
           std::isfinite(s.batteryBusVoltageVolts) && s.batteryBusVoltageVolts > 0.0F &&
           isFiniteNonNegative(s.maximumBatteryPowerWatts) &&
           isFiniteNonNegative(s.maximumMainCurrentAmps) &&
           isFiniteNonNegative(s.powerBeforeFixedLoadsWatts) &&
           isFiniteNonNegative(s.powerPassedToBestFirstWatts) &&
           isFiniteNonNegative(s.fixedOnLoadPowerWatts) &&
           s.powerPassedToBestFirstWatts <= s.powerBeforeFixedLoadsWatts;
}

bool BestFirstSearch::isLoadValid(std::size_t index) const
{
    if (index >= loads_.size() || loads_[index] == nullptr || index >= schedulePenalties_.size()) return false;
    const LoadPower power = loads_[index]->getPower();
    return std::isfinite(power.runningWatts) && power.runningWatts > 0.0F &&
           std::isfinite(power.startupWatts) && power.startupWatts >= power.runningWatts &&
           loads_[index]->getPriority() <= weights_.maximumAllowedPriority &&
           std::isfinite(schedulePenalties_[index]) &&
           schedulePenalties_[index] >= 0.0F && schedulePenalties_[index] <= 1.0F;
}

bool BestFirstSearch::isLoadAlreadyAdded(const Load& load) const
{
    return std::find(loads_.begin(), loads_.end(), &load) != loads_.end();
}

float BestFirstSearch::calculateRunningPowerRatio(std::size_t index) const
{
    return loads_[index]->getPower().runningWatts /
           std::max(planningState_.powerBeforeFixedLoadsWatts, 1.0F);
}

float BestFirstSearch::calculateStartupPowerRatio(std::size_t index) const
{
    const LoadPower power = loads_[index]->getPower();
    const float extraStartupPower = std::max(0.0F, power.startupWatts - power.runningWatts);
    return extraStartupPower / std::max(planningState_.powerBeforeFixedLoadsWatts, 1.0F);
}

float BestFirstSearch::calculatePriorityRatio(std::size_t index) const
{
    return clamp(
        static_cast<float>(loads_[index]->getPriority()) /
            static_cast<float>(weights_.maximumAllowedPriority),
        0.0F,
        1.0F);
}

float BestFirstSearch::calculatePhysicalCost(std::size_t index) const
{
    return (weights_.runningPowerWeight * runningPowerRatios_[index]) +
           (weights_.startupPowerWeight * startupPowerRatios_[index]) +
           (weights_.batteryStressWeight * batteryStressTerm_);
}

float BestFirstSearch::calculateHeuristicCost(std::size_t index) const
{
    return (weights_.priorityWeight * (1.0F - priorityRatios_[index])) +
           (weights_.scheduleWeight * schedulePenalties_[index]);
}

float BestFirstSearch::calculateFinalScore(std::size_t index) const
{
    return physicalCosts_[index] + heuristicCosts_[index];
}

void BestFirstSearch::calculateAndStoreScores(std::size_t index)
{
    runningPowerRatios_[index] = calculateRunningPowerRatio(index);
    startupPowerRatios_[index] = calculateStartupPowerRatio(index);
    priorityRatios_[index] = calculatePriorityRatio(index);
    physicalCosts_[index] = calculatePhysicalCost(index);
    heuristicCosts_[index] = calculateHeuristicCost(index);
    finalScores_[index] = calculateFinalScore(index);
}

std::uint8_t BestFirstSearch::checkLoadFeasibility(std::size_t index) const
{
    const LoadPower power = loads_[index]->getPower();
    return checkFeasibility(FeasibilityInputs{
        planningState_.stateOfChargePercent,
        planningState_.minimumStateOfChargePercent,
        power.runningWatts,
        power.startupWatts,
        finalRemainingPowerWatts_,
        plannedOnPowerWatts_,
        planningState_.maximumBatteryPowerWatts,
        planningState_.batteryBusVoltageVolts,
        planningState_.maximumMainCurrentAmps});
}

void BestFirstSearch::insertIntoOpenSet(std::size_t index)
{
    openSet_.push_back(index);
    moveUp(openSet_.size() - 1U);
}

std::size_t BestFirstSearch::extractBestFromOpenSet()
{
    const std::size_t best = openSet_.front();
    openSet_.front() = openSet_.back();
    openSet_.pop_back();
    if (!openSet_.empty()) moveDown(0U);
    return best;
}

void BestFirstSearch::moveUp(std::size_t position)
{
    while (position > 0U) {
        const std::size_t parent = (position - 1U) / 2U;
        if (!comesBefore(openSet_[position], openSet_[parent])) break;
        std::swap(openSet_[position], openSet_[parent]);
        position = parent;
    }
}

void BestFirstSearch::moveDown(std::size_t position)
{
    while (true) {
        const std::size_t left = (2U * position) + 1U;
        const std::size_t right = left + 1U;
        std::size_t best = position;

        if (left < openSet_.size() && comesBefore(openSet_[left], openSet_[best])) best = left;
        if (right < openSet_.size() && comesBefore(openSet_[right], openSet_[best])) best = right;
        if (best == position) break;
        std::swap(openSet_[position], openSet_[best]);
        position = best;
    }
}

bool BestFirstSearch::comesBefore(std::size_t leftIndex, std::size_t rightIndex) const
{
    if (finalScores_[leftIndex] != finalScores_[rightIndex]) {
        return finalScores_[leftIndex] < finalScores_[rightIndex];
    }
    return loads_[leftIndex]->getPriority() > loads_[rightIndex]->getPriority();
}

void BestFirstSearch::selectLoad(std::size_t index)
{
    const float runningPower = loads_[index]->getPower().runningWatts;
    selected_[index] = 1U;
    rejectionReasons_[index] = NONE;
    admittedOrder_.push_back(index);

    finalRemainingPowerWatts_ -= runningPower;
    selectedAutoLoadPowerWatts_ += runningPower;
    plannedOnPowerWatts_ += runningPower;
}

bool BestFirstSearch::isFiniteNonNegative(float value)
{
    return std::isfinite(value) && value >= 0.0F;
}

bool BestFirstSearch::isFinitePercentage(float value)
{
    return std::isfinite(value) && value >= 0.0F && value <= 100.0F;
}

float BestFirstSearch::clamp(float value, float minimum, float maximum)
{
    return std::max(minimum, std::min(value, maximum));
}

} // namespace kilowatts
