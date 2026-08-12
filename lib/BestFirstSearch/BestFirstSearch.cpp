/**
 * @file BestFirstSearch.cpp
 * @brief Implements the Best-First Search used to select Load objects.
 *
 * BestFirstSearch receives Load objects and the amount of power available
 * at the start of the search.
 *
 * Power preparation, hardware control and other system processing are
 * handled outside this class.
 *
 * OPEN is implemented as a binary min-heap.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */

#include "BestFirstSearch.h"

#include <algorithm>
#include <cmath>

#include "esp_log.h"


static const char *TAG = "BEST_FIRST_SEARCH";

namespace kilowatts {


BestFirstSearch::BestFirstSearch()
    : searchScoreWeightsConfigured_(false),
      searchHasStarted_(false),
      searchHasCompleted_(false),
      runningPowerScoreWeight_(0.0F),
      startupPowerScoreWeight_(0.0F),
      priorityScoreWeight_(0.0F),
      maximumAllowedPriority_(0U),
      powerAvailableAtStartWatts_(0.0F),
      remainingPowerWatts_(0.0F)
{
    ESP_LOGI(TAG, "BestFirstSearch created");
}


bool BestFirstSearch::setSearchScoreWeights(
    float runningPowerScoreWeight,
    float startupPowerScoreWeight,
    float priorityScoreWeight,
    std::uint16_t maximumAllowedPriority
)
{
    if (searchHasStarted_) {
        ESP_LOGW(
            TAG,
            "Search score weights rejected because a search has already started"
        );
        return false;
    }

    if (!isFiniteAndNonNegative(runningPowerScoreWeight) ||
        !isFiniteAndNonNegative(startupPowerScoreWeight) ||
        !isFiniteAndNonNegative(priorityScoreWeight) ||
        maximumAllowedPriority == 0U) {

        ESP_LOGW(
            TAG,
            "Search score weights rejected: runningWeight=%.3f startupWeight=%.3f priorityWeight=%.3f maximumPriority=%u",
            runningPowerScoreWeight,
            startupPowerScoreWeight,
            priorityScoreWeight,
            static_cast<unsigned int>(maximumAllowedPriority)
        );

        return false;
    }

    runningPowerScoreWeight_ = runningPowerScoreWeight;
    startupPowerScoreWeight_ = startupPowerScoreWeight;
    priorityScoreWeight_ = priorityScoreWeight;
    maximumAllowedPriority_ = maximumAllowedPriority;

    searchScoreWeightsConfigured_ =
        areSearchScoreWeightsValid();

    if (searchScoreWeightsConfigured_) {
        ESP_LOGI(
            TAG,
            "Search score weights configured: runningWeight=%.3f startupWeight=%.3f priorityWeight=%.3f maximumPriority=%u",
            runningPowerScoreWeight_,
            startupPowerScoreWeight_,
            priorityScoreWeight_,
            static_cast<unsigned int>(maximumAllowedPriority_)
        );
    }
    else {
        ESP_LOGE(TAG, "Search score weights failed validation after assignment");
    }

    return searchScoreWeightsConfigured_;
}


bool BestFirstSearch::startSearch(
    float powerAvailableAtStartWatts
)
{
    if (!searchScoreWeightsConfigured_ ||
        !areSearchScoreWeightsValid() ||
        !isFiniteAndNonNegative(powerAvailableAtStartWatts)) {

        ESP_LOGW(
            TAG,
            "Search start rejected: powerAvailableAtStart=%.3fW weightsConfigured=%s",
            powerAvailableAtStartWatts,
            searchScoreWeightsConfigured_ ? "Yes" : "No"
        );

        return false;
    }

    if (searchHasStarted_ && !searchHasCompleted_) {
        ESP_LOGW(
            TAG,
            "Search start rejected because the current search has not completed"
        );
        return false;
    }

    resetSearch();

    powerAvailableAtStartWatts_ =
        powerAvailableAtStartWatts;

    remainingPowerWatts_ =
        powerAvailableAtStartWatts;

    searchHasStarted_ = true;

    const bool searchStateValid =
        isCurrentSearchStateValid();

    if (searchStateValid) {
        ESP_LOGI(
            TAG,
            "Search started: powerAvailableAtStart=%.3fW remainingPower=%.3fW",
            powerAvailableAtStartWatts_,
            remainingPowerWatts_
        );
    }
    else {
        ESP_LOGE(TAG, "Search state became invalid during startSearch()");
    }

    return searchStateValid;
}


bool BestFirstSearch::addLoad(const Load& load)
{
    if (!searchHasStarted_ ||
        searchHasCompleted_ ||
        isLoadAlreadyAdded(load)) {

        ESP_LOGW(
            TAG,
            "Load rejected by addLoad(): name=%s pin=%u searchStarted=%s searchCompleted=%s duplicate=%s",
            load.getName().c_str(),
            static_cast<unsigned int>(load.getRelayPin()),
            searchHasStarted_ ? "Yes" : "No",
            searchHasCompleted_ ? "Yes" : "No",
            isLoadAlreadyAdded(load) ? "Yes" : "No"
        );

        return false;
    }

    const LoadPower loadPower = load.getPower();

    if (!std::isfinite(loadPower.runningWatts) ||
        loadPower.runningWatts <= 0.0F ||
        !std::isfinite(loadPower.startupWatts) ||
        loadPower.startupWatts < loadPower.runningWatts ||
        load.getPriority() > maximumAllowedPriority_) {

        ESP_LOGW(
            TAG,
            "Load rejected because search values are invalid: name=%s pin=%u running=%.3fW startup=%.3fW priority=%u",
            load.getName().c_str(),
            static_cast<unsigned int>(load.getRelayPin()),
            loadPower.runningWatts,
            loadPower.startupWatts,
            static_cast<unsigned int>(load.getPriority())
        );

        return false;
    }

    loads_.push_back(&load);

    loadRunningPowerRatios_.push_back(0.0F);
    loadStartupPowerRatios_.push_back(0.0F);
    loadPriorityRatios_.push_back(0.0F);

    loadPowerScores_.push_back(0.0F);
    loadPriorityScores_.push_back(0.0F);
    loadFinalSearchScores_.push_back(0.0F);

    loadSelectedToBeOn_.push_back(0U);
    loadSelectionRejectionReasons_.push_back(
        LOAD_CAN_FIT_REMAINING_POWER
    );

    const Load::MacAddress& macAddress =
        load.getMacAddress();

    ESP_LOGI(
        TAG,
        "Load added to search: index=%u name=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X pin=%u running=%.3fW startup=%.3fW priority=%u",
        static_cast<unsigned int>(loads_.size() - 1U),
        load.getName().c_str(),
        macAddress[0],
        macAddress[1],
        macAddress[2],
        macAddress[3],
        macAddress[4],
        macAddress[5],
        static_cast<unsigned int>(load.getRelayPin()),
        loadPower.runningWatts,
        loadPower.startupWatts,
        static_cast<unsigned int>(load.getPriority())
    );

    return true;
}


bool BestFirstSearch::run()
{
    if (!isCurrentSearchStateValid() ||
        searchHasCompleted_) {

        ESP_LOGW(
            TAG,
            "run() rejected: searchStateValid=%s searchCompleted=%s",
            isCurrentSearchStateValid() ? "Yes" : "No",
            searchHasCompleted_ ? "Yes" : "No"
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "Best-First Search running: loads=%u powerAvailableAtStart=%.3fW",
        static_cast<unsigned int>(loads_.size()),
        powerAvailableAtStartWatts_
    );

    for (std::size_t loadIndex = 0U;
         loadIndex < loads_.size();
         ++loadIndex) {

        if (!isLoadValidForSearch(loadIndex)) {
            ESP_LOGE(
                TAG,
                "Search stopped because Load index=%u is invalid",
                static_cast<unsigned int>(loadIndex)
            );
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
        LOAD_CAN_FIT_REMAINING_POWER
    );

    clearOpenSet();


    /**
     * Candidate evaluation.
     *
     * runningPowerRatio =
     *     runningPowerWatts / max(powerAvailableAtStartWatts, 1 W)
     *
     * startupPowerRatio =
     *     max(0, startupPowerWatts - runningPowerWatts)
     *     / max(powerAvailableAtStartWatts, 1 W)
     *
     * priorityRatio =
     *     priority / maximumAllowedPriority
     *
     * powerScore =
     *     runningPowerScoreWeight * runningPowerRatio
     *     + startupPowerScoreWeight * startupPowerRatio
     *
     * priorityScore =
     *     priorityScoreWeight * (1 - priorityRatio)
     *
     * finalSearchScore =
     *     powerScore + priorityScore
     *
     * The Load with the smallest finalSearchScore is extracted first.
     */
    for (std::size_t loadIndex = 0U;
         loadIndex < loads_.size();
         ++loadIndex) {

        calculateAndStoreLoadSearchScores(loadIndex);

        if (!std::isfinite(loadRunningPowerRatios_[loadIndex]) ||
            !std::isfinite(loadStartupPowerRatios_[loadIndex]) ||
            !std::isfinite(loadPriorityRatios_[loadIndex]) ||
            !std::isfinite(loadPowerScores_[loadIndex]) ||
            !std::isfinite(loadPriorityScores_[loadIndex]) ||
            !std::isfinite(loadFinalSearchScores_[loadIndex])) {

            ESP_LOGE(
                TAG,
                "Search score calculation produced a non-finite value for Load index=%u",
                static_cast<unsigned int>(loadIndex)
            );

            clearOpenSet();
            return false;
        }

        ESP_LOGD(
            TAG,
            "Load scored: index=%u runningRatio=%.4f startupRatio=%.4f priorityRatio=%.4f powerScore=%.4f priorityScore=%.4f finalScore=%.4f",
            static_cast<unsigned int>(loadIndex),
            loadRunningPowerRatios_[loadIndex],
            loadStartupPowerRatios_[loadIndex],
            loadPriorityRatios_[loadIndex],
            loadPowerScores_[loadIndex],
            loadPriorityScores_[loadIndex],
            loadFinalSearchScores_[loadIndex]
        );

        insertLoadIntoOpenSet(loadIndex);
    }


    /**
     * OPEN is a binary min-heap.
     *
     * Each extraction is O(log n), so processing n Loads through
     * the heap is O(n log n).
     */
    while (!openSetLoadIndexes_.empty()) {

        const std::size_t loadIndex =
            extractBestLoadFromOpenSet();

        if (loadIndex >= loads_.size()) {
            ESP_LOGE(
                TAG,
                "OPEN returned an invalid Load index=%u",
                static_cast<unsigned int>(loadIndex)
            );
            clearOpenSet();
            return false;
        }

        const std::uint8_t remainingPowerCheck =
            getLoadRemainingPowerCheckResult(loadIndex);

        if (remainingPowerCheck ==
            LOAD_CAN_FIT_REMAINING_POWER) {

            markLoadSelectedToBeOn(loadIndex);
        }
        else {
            markLoadNotSelected(
                loadIndex,
                remainingPowerCheck
            );
        }
    }

    searchHasCompleted_ = true;

    ESP_LOGI(
        TAG,
        "Best-First Search completed: loads=%u remainingPower=%.3fW",
        static_cast<unsigned int>(loads_.size()),
        remainingPowerWatts_
    );

    return true;
}


void BestFirstSearch::resetSearch()
{
    ESP_LOGI(
        TAG,
        "Resetting search: previousLoads=%u",
        static_cast<unsigned int>(loads_.size())
    );

    searchHasStarted_ = false;
    searchHasCompleted_ = false;

    powerAvailableAtStartWatts_ = 0.0F;
    remainingPowerWatts_ = 0.0F;

    loads_.clear();

    loadRunningPowerRatios_.clear();
    loadStartupPowerRatios_.clear();
    loadPriorityRatios_.clear();

    loadPowerScores_.clear();
    loadPriorityScores_.clear();
    loadFinalSearchScores_.clear();

    loadSelectedToBeOn_.clear();
    loadSelectionRejectionReasons_.clear();

    clearOpenSet();
}


std::size_t BestFirstSearch::getNumberOfLoadsAdded() const
{
    return loads_.size();
}


float BestFirstSearch::getPowerAvailableAtStartWatts() const
{
    return powerAvailableAtStartWatts_;
}


float BestFirstSearch::getRemainingPowerWatts() const
{
    return remainingPowerWatts_;
}


const Load* BestFirstSearch::getLoad(
    std::size_t loadIndex
) const
{
    if (loadIndex >= loads_.size()) {
        ESP_LOGW(
            TAG,
            "getLoad() received invalid index=%u totalLoads=%u",
            static_cast<unsigned int>(loadIndex),
            static_cast<unsigned int>(loads_.size())
        );
        return nullptr;
    }

    return loads_[loadIndex];
}


bool BestFirstSearch::isLoadSelectedToBeOn(
    std::size_t loadIndex
) const
{
    if (loadIndex >= loadSelectedToBeOn_.size()) {
        ESP_LOGW(
            TAG,
            "isLoadSelectedToBeOn() received invalid index=%u",
            static_cast<unsigned int>(loadIndex)
        );
        return false;
    }

    return loadSelectedToBeOn_[loadIndex] != 0U;
}


std::uint8_t BestFirstSearch::getLoadSelectionRejectionReason(
    std::size_t loadIndex
) const
{
    if (loadIndex >=
        loadSelectionRejectionReasons_.size()) {

        return LOAD_NEEDS_MORE_POWER_THAN_REMAINS;
    }

    return loadSelectionRejectionReasons_[loadIndex];
}


bool BestFirstSearch::areSearchScoreWeightsValid() const
{
    return
        isFiniteAndNonNegative(
            runningPowerScoreWeight_
        ) &&
        isFiniteAndNonNegative(
            startupPowerScoreWeight_
        ) &&
        isFiniteAndNonNegative(
            priorityScoreWeight_
        ) &&
        maximumAllowedPriority_ > 0U;
}


bool BestFirstSearch::isCurrentSearchStateValid() const
{
    return
        searchScoreWeightsConfigured_ &&
        areSearchScoreWeightsValid() &&
        searchHasStarted_ &&
        isFiniteAndNonNegative(
            powerAvailableAtStartWatts_
        ) &&
        isFiniteAndNonNegative(
            remainingPowerWatts_
        ) &&
        remainingPowerWatts_ <=
            powerAvailableAtStartWatts_;
}


bool BestFirstSearch::isLoadValidForSearch(
    std::size_t loadIndex
) const
{
    if (loadIndex >= loads_.size() ||
        loads_[loadIndex] == nullptr) {
        return false;
    }

    const LoadPower loadPower =
        loads_[loadIndex]->getPower();

    return
        std::isfinite(loadPower.runningWatts) &&
        loadPower.runningWatts > 0.0F &&
        std::isfinite(loadPower.startupWatts) &&
        loadPower.startupWatts >=
            loadPower.runningWatts &&
        loads_[loadIndex]->getPriority() <=
            maximumAllowedPriority_;
}


bool BestFirstSearch::isLoadAlreadyAdded(
    const Load& load
) const
{
    return std::find(
        loads_.begin(),
        loads_.end(),
        &load
    ) != loads_.end();
}


float BestFirstSearch::calculateLoadRunningPowerRatio(
    std::size_t loadIndex
) const
{
    if (!isLoadValidForSearch(loadIndex)) {
        return 0.0F;
    }

    const float runningPowerWatts =
        loads_[loadIndex]->getPower().runningWatts;

    return runningPowerWatts /
           std::max(
               powerAvailableAtStartWatts_,
               1.0F
           );
}


float BestFirstSearch::calculateLoadStartupPowerRatio(
    std::size_t loadIndex
) const
{
    if (!isLoadValidForSearch(loadIndex)) {
        return 0.0F;
    }

    const LoadPower loadPower =
        loads_[loadIndex]->getPower();

    const float extraStartupPowerWatts =
        std::max(
            0.0F,
            loadPower.startupWatts -
                loadPower.runningWatts
        );

    return extraStartupPowerWatts /
           std::max(
               powerAvailableAtStartWatts_,
               1.0F
           );
}


float BestFirstSearch::calculateLoadPriorityRatio(
    std::size_t loadIndex
) const
{
    if (!isLoadValidForSearch(loadIndex) ||
        maximumAllowedPriority_ == 0U) {
        return 0.0F;
    }

    return limitValueToRange(
        static_cast<float>(
            loads_[loadIndex]->getPriority()
        ) /
        static_cast<float>(
            maximumAllowedPriority_
        ),
        0.0F,
        1.0F
    );
}


float BestFirstSearch::calculateLoadPowerScore(
    std::size_t loadIndex
) const
{
    if (loadIndex >=
            loadRunningPowerRatios_.size() ||
        loadIndex >=
            loadStartupPowerRatios_.size()) {
        return 0.0F;
    }

    return
        (runningPowerScoreWeight_ *
         loadRunningPowerRatios_[loadIndex]) +
        (startupPowerScoreWeight_ *
         loadStartupPowerRatios_[loadIndex]);
}


float BestFirstSearch::calculateLoadPriorityScore(
    std::size_t loadIndex
) const
{
    if (loadIndex >=
        loadPriorityRatios_.size()) {
        return 0.0F;
    }

    return priorityScoreWeight_ *
           (1.0F -
            loadPriorityRatios_[loadIndex]);
}


float BestFirstSearch::calculateLoadFinalSearchScore(
    std::size_t loadIndex
) const
{
    if (loadIndex >= loadPowerScores_.size() ||
        loadIndex >= loadPriorityScores_.size()) {
        return 0.0F;
    }

    return
        loadPowerScores_[loadIndex] +
        loadPriorityScores_[loadIndex];
}


void BestFirstSearch::calculateAndStoreLoadSearchScores(
    std::size_t loadIndex
)
{
    if (!isLoadValidForSearch(loadIndex)) {
        return;
    }

    loadRunningPowerRatios_[loadIndex] =
        calculateLoadRunningPowerRatio(
            loadIndex
        );

    loadStartupPowerRatios_[loadIndex] =
        calculateLoadStartupPowerRatio(
            loadIndex
        );

    loadPriorityRatios_[loadIndex] =
        calculateLoadPriorityRatio(
            loadIndex
        );

    loadPowerScores_[loadIndex] =
        calculateLoadPowerScore(
            loadIndex
        );

    loadPriorityScores_[loadIndex] =
        calculateLoadPriorityScore(
            loadIndex
        );

    loadFinalSearchScores_[loadIndex] =
        calculateLoadFinalSearchScore(
            loadIndex
        );
}


std::uint8_t BestFirstSearch::getLoadRemainingPowerCheckResult(
    std::size_t loadIndex
) const
{
    if (!isLoadValidForSearch(loadIndex)) {
        return LOAD_NEEDS_MORE_POWER_THAN_REMAINS;
    }

    const float runningPowerWatts =
        loads_[loadIndex]->getPower().runningWatts;

    if (runningPowerWatts >
        remainingPowerWatts_) {

        return LOAD_NEEDS_MORE_POWER_THAN_REMAINS;
    }

    return LOAD_CAN_FIT_REMAINING_POWER;
}


void BestFirstSearch::clearOpenSet()
{
    openSetLoadIndexes_.clear();
}


void BestFirstSearch::insertLoadIntoOpenSet(
    std::size_t loadIndex
)
{
    if (loadIndex >=
        loadFinalSearchScores_.size()) {
        return;
    }

    openSetLoadIndexes_.push_back(
        loadIndex
    );

    ESP_LOGD(
        TAG,
        "Load inserted into OPEN: loadIndex=%u openSize=%u",
        static_cast<unsigned int>(loadIndex),
        static_cast<unsigned int>(openSetLoadIndexes_.size())
    );

    moveLoadUpOpenSetUntilOrdered(
        openSetLoadIndexes_.size() - 1U
    );
}


std::size_t BestFirstSearch::extractBestLoadFromOpenSet()
{
    if (openSetLoadIndexes_.empty()) {
        return loads_.size();
    }

    const std::size_t bestLoadIndex =
        openSetLoadIndexes_.front();

    openSetLoadIndexes_.front() =
        openSetLoadIndexes_.back();

    openSetLoadIndexes_.pop_back();

    if (!openSetLoadIndexes_.empty()) {
        moveLoadDownOpenSetUntilOrdered(0U);
    }

    ESP_LOGD(
        TAG,
        "Best Load extracted from OPEN: loadIndex=%u openSize=%u",
        static_cast<unsigned int>(bestLoadIndex),
        static_cast<unsigned int>(openSetLoadIndexes_.size())
    );

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
                openSetLoadIndexes_[
                    openSetPosition
                ],
                openSetLoadIndexes_[
                    parentPosition
                ])) {
            break;
        }

        std::swap(
            openSetLoadIndexes_[
                parentPosition
            ],
            openSetLoadIndexes_[
                openSetPosition
            ]
        );

        openSetPosition =
            parentPosition;
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

        std::size_t bestPosition =
            openSetPosition;


        if (leftChildPosition <
                openSetLoadIndexes_.size() &&
            doesLeftLoadHaveBetterSearchOrder(
                openSetLoadIndexes_[
                    leftChildPosition
                ],
                openSetLoadIndexes_[
                    bestPosition
                ])) {

            bestPosition =
                leftChildPosition;
        }


        if (rightChildPosition <
                openSetLoadIndexes_.size() &&
            doesLeftLoadHaveBetterSearchOrder(
                openSetLoadIndexes_[
                    rightChildPosition
                ],
                openSetLoadIndexes_[
                    bestPosition
                ])) {

            bestPosition =
                rightChildPosition;
        }


        if (bestPosition ==
            openSetPosition) {
            break;
        }


        std::swap(
            openSetLoadIndexes_[
                openSetPosition
            ],
            openSetLoadIndexes_[
                bestPosition
            ]
        );

        openSetPosition =
            bestPosition;
    }
}


bool BestFirstSearch::doesLeftLoadHaveBetterSearchOrder(
    std::size_t leftLoadIndex,
    std::size_t rightLoadIndex
) const
{
    if (leftLoadIndex >=
            loadFinalSearchScores_.size() ||
        rightLoadIndex >=
            loadFinalSearchScores_.size()) {
        return false;
    }

    return
        loadFinalSearchScores_[
            leftLoadIndex
        ] <
        loadFinalSearchScores_[
            rightLoadIndex
        ];
}


void BestFirstSearch::markLoadSelectedToBeOn(
    std::size_t loadIndex
)
{
    if (!isLoadValidForSearch(loadIndex) ||
        loadIndex >=
            loadSelectedToBeOn_.size() ||
        loadIndex >=
            loadSelectionRejectionReasons_.size()) {
        return;
    }

    const float runningPowerWatts =
        loads_[loadIndex]->getPower().runningWatts;

    loadSelectedToBeOn_[loadIndex] = 1U;

    loadSelectionRejectionReasons_[loadIndex] =
        LOAD_CAN_FIT_REMAINING_POWER;

    remainingPowerWatts_ =
        std::max(
            0.0F,
            remainingPowerWatts_ -
                runningPowerWatts
        );

    const Load& load = *loads_[loadIndex];
    const Load::MacAddress& macAddress = load.getMacAddress();

    ESP_LOGI(
        TAG,
        "Load selected ON: name=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X pin=%u running=%.3fW remainingPower=%.3fW",
        load.getName().c_str(),
        macAddress[0],
        macAddress[1],
        macAddress[2],
        macAddress[3],
        macAddress[4],
        macAddress[5],
        static_cast<unsigned int>(load.getRelayPin()),
        runningPowerWatts,
        remainingPowerWatts_
    );
}


void BestFirstSearch::markLoadNotSelected(
    std::size_t loadIndex,
    std::uint8_t loadSelectionRejectionReason
)
{
    if (loadIndex >=
            loadSelectedToBeOn_.size() ||
        loadIndex >=
            loadSelectionRejectionReasons_.size()) {
        return;
    }

    loadSelectedToBeOn_[loadIndex] = 0U;

    loadSelectionRejectionReasons_[loadIndex] =
        loadSelectionRejectionReason;

    if (loadIndex < loads_.size() &&
        loads_[loadIndex] != nullptr) {

        const Load& load = *loads_[loadIndex];
        const Load::MacAddress& macAddress = load.getMacAddress();

        ESP_LOGW(
            TAG,
            "Load not selected: name=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X pin=%u reason=%u remainingPower=%.3fW",
            load.getName().c_str(),
            macAddress[0],
            macAddress[1],
            macAddress[2],
            macAddress[3],
            macAddress[4],
            macAddress[5],
            static_cast<unsigned int>(load.getRelayPin()),
            static_cast<unsigned int>(loadSelectionRejectionReason),
            remainingPowerWatts_
        );
    }
}


float BestFirstSearch::limitValueToRange(
    float value,
    float minimumAllowedValue,
    float maximumAllowedValue
)
{
    return std::max(
        minimumAllowedValue,
        std::min(
            value,
            maximumAllowedValue
        )
    );
}


bool BestFirstSearch::isFiniteAndNonNegative(
    float value
)
{
    return
        std::isfinite(value) &&
        value >= 0.0F;
}


} // namespace kilowatts