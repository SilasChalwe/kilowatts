/**
 * @file BestFirstSearch.h
 * @brief Declares the Best-First Search used to select Load objects.
 *
 * BestFirstSearch receives Load objects and the amount of power available
 * at the start of the search.
 *
 * It does not know how load power was measured or obtained.
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

#ifndef KILOWATTS_BEST_FIRST_SEARCH_H
#define KILOWATTS_BEST_FIRST_SEARCH_H

#include "Load.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


class BestFirstSearch {

public:

    /**
     * Result returned for one Load after the remaining-power check.
     */
    static constexpr std::uint8_t LOAD_CAN_FIT_REMAINING_POWER = 0U;
    static constexpr std::uint8_t LOAD_NEEDS_MORE_POWER_THAN_REMAINS = 1U;


    /**
     * Creates an empty Best-First Search.
     *
     * There is no artificial maximum number of Loads in this class.
     */
    BestFirstSearch();


    /**
     * Sets the weights used by the Best-First score.
     *
     * maximumAllowedPriority is the highest priority value expected
     * from a Load.
     */
    bool setSearchScoreWeights(
        float runningPowerScoreWeight,
        float startupPowerScoreWeight,
        float priorityScoreWeight,
        std::uint16_t maximumAllowedPriority
    );


    /**
     * Starts a new search using the power supplied to BestFirstSearch.
     *
     * powerAvailableAtStartWatts does not change during this search.
     * remainingPowerWatts_ starts with the same value and decreases
     * when Loads are selected.
     */
    bool startSearch(float powerAvailableAtStartWatts);


    /**
     * Adds one Load object to the current search.
     *
     * Call this once for every Load that must participate in the
     * same Best-First Search.
     */
    bool addLoad(const Load& load);


    /**
     * Runs Best-First Search over all Loads that were added.
     */
    bool run();


    /**
     * Clears the Loads and results from the current search.
     *
     * The search score weights remain configured.
     */
    void resetSearch();


    /**
     * Returns the number of Load objects currently added to the search.
     */
    std::size_t getNumberOfLoadsAdded() const;


    /**
     * Returns the unchanged power value supplied to startSearch().
     */
    float getPowerAvailableAtStartWatts() const;


    /**
     * Returns the power still left after selected Loads have used
     * part of the power supplied to the search.
     */
    float getRemainingPowerWatts() const;


    /**
     * Returns the original Load object at loadIndex.
     *
     * Returns nullptr when loadIndex is invalid.
     */
    const Load* getLoad(std::size_t loadIndex) const;


    /**
     * Returns true when BestFirstSearch selected the Load to be ON.
     */
    bool isLoadSelectedToBeOn(std::size_t loadIndex) const;


    /**
     * Returns the reason why a Load was not selected.
     */
    std::uint8_t getLoadSelectionRejectionReason(
        std::size_t loadIndex
    ) const;


private:

    /**
     * Checks whether the configured score weights can be used.
     */
    bool areSearchScoreWeightsValid() const;


    /**
     * Checks whether the current search state is valid.
     */
    bool isCurrentSearchStateValid() const;


    /**
     * Checks whether the Load at loadIndex contains values that
     * BestFirstSearch can use.
     */
    bool isLoadValidForSearch(std::size_t loadIndex) const;


    /**
     * Checks whether the same Load object has already been added.
     */
    bool isLoadAlreadyAdded(const Load& load) const;


    /**
     * Converts the Load's running power into the ratio used
     * by the search score.
     */
    float calculateLoadRunningPowerRatio(
        std::size_t loadIndex
    ) const;


    /**
     * Converts the Load's startup power into the ratio used
     * by the search score.
     */
    float calculateLoadStartupPowerRatio(
        std::size_t loadIndex
    ) const;


    /**
     * Converts the Load's priority into the ratio used
     * by the search score.
     */
    float calculateLoadPriorityRatio(
        std::size_t loadIndex
    ) const;


    /**
     * Calculates the score produced by the Load's power requirements.
     */
    float calculateLoadPowerScore(
        std::size_t loadIndex
    ) const;


    /**
     * Calculates the score produced by the Load's priority.
     */
    float calculateLoadPriorityScore(
        std::size_t loadIndex
    ) const;


    /**
     * Calculates the final score used to order the Load in OPEN.
     */
    float calculateLoadFinalSearchScore(
        std::size_t loadIndex
    ) const;


    /**
     * Calculates and stores all ratios and scores for one Load.
     */
    void calculateAndStoreLoadSearchScores(
        std::size_t loadIndex
    );


    /**
     * Checks whether the Load's running power can fit inside
     * remainingPowerWatts_.
     */
    std::uint8_t getLoadRemainingPowerCheckResult(
        std::size_t loadIndex
    ) const;


    /**
     * OPEN binary min-heap operations.
     */
    void clearOpenSet();

    void insertLoadIntoOpenSet(
        std::size_t loadIndex
    );

    std::size_t extractBestLoadFromOpenSet();

    void moveLoadUpOpenSetUntilOrdered(
        std::size_t openSetPosition
    );

    void moveLoadDownOpenSetUntilOrdered(
        std::size_t openSetPosition
    );

    bool doesLeftLoadHaveBetterSearchOrder(
        std::size_t leftLoadIndex,
        std::size_t rightLoadIndex
    ) const;


    /**
     * Stores that one Load was selected and updates
     * remainingPowerWatts_.
     */
    void markLoadSelectedToBeOn(
        std::size_t loadIndex
    );


    /**
     * Stores that one Load was not selected.
     */
    void markLoadNotSelected(
        std::size_t loadIndex,
        std::uint8_t loadSelectionRejectionReason
    );


    /**
     * Keeps a value inside an allowed range.
     */
    static float limitValueToRange(
        float value,
        float minimumAllowedValue,
        float maximumAllowedValue
    );


    /**
     * Returns true when a number is finite and zero or greater.
     */
    static bool isFiniteAndNonNegative(float value);


    /**
     * Internal search state.
     */
    bool searchScoreWeightsConfigured_;
    bool searchHasStarted_;
    bool searchHasCompleted_;


    /**
     * Best-First score configuration.
     */
    float runningPowerScoreWeight_;
    float startupPowerScoreWeight_;
    float priorityScoreWeight_;
    std::uint16_t maximumAllowedPriority_;


    /**
     * Power supplied to BestFirstSearch when startSearch() is called.
     *
     * This value does not change during one search.
     */
    float powerAvailableAtStartWatts_;


    /**
     * Power still left while Loads are being selected.
     */
    float remainingPowerWatts_;


    /**
     * Load objects participating in this search.
     *
     * No separate Load ID or extra Load reference is created.
     */
    std::vector<const Load*> loads_;


    /**
     * Ratios calculated internally for each Load.
     */
    std::vector<float> loadRunningPowerRatios_;
    std::vector<float> loadStartupPowerRatios_;
    std::vector<float> loadPriorityRatios_;


    /**
     * Search scores calculated internally for each Load.
     */
    std::vector<float> loadPowerScores_;
    std::vector<float> loadPriorityScores_;
    std::vector<float> loadFinalSearchScores_;


    /**
     * Final decision and rejection reason for each Load.
     */
    std::vector<std::uint8_t> loadSelectedToBeOn_;
    std::vector<std::uint8_t> loadSelectionRejectionReasons_;


    /**
     * OPEN binary min-heap.
     *
     * Each value is an index into loads_.
     */
    std::vector<std::size_t> openSetLoadIndexes_;
};


} // namespace kilowatts

#endif // KILOWATTS_BEST_FIRST_SEARCH_H