/**
 * @file BestFirstSearch.cpp
 * @brief Implements the Best-First Search used to select Auto candidate
 *        Load objects.
 *
 * Power preparation (AvailablePowerManager), Load classification
 * (LoadFilter), schedule evaluation (LoadScheduleEvaluator), battery-state
 * estimation, hardware measurement, relay control and communication are
 * all handled outside this class.
 *
 * OPEN is implemented as a binary min-heap ordered by f(i).
 */

#include "BestFirstSearch.h"

#include <algorithm>
#include <cmath>

/*
 * esp_log.h and every ESP_LOGx() call are only compiled on a real ESP32
 * build (ESP_PLATFORM is the macro ESP-IDF defines for every component
 * build). This matches the pattern already used by INA219Monitor.cpp: the
 * host build (run_cpp_test.sh, plain g++, no ESP-IDF headers available)
 * simply compiles without logging rather than needing a fake stub header,
 * and production logging on the real target is completely unchanged.
 */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace kilowatts {


#ifdef ESP_PLATFORM
static const char *TAG = "BEST_FIRST_SEARCH";
#endif


BestFirstSearch::BestFirstSearch()
    : searchScoreWeightsConfigured_(false),
      weights_{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0U},
      searchHasStarted_(false),
      searchHasCompleted_(false),
      planningState_{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
      remainingPowerWatts_(0.0F),
      committedPowerWatts_(0.0F),
      batteryStressTerm_(0.0F)
{
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "BestFirstSearch created");
#endif
}


bool BestFirstSearch::setSearchScoreWeights(const Weights& weights)
{
    if (searchHasStarted_) {
#ifdef ESP_PLATFORM
        ESP_LOGW(
            TAG,
            "Search score weights rejected because a search has already started"
        );
#endif
        return false;
    }

    if (!areWeightsValid(weights)) {
#ifdef ESP_PLATFORM
        ESP_LOGW(
            TAG,
            "Search score weights rejected: wP=%.3f wS=%.3f wB=%.3f wQ=%.3f wT=%.3f maximumPriority=%u",
            weights.runningPowerWeight,
            weights.startupPowerWeight,
            weights.batteryStressWeight,
            weights.priorityWeight,
            weights.scheduleWeight,
            static_cast<unsigned int>(weights.maximumAllowedPriority)
        );
#endif
        return false;
    }

    weights_ = weights;
    searchScoreWeightsConfigured_ = true;

#ifdef ESP_PLATFORM
    ESP_LOGI(
        TAG,
        "Search score weights configured: wP=%.3f wS=%.3f wB=%.3f wQ=%.3f wT=%.3f maximumPriority=%u",
        weights_.runningPowerWeight,
        weights_.startupPowerWeight,
        weights_.batteryStressWeight,
        weights_.priorityWeight,
        weights_.scheduleWeight,
        static_cast<unsigned int>(weights_.maximumAllowedPriority)
    );
#endif

    return true;
}


bool BestFirstSearch::startSearch(
    const ElectricalPlanningState& planningState
)
{
    if (!searchScoreWeightsConfigured_ ||
        !areWeightsValid(weights_) ||
        !isElectricalPlanningStateValid(planningState)) {

#ifdef ESP_PLATFORM
        ESP_LOGW(
            TAG,
            "Search start rejected: weightsConfigured=%s planningStateValid=%s",
            searchScoreWeightsConfigured_ ? "Yes" : "No",
            isElectricalPlanningStateValid(planningState) ? "Yes" : "No"
        );
#endif
        return false;
    }

    if (searchHasStarted_ && !searchHasCompleted_) {
#ifdef ESP_PLATFORM
        ESP_LOGW(
            TAG,
            "Search start rejected because the current search has not completed"
        );
#endif
        return false;
    }

    resetSearch();

    planningState_ = planningState;

    remainingPowerWatts_ =
        planningState.powerAvailableForAutoLoadsWatts;

    committedPowerWatts_ =
        planningState.initialCommittedPowerWatts;

    /*
     * B = clamp((SoC_warn - SoC) / (SoC_warn - SoC_min), 0, 1). The
     * denominator is guaranteed strictly positive by
     * isElectricalPlanningStateValid(), so B is computed once here rather
     * than once per candidate, since it does not depend on any
     * per-candidate quantity.
     */
    const float batteryStressDenominator =
        planningState.warningStateOfChargePercent -
        planningState.minimumStateOfChargePercent;

    batteryStressTerm_ = limitValueToRange(
        (planningState.warningStateOfChargePercent -
         planningState.stateOfChargePercent) /
            batteryStressDenominator,
        0.0F,
        1.0F
    );

    searchHasStarted_ = true;

    const bool searchStateValid = isCurrentSearchStateValid();

#ifdef ESP_PLATFORM
    if (searchStateValid) {
        ESP_LOGI(
            TAG,
            "Search started: SoC=%.1f%% Ptotal=%.3fW Pauto=%.3fW PowerConsumedByActiveLoads=%.3fW B=%.3f",
            planningState_.stateOfChargePercent,
            planningState_.totalAvailablePowerWatts,
            remainingPowerWatts_,
            committedPowerWatts_,
            batteryStressTerm_
        );
    }
    else {
        ESP_LOGE(TAG, "Search state became invalid during startSearch()");
    }
#endif

    return searchStateValid;
}


bool BestFirstSearch::registerBranch(
    BranchId branchId,
    float initialCommittedPowerWatts,
    float maximumCurrentAmps
)
{
    if (!searchHasStarted_ || searchHasCompleted_) {
#ifdef ESP_PLATFORM
        ESP_LOGW(
            TAG,
            "registerBranch() rejected: searchStarted=%s searchCompleted=%s",
            searchHasStarted_ ? "Yes" : "No",
            searchHasCompleted_ ? "Yes" : "No"
        );
#endif
        return false;
    }

    if (!isFiniteAndNonNegative(initialCommittedPowerWatts) ||
        !isFiniteAndNonNegative(maximumCurrentAmps)) {
#ifdef ESP_PLATFORM
        const Load::MacAddress& mac = branchId.nodeMacAddress;
        ESP_LOGW(
            TAG,
            "registerBranch() rejected: branch=%02X:%02X:%02X:%02X:%02X:%02X/pin%u committedPower=%.3fW maximumCurrent=%.3fA",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            static_cast<unsigned int>(branchId.relayPin),
            initialCommittedPowerWatts,
            maximumCurrentAmps
        );
#endif
        return false;
    }

    if (findBranchState(branchId) != nullptr) {
#ifdef ESP_PLATFORM
        const Load::MacAddress& mac = branchId.nodeMacAddress;
        ESP_LOGW(
            TAG,
            "registerBranch() rejected: branch=%02X:%02X:%02X:%02X:%02X:%02X/pin%u already registered",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            static_cast<unsigned int>(branchId.relayPin)
        );
#endif
        return false;
    }

    branches_.push_back(
        BranchState{branchId, initialCommittedPowerWatts, maximumCurrentAmps}
    );

#ifdef ESP_PLATFORM
    {
        const Load::MacAddress& mac = branchId.nodeMacAddress;
        ESP_LOGI(
            TAG,
            "Branch registered: branch=%02X:%02X:%02X:%02X:%02X:%02X/pin%u committedPower=%.3fW maximumCurrent=%.3fA",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            static_cast<unsigned int>(branchId.relayPin),
            initialCommittedPowerWatts,
            maximumCurrentAmps
        );
    }
#endif

    return true;
}


bool BestFirstSearch::addLoad(
    const Load& load,
    BranchId branchId,
    float scheduleFuturePenalty
)
{
    if (!searchHasStarted_ ||
        searchHasCompleted_ ||
        isLoadAlreadyAdded(load)) {

#ifdef ESP_PLATFORM
        ESP_LOGW(
            TAG,
            "Load rejected by addLoad(): name=%s pin=%u searchStarted=%s searchCompleted=%s duplicate=%s",
            load.getName().c_str(),
            static_cast<unsigned int>(load.getRelayPin()),
            searchHasStarted_ ? "Yes" : "No",
            searchHasCompleted_ ? "Yes" : "No",
            isLoadAlreadyAdded(load) ? "Yes" : "No"
        );
#endif
        return false;
    }

    if (findBranchState(branchId) == nullptr) {
#ifdef ESP_PLATFORM
        const Load::MacAddress& branchMac = branchId.nodeMacAddress;
        ESP_LOGW(
            TAG,
            "Load rejected because branch=%02X:%02X:%02X:%02X:%02X:%02X/pin%u was never registered: name=%s",
            branchMac[0], branchMac[1], branchMac[2], branchMac[3], branchMac[4], branchMac[5],
            static_cast<unsigned int>(branchId.relayPin),
            load.getName().c_str()
        );
#endif
        return false;
    }

    if (!std::isfinite(scheduleFuturePenalty) ||
        scheduleFuturePenalty < 0.0F ||
        scheduleFuturePenalty > 1.0F) {

#ifdef ESP_PLATFORM
        ESP_LOGW(
            TAG,
            "Load rejected because scheduleFuturePenalty is invalid: name=%s r_i=%.3f",
            load.getName().c_str(),
            scheduleFuturePenalty
        );
#endif
        return false;
    }

    const LoadPower loadPower = load.getPower();

    if (!std::isfinite(loadPower.runningWatts) ||
        loadPower.runningWatts <= 0.0F ||
        !std::isfinite(loadPower.startupWatts) ||
        loadPower.startupWatts < loadPower.runningWatts ||
        load.getPriority() > weights_.maximumAllowedPriority) {

#ifdef ESP_PLATFORM
        ESP_LOGW(
            TAG,
            "Load rejected because search values are invalid: name=%s pin=%u running=%.3fW startup=%.3fW priority=%u",
            load.getName().c_str(),
            static_cast<unsigned int>(load.getRelayPin()),
            loadPower.runningWatts,
            loadPower.startupWatts,
            static_cast<unsigned int>(load.getPriority())
        );
#endif
        return false;
    }

    loads_.push_back(&load);
    loadBranchIds_.push_back(branchId);
    loadScheduleFuturePenalties_.push_back(scheduleFuturePenalty);

    loadRunningPowerRatios_.push_back(0.0F);
    loadStartupPowerRatios_.push_back(0.0F);
    loadPriorityRatios_.push_back(0.0F);

    loadPhysicalCosts_.push_back(0.0F);
    loadHeuristicCosts_.push_back(0.0F);
    loadFinalScores_.push_back(0.0F);

    loadSelectedToBeOn_.push_back(0U);
    loadSelectionRejectionReasons_.push_back(NONE);

#ifdef ESP_PLATFORM
    const Load::MacAddress& macAddress = load.getMacAddress();

    ESP_LOGI(
        TAG,
        "Load added to search: index=%u name=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X pin=%u branchPin=%u running=%.3fW startup=%.3fW priority=%u r_i=%.3f",
        static_cast<unsigned int>(loads_.size() - 1U),
        load.getName().c_str(),
        macAddress[0],
        macAddress[1],
        macAddress[2],
        macAddress[3],
        macAddress[4],
        macAddress[5],
        static_cast<unsigned int>(load.getRelayPin()),
        static_cast<unsigned int>(branchId.relayPin),
        loadPower.runningWatts,
        loadPower.startupWatts,
        static_cast<unsigned int>(load.getPriority()),
        scheduleFuturePenalty
    );
#endif

    return true;
}


bool BestFirstSearch::run()
{
    if (!isCurrentSearchStateValid() || searchHasCompleted_) {
#ifdef ESP_PLATFORM
        ESP_LOGW(
            TAG,
            "run() rejected: searchStateValid=%s searchCompleted=%s",
            isCurrentSearchStateValid() ? "Yes" : "No",
            searchHasCompleted_ ? "Yes" : "No"
        );
#endif
        return false;
    }

#ifdef ESP_PLATFORM
    ESP_LOGI(
        TAG,
        "Best-First Search running: candidates=%u Ptotal=%.3fW Pauto=%.3fW",
        static_cast<unsigned int>(loads_.size()),
        planningState_.totalAvailablePowerWatts,
        planningState_.powerAvailableForAutoLoadsWatts
    );
#endif

    for (std::size_t loadIndex = 0U;
         loadIndex < loads_.size();
         ++loadIndex) {

        if (!isLoadValidForSearch(loadIndex)) {
#ifdef ESP_PLATFORM
            ESP_LOGE(
                TAG,
                "Search stopped because Load index=%u is invalid",
                static_cast<unsigned int>(loadIndex)
            );
#endif
            return false;
        }
    }

    std::fill(
        loadSelectedToBeOn_.begin(),
        loadSelectedToBeOn_.end(),
        static_cast<std::uint8_t>(0U)
    );

    std::fill(
        loadSelectionRejectionReasons_.begin(),
        loadSelectionRejectionReasons_.end(),
        NONE
    );

    clearOpenSet();


    /* Score every candidate once and insert it into OPEN. */
    for (std::size_t loadIndex = 0U;
         loadIndex < loads_.size();
         ++loadIndex) {

        calculateAndStoreLoadScores(loadIndex);

        if (!std::isfinite(loadRunningPowerRatios_[loadIndex]) ||
            !std::isfinite(loadStartupPowerRatios_[loadIndex]) ||
            !std::isfinite(loadPriorityRatios_[loadIndex]) ||
            !std::isfinite(loadPhysicalCosts_[loadIndex]) ||
            !std::isfinite(loadHeuristicCosts_[loadIndex]) ||
            !std::isfinite(loadFinalScores_[loadIndex])) {

#ifdef ESP_PLATFORM
            ESP_LOGE(
                TAG,
                "Search score calculation produced a non-finite value for Load index=%u",
                static_cast<unsigned int>(loadIndex)
            );
#endif
            clearOpenSet();
            return false;
        }

#ifdef ESP_PLATFORM
        ESP_LOGD(
            TAG,
            "Load scored: index=%u p_i=%.4f s_i=%.4f q_i=%.4f g=%.4f h=%.4f f=%.4f",
            static_cast<unsigned int>(loadIndex),
            loadRunningPowerRatios_[loadIndex],
            loadStartupPowerRatios_[loadIndex],
            loadPriorityRatios_[loadIndex],
            loadPhysicalCosts_[loadIndex],
            loadHeuristicCosts_[loadIndex],
            loadFinalScores_[loadIndex]
        );
#endif

        insertLoadIntoOpenSet(loadIndex);
    }


    /*
     * Repeatedly extract the smallest f(i) from OPEN and apply the
     * constraint guard. Each extraction is O(log n), so processing n
     * candidates through the heap is O(n log n).
     */
    while (!openSetLoadIndexes_.empty()) {

        const std::size_t loadIndex = extractBestLoadFromOpenSet();

        if (loadIndex >= loads_.size()) {
#ifdef ESP_PLATFORM
            ESP_LOGE(
                TAG,
                "OPEN returned an invalid Load index=%u",
                static_cast<unsigned int>(loadIndex)
            );
#endif
            clearOpenSet();
            return false;
        }

        const std::uint8_t rejectionReason = checkFeasibility(loadIndex);

        if (rejectionReason == NONE) {
            markLoadSelectedToBeOn(loadIndex);
        }
        else {
            markLoadNotSelected(loadIndex, rejectionReason);
        }
    }

    searchHasCompleted_ = true;

#ifdef ESP_PLATFORM
    ESP_LOGI(
        TAG,
        "Best-First Search completed: candidates=%u Premaining=%.3fW PowerConsumedByActiveLoads=%.3fW",
        static_cast<unsigned int>(loads_.size()),
        remainingPowerWatts_,
        committedPowerWatts_
    );
#endif

    return true;
}


void BestFirstSearch::resetSearch()
{
#ifdef ESP_PLATFORM
    ESP_LOGI(
        TAG,
        "Resetting search: previousCandidates=%u",
        static_cast<unsigned int>(loads_.size())
    );
#endif

    searchHasStarted_ = false;
    searchHasCompleted_ = false;

    planningState_ = ElectricalPlanningState{
        0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F
    };
    remainingPowerWatts_ = 0.0F;
    committedPowerWatts_ = 0.0F;
    batteryStressTerm_ = 0.0F;

    branches_.clear();

    loads_.clear();
    loadBranchIds_.clear();
    loadScheduleFuturePenalties_.clear();

    loadRunningPowerRatios_.clear();
    loadStartupPowerRatios_.clear();
    loadPriorityRatios_.clear();

    loadPhysicalCosts_.clear();
    loadHeuristicCosts_.clear();
    loadFinalScores_.clear();

    loadSelectedToBeOn_.clear();
    loadSelectionRejectionReasons_.clear();

    admittedLoadIndexesInOrder_.clear();

    clearOpenSet();
}


std::size_t BestFirstSearch::getNumberOfLoadsAdded() const
{
    return loads_.size();
}


const Load* BestFirstSearch::getLoad(std::size_t loadIndex) const
{
    if (loadIndex >= loads_.size()) {
        return nullptr;
    }

    return loads_[loadIndex];
}


bool BestFirstSearch::isLoadSelectedToBeOn(std::size_t loadIndex) const
{
    if (loadIndex >= loadSelectedToBeOn_.size()) {
        return false;
    }

    return loadSelectedToBeOn_[loadIndex] != 0U;
}


std::uint8_t BestFirstSearch::getLoadSelectionRejectionReason(
    std::size_t loadIndex
) const
{
    if (loadIndex >= loadSelectionRejectionReasons_.size()) {
        /*
         * Defensive sentinel for an out-of-range query, not a real
         * evaluation result: LOW_BATTERY is the guard's first and most
         * conservative check, so an invalid query never reads as
         * admitted (NONE).
         */
        return LOW_BATTERY;
    }

    return loadSelectionRejectionReasons_[loadIndex];
}


std::size_t BestFirstSearch::getNumberOfAdmittedLoads() const
{
    return admittedLoadIndexesInOrder_.size();
}


const Load* BestFirstSearch::getAdmittedLoad(std::size_t admissionPosition) const
{
    if (admissionPosition >= admittedLoadIndexesInOrder_.size()) {
        return nullptr;
    }

    const std::size_t loadIndex = admittedLoadIndexesInOrder_[admissionPosition];
    if (loadIndex >= loads_.size()) {
        return nullptr;
    }

    return loads_[loadIndex];
}


float BestFirstSearch::getTotalAvailablePowerWatts() const
{
    return planningState_.totalAvailablePowerWatts;
}


float BestFirstSearch::getRemainingPowerWatts() const
{
    return remainingPowerWatts_;
}


float BestFirstSearch::getCommittedPowerWatts() const
{
    return committedPowerWatts_;
}


float BestFirstSearch::getBranchCommittedPowerWatts(BranchId branchId) const
{
    const BranchState* branch = findBranchState(branchId);

    if (branch == nullptr) {
        return 0.0F;
    }

    return branch->committedPowerWatts;
}


float BestFirstSearch::getBatteryStressTerm() const
{
    return batteryStressTerm_;
}


float BestFirstSearch::getLoadRunningPowerRatio(std::size_t loadIndex) const
{
    if (loadIndex >= loadRunningPowerRatios_.size()) {
        return 0.0F;
    }

    return loadRunningPowerRatios_[loadIndex];
}


float BestFirstSearch::getLoadStartupPowerRatio(std::size_t loadIndex) const
{
    if (loadIndex >= loadStartupPowerRatios_.size()) {
        return 0.0F;
    }

    return loadStartupPowerRatios_[loadIndex];
}


float BestFirstSearch::getLoadPriorityRatio(std::size_t loadIndex) const
{
    if (loadIndex >= loadPriorityRatios_.size()) {
        return 0.0F;
    }

    return loadPriorityRatios_[loadIndex];
}


float BestFirstSearch::getLoadPhysicalCost(std::size_t loadIndex) const
{
    if (loadIndex >= loadPhysicalCosts_.size()) {
        return 0.0F;
    }

    return loadPhysicalCosts_[loadIndex];
}


float BestFirstSearch::getLoadHeuristicCost(std::size_t loadIndex) const
{
    if (loadIndex >= loadHeuristicCosts_.size()) {
        return 0.0F;
    }

    return loadHeuristicCosts_[loadIndex];
}


float BestFirstSearch::getLoadFinalScore(std::size_t loadIndex) const
{
    if (loadIndex >= loadFinalScores_.size()) {
        return 0.0F;
    }

    return loadFinalScores_[loadIndex];
}


bool BestFirstSearch::areWeightsValid(const Weights& weights) const
{
    return
        isFiniteAndNonNegative(weights.runningPowerWeight) &&
        isFiniteAndNonNegative(weights.startupPowerWeight) &&
        isFiniteAndNonNegative(weights.batteryStressWeight) &&
        isFiniteAndNonNegative(weights.priorityWeight) &&
        isFiniteAndNonNegative(weights.scheduleWeight) &&
        weights.maximumAllowedPriority > 0U;
}


bool BestFirstSearch::isElectricalPlanningStateValid(
    const ElectricalPlanningState& planningState
) const
{
    return
        isFinitePercentage(planningState.stateOfChargePercent) &&
        isFinitePercentage(planningState.minimumStateOfChargePercent) &&
        isFinitePercentage(planningState.warningStateOfChargePercent) &&
        /*
         * SoC_warn must lie strictly above SoC_min so the battery-stress
         * denominator is strictly positive and never divides by zero.
         */
        planningState.warningStateOfChargePercent >
            planningState.minimumStateOfChargePercent &&
        std::isfinite(planningState.batteryBusVoltageVolts) &&
        planningState.batteryBusVoltageVolts > 0.0F &&
        isFiniteAndNonNegative(planningState.maximumBatteryPowerWatts) &&
        isFiniteAndNonNegative(planningState.maximumMainCurrentAmps) &&
        isFiniteAndNonNegative(planningState.totalAvailablePowerWatts) &&
        isFiniteAndNonNegative(planningState.powerAvailableForAutoLoadsWatts) &&
        isFiniteAndNonNegative(planningState.initialCommittedPowerWatts);
}


bool BestFirstSearch::isCurrentSearchStateValid() const
{
    return
        searchScoreWeightsConfigured_ &&
        areWeightsValid(weights_) &&
        searchHasStarted_ &&
        isElectricalPlanningStateValid(planningState_) &&
        isFiniteAndNonNegative(remainingPowerWatts_) &&
        isFiniteAndNonNegative(committedPowerWatts_) &&
        remainingPowerWatts_ <=
            planningState_.powerAvailableForAutoLoadsWatts;
}


bool BestFirstSearch::isLoadValidForSearch(std::size_t loadIndex) const
{
    if (loadIndex >= loads_.size() || loads_[loadIndex] == nullptr) {
        return false;
    }

    const LoadPower loadPower = loads_[loadIndex]->getPower();

    if (!std::isfinite(loadPower.runningWatts) ||
        loadPower.runningWatts <= 0.0F ||
        !std::isfinite(loadPower.startupWatts) ||
        loadPower.startupWatts < loadPower.runningWatts ||
        loads_[loadIndex]->getPriority() > weights_.maximumAllowedPriority) {
        return false;
    }

    if (loadIndex >= loadBranchIds_.size() ||
        findBranchState(loadBranchIds_[loadIndex]) == nullptr) {
        return false;
    }

    if (loadIndex >= loadScheduleFuturePenalties_.size() ||
        !std::isfinite(loadScheduleFuturePenalties_[loadIndex]) ||
        loadScheduleFuturePenalties_[loadIndex] < 0.0F ||
        loadScheduleFuturePenalties_[loadIndex] > 1.0F) {
        return false;
    }

    return true;
}


bool BestFirstSearch::isLoadAlreadyAdded(const Load& load) const
{
    return std::find(
        loads_.begin(),
        loads_.end(),
        &load
    ) != loads_.end();
}


BestFirstSearch::BranchState* BestFirstSearch::findBranchState(
    BranchId branchId
)
{
    for (BranchState& branch : branches_) {
        if (branch.branchId == branchId) {
            return &branch;
        }
    }

    return nullptr;
}


const BestFirstSearch::BranchState* BestFirstSearch::findBranchState(
    BranchId branchId
) const
{
    for (const BranchState& branch : branches_) {
        if (branch.branchId == branchId) {
            return &branch;
        }
    }

    return nullptr;
}


float BestFirstSearch::calculateRunningPowerRatio(
    std::size_t loadIndex
) const
{
    if (!isLoadValidForSearch(loadIndex)) {
        return 0.0F;
    }

    /* p_i = P_i / max(P_available, 1 W). */
    const float runningPowerWatts =
        loads_[loadIndex]->getPower().runningWatts;

    return runningPowerWatts /
           std::max(
               planningState_.totalAvailablePowerWatts,
               1.0F
           );
}


float BestFirstSearch::calculateStartupPowerRatio(
    std::size_t loadIndex
) const
{
    if (!isLoadValidForSearch(loadIndex)) {
        return 0.0F;
    }

    /*
     * s_i = max(0, P_i_peak - P_i) / max(P_available, 1 W). Only the
     * extra current above the Load's own running power is charged here,
     * so a Load with no startup surge scores s_i = 0.
     */
    const LoadPower loadPower = loads_[loadIndex]->getPower();

    const float extraStartupPowerWatts =
        std::max(
            0.0F,
            loadPower.startupWatts - loadPower.runningWatts
        );

    return extraStartupPowerWatts /
           std::max(
               planningState_.totalAvailablePowerWatts,
               1.0F
           );
}


float BestFirstSearch::calculatePriorityRatio(
    std::size_t loadIndex
) const
{
    if (!isLoadValidForSearch(loadIndex) ||
        weights_.maximumAllowedPriority == 0U) {
        return 0.0F;
    }

    /* q_i = W_i / W_max. */
    return limitValueToRange(
        static_cast<float>(
            loads_[loadIndex]->getPriority()
        ) /
        static_cast<float>(
            weights_.maximumAllowedPriority
        ),
        0.0F,
        1.0F
    );
}


float BestFirstSearch::calculatePhysicalCost(
    std::size_t loadIndex
) const
{
    if (loadIndex >= loadRunningPowerRatios_.size() ||
        loadIndex >= loadStartupPowerRatios_.size()) {
        return 0.0F;
    }

    /* g(i) = w_P p_i + w_S s_i + w_B B. */
    return
        (weights_.runningPowerWeight *
         loadRunningPowerRatios_[loadIndex]) +
        (weights_.startupPowerWeight *
         loadStartupPowerRatios_[loadIndex]) +
        (weights_.batteryStressWeight *
         batteryStressTerm_);
}


float BestFirstSearch::calculateHeuristicCost(
    std::size_t loadIndex
) const
{
    if (loadIndex >= loadPriorityRatios_.size() ||
        loadIndex >= loadScheduleFuturePenalties_.size()) {
        return 0.0F;
    }

    /*
     * h(i) = w_Q (1 - q_i) + w_T r_i. r_i was already prepared outside
     * this class (LoadScheduleEvaluator) and is never combined with
     * W_i/q_i — priority and schedule stay two separate additive terms.
     */
    return
        (weights_.priorityWeight *
         (1.0F - loadPriorityRatios_[loadIndex])) +
        (weights_.scheduleWeight *
         loadScheduleFuturePenalties_[loadIndex]);
}


float BestFirstSearch::calculateFinalScore(
    std::size_t loadIndex
) const
{
    if (loadIndex >= loadPhysicalCosts_.size() ||
        loadIndex >= loadHeuristicCosts_.size()) {
        return 0.0F;
    }

    /* f(i) = g(i) + h(i). Smaller is more desirable. */
    return
        loadPhysicalCosts_[loadIndex] +
        loadHeuristicCosts_[loadIndex];
}


void BestFirstSearch::calculateAndStoreLoadScores(
    std::size_t loadIndex
)
{
    if (!isLoadValidForSearch(loadIndex)) {
        return;
    }

    loadRunningPowerRatios_[loadIndex] =
        calculateRunningPowerRatio(loadIndex);

    loadStartupPowerRatios_[loadIndex] =
        calculateStartupPowerRatio(loadIndex);

    loadPriorityRatios_[loadIndex] =
        calculatePriorityRatio(loadIndex);

    loadPhysicalCosts_[loadIndex] =
        calculatePhysicalCost(loadIndex);

    loadHeuristicCosts_[loadIndex] =
        calculateHeuristicCost(loadIndex);

    loadFinalScores_[loadIndex] =
        calculateFinalScore(loadIndex);
}


std::uint8_t BestFirstSearch::checkFeasibility(
    std::size_t loadIndex
) const
{
    if (loadIndex >= loads_.size() || loads_[loadIndex] == nullptr) {
        return LOW_BATTERY;
    }

    const LoadPower loadPower = loads_[loadIndex]->getPower();

    const BranchState* branch = findBranchState(loadBranchIds_[loadIndex]);

    /*
     * branch == nullptr cannot actually happen here (addLoad() already
     * refuses a Load whose branchId was never registered), but a live
     * branch's committed power/limit is still required to build
     * FeasibilityInputs, so this is treated the same as an infeasible
     * branch rather than dereferencing a null pointer.
     */
    FeasibilityInputs inputs{};
    inputs.stateOfChargePercent = planningState_.stateOfChargePercent;
    inputs.minimumStateOfChargePercent = planningState_.minimumStateOfChargePercent;
    inputs.candidateRunningPowerWatts = loadPower.runningWatts;
    inputs.candidatePeakPowerWatts = loadPower.startupWatts;
    inputs.remainingPowerWatts = remainingPowerWatts_;
    inputs.committedPowerWatts = committedPowerWatts_;
    inputs.maximumBatteryPowerWatts = planningState_.maximumBatteryPowerWatts;
    inputs.batteryBusVoltageVolts = planningState_.batteryBusVoltageVolts;
    inputs.maximumMainCurrentAmps = planningState_.maximumMainCurrentAmps;
    inputs.branchCommittedPowerWatts = branch != nullptr ? branch->committedPowerWatts : 0.0F;
    inputs.branchMaximumCurrentAmps = branch != nullptr ? branch->maximumCurrentAmps : 0.0F;

    if (branch == nullptr) {
        return BRANCH_LIMIT_EXCEEDED;
    }

    return checkFeasibility(inputs);
}


std::uint8_t BestFirstSearch::checkFeasibility(const FeasibilityInputs& inputs)
{
    /* Condition 1: SoC > SoC_min. */
    if (inputs.stateOfChargePercent <= inputs.minimumStateOfChargePercent) {
        return LOW_BATTERY;
    }

    /* Condition 2: P_i <= P_remaining. */
    if (inputs.candidateRunningPowerWatts > inputs.remainingPowerWatts) {
        return POWER_LIMIT_EXCEEDED;
    }

    /* Condition 3: P_committed + P_i_peak <= P_battery,max. */
    if (inputs.committedPowerWatts + inputs.candidatePeakPowerWatts >
        inputs.maximumBatteryPowerWatts) {
        return BATTERY_CURRENT_LIMIT;
    }

    /*
     * I_main,start(i) = (P_committed + P_i_peak) / V_B. Callers of this
     * static overload are responsible for supplying a strictly positive
     * batteryBusVoltageVolts (guaranteed for the internal caller by
     * isElectricalPlanningStateValid(); the pre-actuation caller re-reads
     * a live measurement it already knows to be valid before ever
     * reaching this check).
     */
    const float mainStartupCurrentAmps =
        (inputs.committedPowerWatts + inputs.candidatePeakPowerWatts) /
        inputs.batteryBusVoltageVolts;

    /* Condition 4: I_main,start(i) <= I_main,max. */
    if (mainStartupCurrentAmps > inputs.maximumMainCurrentAmps) {
        return MAIN_LIMIT_EXCEEDED;
    }

    /* I_b,start(i) = (P_b + P_i_peak) / V_B. */
    const float branchStartupCurrentAmps =
        (inputs.branchCommittedPowerWatts + inputs.candidatePeakPowerWatts) /
        inputs.batteryBusVoltageVolts;

    /* Condition 5: I_b,start(i) <= I_b,max. */
    if (branchStartupCurrentAmps > inputs.branchMaximumCurrentAmps) {
        return BRANCH_LIMIT_EXCEEDED;
    }

    return NONE;
}


void BestFirstSearch::clearOpenSet()
{
    openSetLoadIndexes_.clear();
}


void BestFirstSearch::insertLoadIntoOpenSet(std::size_t loadIndex)
{
    if (loadIndex >= loadFinalScores_.size()) {
        return;
    }

    openSetLoadIndexes_.push_back(loadIndex);

    moveLoadUpOpenSetUntilOrdered(
        openSetLoadIndexes_.size() - 1U
    );
}


std::size_t BestFirstSearch::extractBestLoadFromOpenSet()
{
    if (openSetLoadIndexes_.empty()) {
        return loads_.size();
    }

    const std::size_t bestLoadIndex = openSetLoadIndexes_.front();

    openSetLoadIndexes_.front() = openSetLoadIndexes_.back();
    openSetLoadIndexes_.pop_back();

    if (!openSetLoadIndexes_.empty()) {
        moveLoadDownOpenSetUntilOrdered(0U);
    }

    return bestLoadIndex;
}


void BestFirstSearch::moveLoadUpOpenSetUntilOrdered(
    std::size_t openSetPosition
)
{
    while (openSetPosition > 0U) {

        const std::size_t parentPosition =
            (openSetPosition - 1U) / 2U;

        if (!doesLeftLoadHaveBetterSearchOrder(
                openSetLoadIndexes_[openSetPosition],
                openSetLoadIndexes_[parentPosition])) {
            break;
        }

        std::swap(
            openSetLoadIndexes_[parentPosition],
            openSetLoadIndexes_[openSetPosition]
        );

        openSetPosition = parentPosition;
    }
}


void BestFirstSearch::moveLoadDownOpenSetUntilOrdered(
    std::size_t openSetPosition
)
{
    while (true) {

        const std::size_t leftChildPosition =
            (2U * openSetPosition) + 1U;

        const std::size_t rightChildPosition =
            leftChildPosition + 1U;

        std::size_t bestPosition = openSetPosition;

        if (leftChildPosition < openSetLoadIndexes_.size() &&
            doesLeftLoadHaveBetterSearchOrder(
                openSetLoadIndexes_[leftChildPosition],
                openSetLoadIndexes_[bestPosition])) {
            bestPosition = leftChildPosition;
        }

        if (rightChildPosition < openSetLoadIndexes_.size() &&
            doesLeftLoadHaveBetterSearchOrder(
                openSetLoadIndexes_[rightChildPosition],
                openSetLoadIndexes_[bestPosition])) {
            bestPosition = rightChildPosition;
        }

        if (bestPosition == openSetPosition) {
            break;
        }

        std::swap(
            openSetLoadIndexes_[openSetPosition],
            openSetLoadIndexes_[bestPosition]
        );

        openSetPosition = bestPosition;
    }
}


bool BestFirstSearch::doesLeftLoadHaveBetterSearchOrder(
    std::size_t leftLoadIndex,
    std::size_t rightLoadIndex
) const
{
    if (leftLoadIndex >= loadFinalScores_.size() ||
        rightLoadIndex >= loadFinalScores_.size()) {
        return false;
    }

    return
        loadFinalScores_[leftLoadIndex] <
        loadFinalScores_[rightLoadIndex];
}


void BestFirstSearch::markLoadSelectedToBeOn(std::size_t loadIndex)
{
    if (!isLoadValidForSearch(loadIndex) ||
        loadIndex >= loadSelectedToBeOn_.size() ||
        loadIndex >= loadSelectionRejectionReasons_.size()) {
        return;
    }

    const float runningPowerWatts =
        loads_[loadIndex]->getPower().runningWatts;

    loadSelectedToBeOn_[loadIndex] = 1U;
    loadSelectionRejectionReasons_[loadIndex] = NONE;

    /* Records the exact extraction/admission order — see getAdmittedLoad(). */
    admittedLoadIndexesInOrder_.push_back(loadIndex);

    /*
     * checkFeasibility() already guaranteed runningPowerWatts <=
     * remainingPowerWatts_, so this subtraction never goes negative.
     */
    remainingPowerWatts_ -= runningPowerWatts;
    committedPowerWatts_ += runningPowerWatts;

    BranchState* branch = findBranchState(loadBranchIds_[loadIndex]);
    if (branch != nullptr) {
        branch->committedPowerWatts += runningPowerWatts;
    }

#ifdef ESP_PLATFORM
    const Load& load = *loads_[loadIndex];
    const Load::MacAddress& macAddress = load.getMacAddress();

    ESP_LOGI(
        TAG,
        "Load selected ON: name=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X pin=%u running=%.3fW Premaining=%.3fW PowerConsumedByActiveLoads=%.3fW",
        load.getName().c_str(),
        macAddress[0],
        macAddress[1],
        macAddress[2],
        macAddress[3],
        macAddress[4],
        macAddress[5],
        static_cast<unsigned int>(load.getRelayPin()),
        runningPowerWatts,
        remainingPowerWatts_,
        committedPowerWatts_
    );
#endif
}


void BestFirstSearch::markLoadNotSelected(
    std::size_t loadIndex,
    std::uint8_t loadSelectionRejectionReason
)
{
    if (loadIndex >= loadSelectedToBeOn_.size() ||
        loadIndex >= loadSelectionRejectionReasons_.size()) {
        return;
    }

    loadSelectedToBeOn_[loadIndex] = 0U;
    loadSelectionRejectionReasons_[loadIndex] =
        loadSelectionRejectionReason;

#ifdef ESP_PLATFORM
    if (loadIndex < loads_.size() && loads_[loadIndex] != nullptr) {

        const Load& load = *loads_[loadIndex];
        const Load::MacAddress& macAddress = load.getMacAddress();

        ESP_LOGW(
            TAG,
            "Load not selected: name=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X pin=%u reason=%u",
            load.getName().c_str(),
            macAddress[0],
            macAddress[1],
            macAddress[2],
            macAddress[3],
            macAddress[4],
            macAddress[5],
            static_cast<unsigned int>(load.getRelayPin()),
            static_cast<unsigned int>(loadSelectionRejectionReason)
        );
    }
#endif
}


float BestFirstSearch::limitValueToRange(
    float value,
    float minimumAllowedValue,
    float maximumAllowedValue
)
{
    return std::max(
        minimumAllowedValue,
        std::min(value, maximumAllowedValue)
    );
}


bool BestFirstSearch::isFiniteAndNonNegative(float value)
{
    return std::isfinite(value) && value >= 0.0F;
}


bool BestFirstSearch::isFinitePercentage(float value)
{
    return std::isfinite(value) && value >= 0.0F && value <= 100.0F;
}


} // namespace kilowatts
