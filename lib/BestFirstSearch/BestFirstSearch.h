#ifndef KILOWATTS_BEST_FIRST_SEARCH_H
#define KILOWATTS_BEST_FIRST_SEARCH_H

#include "Load.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {

class BestFirstSearch {
public:
    static constexpr std::uint8_t NONE = 0U;
    static constexpr std::uint8_t LOW_BATTERY = 1U;
    static constexpr std::uint8_t POWER_LIMIT_EXCEEDED = 2U;
    static constexpr std::uint8_t BATTERY_CURRENT_LIMIT = 3U;
    static constexpr std::uint8_t MAIN_LIMIT_EXCEEDED = 4U;

    struct Weights {
        float runningPowerWeight;
        float startupPowerWeight;
        float batteryStressWeight;
        float priorityWeight;
        float scheduleWeight;
        std::uint16_t maximumAllowedPriority;
    };

    struct ElectricalPlanningState {
        float stateOfChargePercent;
        float minimumStateOfChargePercent;
        float warningStateOfChargePercent;
        float batteryBusVoltageVolts;
        float maximumBatteryPowerWatts;
        float maximumMainCurrentAmps;
        float powerBeforeFixedLoadsWatts;
        float powerPassedToBestFirstWatts;
        float fixedOnLoadPowerWatts;
    };

    struct FeasibilityInputs {
        float stateOfChargePercent;
        float minimumStateOfChargePercent;
        float candidateRunningPowerWatts;
        float candidatePeakPowerWatts;
        float remainingPowerWatts;
        float plannedOnPowerWatts;
        float maximumBatteryPowerWatts;
        float batteryBusVoltageVolts;
        float maximumMainCurrentAmps;
    };

    BestFirstSearch();

    bool setSearchScoreWeights(const Weights& weights);
    bool startSearch(const ElectricalPlanningState& planningState);
    bool addLoad(const Load& load, float scheduleFuturePenalty);
    bool run();
    void resetSearch();

    std::size_t getNumberOfLoadsAdded() const;
    const Load* getLoad(std::size_t loadIndex) const;
    bool isLoadSelectedToBeOn(std::size_t loadIndex) const;
    std::uint8_t getLoadSelectionRejectionReason(std::size_t loadIndex) const;

    std::size_t getNumberOfAdmittedLoads() const;
    const Load* getAdmittedLoad(std::size_t admissionPosition) const;

    static std::uint8_t checkFeasibility(const FeasibilityInputs& inputs);

    float getPowerBeforeFixedLoadsWatts() const;
    float getPowerPassedToBestFirstWatts() const;
    float getFinalRemainingPowerWatts() const;
    float getSelectedAutoLoadPowerWatts() const;
    float getPlannedOnPowerWatts() const;
    float getBatteryStressTerm() const;

    float getLoadRunningPowerRatio(std::size_t loadIndex) const;
    float getLoadStartupPowerRatio(std::size_t loadIndex) const;
    float getLoadPriorityRatio(std::size_t loadIndex) const;
    float getLoadPhysicalCost(std::size_t loadIndex) const;
    float getLoadHeuristicCost(std::size_t loadIndex) const;
    float getLoadFinalScore(std::size_t loadIndex) const;

private:
    bool areWeightsValid(const Weights& weights) const;
    bool isPlanningStateValid(const ElectricalPlanningState& state) const;
    bool isLoadValid(std::size_t index) const;
    bool isLoadAlreadyAdded(const Load& load) const;

    float calculateRunningPowerRatio(std::size_t index) const;
    float calculateStartupPowerRatio(std::size_t index) const;
    float calculatePriorityRatio(std::size_t index) const;
    float calculatePhysicalCost(std::size_t index) const;
    float calculateHeuristicCost(std::size_t index) const;
    float calculateFinalScore(std::size_t index) const;
    void calculateAndStoreScores(std::size_t index);
    std::uint8_t checkLoadFeasibility(std::size_t index) const;

    void insertIntoOpenSet(std::size_t index);
    std::size_t extractBestFromOpenSet();
    void moveUp(std::size_t position);
    void moveDown(std::size_t position);
    bool comesBefore(std::size_t leftIndex, std::size_t rightIndex) const;

    void selectLoad(std::size_t index);

    static bool isFiniteNonNegative(float value);
    static bool isFinitePercentage(float value);
    static float clamp(float value, float minimum, float maximum);

    bool weightsConfigured_;
    bool searchStarted_;
    bool searchCompleted_;

    Weights weights_;
    ElectricalPlanningState planningState_;

    float finalRemainingPowerWatts_;
    float selectedAutoLoadPowerWatts_;
    float plannedOnPowerWatts_;
    float batteryStressTerm_;

    std::vector<const Load*> loads_;
    std::vector<float> schedulePenalties_;
    std::vector<float> runningPowerRatios_;
    std::vector<float> startupPowerRatios_;
    std::vector<float> priorityRatios_;
    std::vector<float> physicalCosts_;
    std::vector<float> heuristicCosts_;
    std::vector<float> finalScores_;
    std::vector<std::uint8_t> selected_;
    std::vector<std::uint8_t> rejectionReasons_;
    std::vector<std::size_t> openSet_;
    std::vector<std::size_t> admittedOrder_;
};

} // namespace kilowatts

#endif // KILOWATTS_BEST_FIRST_SEARCH_H
