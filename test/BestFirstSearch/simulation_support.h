#ifndef KILOWATTS_BEST_FIRST_SIMULATION_SUPPORT_H
#define KILOWATTS_BEST_FIRST_SIMULATION_SUPPORT_H

#include "BestFirstSearch.h"
#include "Load.h"
#include "LoadFilter.h"
#include "PowerManager.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace best_first_simulation {

using kilowatts::BestFirstSearch;
using kilowatts::AutoSchedule;
using kilowatts::Load;
using kilowatts::LoadFilter;
using kilowatts::LoadMode;
using kilowatts::LoadPower;
using kilowatts::SafePowerLimitCalculator;

struct LoadSpec {
    const char* name;
    Load::MacAddress node;
    std::uint8_t pin;
    float runningWatts;
    float startupWatts;
    std::uint16_t priority;
    LoadMode::Value mode;
    AutoSchedule schedule;
};

struct RunResult {
    bool valid;
    float availableWatts;
    float committedWatts;
    float naiveWatts;
    std::size_t selectedLoads;
    double elapsedMicroseconds;
};

inline BestFirstSearch::Weights equalWeights() {
    return BestFirstSearch::Weights{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 10U};
}

inline BestFirstSearch::ElectricalPlanningState planningState(
    float soc, float availableWatts, float maximumBatteryWatts,
    float fixedWatts, float autoWatts)
{
    return BestFirstSearch::ElectricalPlanningState{
        soc, 20.0F, 40.0F, 12.0F, maximumBatteryWatts, 30.0F,
        availableWatts, autoWatts, fixedWatts};
}

inline float schedulePenaltyAtHour(const AutoSchedule& schedule, unsigned currentHour) {
    if (!schedule.enabled) return 0.0F;
    return (currentHour > schedule.hour ||
            (currentHour == schedule.hour && schedule.minute == 0U)) ? 0.0F : 1.0F;
}

inline RunResult run(const std::vector<LoadSpec>& specs, float capacityAh, float soc,
                     unsigned currentHour = 12U) {
    SafePowerLimitCalculator::Inputs inputs{};
    inputs.stateOfChargePercent = soc;
    inputs.minimumStateOfChargePercent = 20.0F;
    inputs.nominalBatteryVoltageVolts = 12.0F;
    inputs.batteryCapacityAmpHours = capacityAh;
    inputs.targetRuntimeHours = 4.0F;
    inputs.batteryBusVoltageVolts = 12.0F;
    inputs.maximumBatteryDischargeCurrentAmps = 40.0F;
    inputs.maximumMainCurrentAmps = 30.0F;
    inputs.safetyFactor = 0.9F;

    SafePowerLimitCalculator limit;
    if (!limit.calculate(inputs)) return {false, 0.0F, 0.0F, 0.0F, 0U, 0.0};

    std::vector<Load> loads;
    loads.reserve(specs.size());
    for (std::size_t i = 0U; i < specs.size(); ++i) {
        loads.emplace_back(Load::Id{specs[i].node, specs[i].pin}, specs[i].name,
                           LoadPower{specs[i].runningWatts, specs[i].startupWatts},
                           specs[i].priority, specs[i].mode);
        if (specs[i].mode == LoadMode::Auto::ON || specs[i].mode == LoadMode::Auto::OFF) {
            loads.back().setSchedule(specs[i].schedule);
        }
    }

    LoadFilter filter;
    float fixedWatts = 0.0F;
    float naiveWatts = 0.0F;
    for (std::size_t i = 0U; i < loads.size(); ++i) {
        filter.addLoad(loads[i]);
        if (specs[i].mode == LoadMode::Fixed::ON) fixedWatts += specs[i].runningWatts;
        if (specs[i].mode == LoadMode::Fixed::ON || specs[i].mode == LoadMode::Auto::ON) {
            naiveWatts += specs[i].runningWatts;
        }
    }

    const float availableWatts = limit.getAvailablePowerWatts();
    const float autoWatts = std::max(0.0F, availableWatts - fixedWatts);
    BestFirstSearch search;
    if (!search.setSearchScoreWeights(equalWeights()) ||
        !search.startSearch(planningState(soc, availableWatts,
                                          limit.getMaximumBatteryPowerWatts(),
                                          fixedWatts, autoWatts))) {
        return {false, availableWatts, fixedWatts, naiveWatts, 0U, 0.0};
    }

    std::vector<std::size_t> candidateIndices;
    candidateIndices.reserve(filter.getNumberOfAutoCandidateLoads());
    for (std::size_t i = 0U; i < filter.getNumberOfAutoCandidateLoads(); ++i) {
        const Load* candidate = filter.getAutoCandidateLoad(i);
        for (std::size_t j = 0U; j < loads.size(); ++j) {
            if (&loads[j] == candidate &&
                search.addLoad(*candidate, schedulePenaltyAtHour(specs[j].schedule, currentHour))) {
                candidateIndices.push_back(j);
                break;
            }
        }
    }

    const auto start = std::chrono::steady_clock::now();
    const bool completed = search.run();
    const auto finish = std::chrono::steady_clock::now();
    if (!completed) return {false, availableWatts, fixedWatts, naiveWatts, 0U, 0.0};

    float committedWatts = fixedWatts;
    std::size_t selectedLoads = 0U;
    for (std::size_t i = 0U; i < candidateIndices.size(); ++i) {
        if (search.isLoadSelectedToBeOn(i)) {
            committedWatts += specs[candidateIndices[i]].runningWatts;
            ++selectedLoads;
        }
    }
    const std::chrono::duration<double, std::micro> elapsed = finish - start;
    return {true, availableWatts, committedWatts, naiveWatts, selectedLoads, elapsed.count()};
}

inline Load::MacAddress nodeMac(std::size_t nodeIndex) {
    return Load::MacAddress{0x02U, 0x00U, 0x00U, 0x00U,
                            static_cast<std::uint8_t>(nodeIndex), 0x01U};
}

inline std::vector<LoadSpec> makeSyntheticLoads(std::size_t count) {
    std::vector<LoadSpec> specs;
    specs.reserve(count);
    for (std::size_t i = 0U; i < count; ++i) {
        const std::size_t node = i / 4U;
        const float running = 1.0F + static_cast<float>((i * 37U) % 10U) * 0.25F;
        const float startup = running + 0.5F + static_cast<float>((i * 17U) % 5U) * 0.25F;
        const bool fixed = (i % 16U) == 0U;
        const bool autoOn = (i % 3U) != 0U;
        specs.push_back(LoadSpec{"SyntheticLoad", nodeMac(node), static_cast<std::uint8_t>(16U + (i % 4U)),
                                 running, startup,
                                 static_cast<std::uint16_t>(1U + ((i * 31U) % 10U)),
                                 fixed ? LoadMode::Fixed::ON : (autoOn ? LoadMode::Auto::ON : LoadMode::Auto::OFF),
                                 AutoSchedule{(i % 11U) == 0U, static_cast<std::uint8_t>((i * 3U) % 24U), 0U}});
    }
    return specs;
}

inline bool writeCsvHeader(const std::string& path, const std::string& header) {
    std::ofstream output(path);
    if (!output) return false;
    output << header << '\n';
    return true;
}

} // namespace best_first_simulation

#endif
