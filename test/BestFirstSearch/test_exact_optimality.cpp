#include "BestFirstSearch.h"
#include "Load.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

using kilowatts::BestFirstSearch;
using kilowatts::Load;
using kilowatts::LoadMode;
using kilowatts::LoadPower;

namespace {

struct ExactResult {
    float watts;
    unsigned mask;
};

ExactResult exactBest(const std::vector<Load>& loads, float budget) {
    ExactResult best{0.0F, 0U};
    for (unsigned mask = 0U; mask < (1U << loads.size()); ++mask) {
        float watts = 0.0F;
        bool feasible = true;
        for (std::size_t i = 0U; i < loads.size(); ++i) {
            if ((mask & (1U << i)) == 0U) continue;
            watts += loads[i].getPower().runningWatts;
            if (watts > budget) {
                feasible = false;
                break;
            }
        }
        if (feasible && watts > best.watts) best = ExactResult{watts, mask};
    }
    return best;
}

} // namespace

int main() {
    const Load::MacAddress node = {0x02U, 0x00U, 0x00U, 0x00U, 0x10U, 0x01U};
    const float budget = 20.0F;
    const float values[] = {12.0F, 9.0F, 7.0F, 4.0F, 25.0F, 25.0F, 25.0F, 25.0F, 25.0F, 25.0F};
    std::vector<Load> loads;
    for (std::size_t i = 0U; i < 10U; ++i) {
        loads.emplace_back(Load::Id{node, static_cast<std::uint8_t>(16U + i)},
                           "ExactLoad", LoadPower{values[i], values[i]},
                           static_cast<std::uint16_t>(i + 1U), LoadMode::Auto::ON);
    }

    BestFirstSearch search;
    const BestFirstSearch::Weights weights{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 10U};
    bool passed = search.setSearchScoreWeights(weights) &&
                  search.startSearch(BestFirstSearch::ElectricalPlanningState{
                      80.0F, 20.0F, 40.0F, 12.0F, 480.0F, 30.0F,
                      budget, budget, 0.0F});
    for (const Load& load : loads) passed = search.addLoad(load, 0.0F) && passed;
    passed = search.run() && passed;

    float greedyWatts = 0.0F;
    unsigned greedyMask = 0U;
    for (std::size_t i = 0U; i < loads.size(); ++i) {
        if (search.isLoadSelectedToBeOn(i)) {
            greedyWatts += loads[i].getPower().runningWatts;
            greedyMask |= (1U << i);
        }
    }
    const ExactResult exact = exactBest(loads, budget);
    const bool result = passed && greedyWatts == exact.watts && greedyMask == exact.mask;

    std::ofstream csv("test/report/csv/exact_optimality.csv");
    if (!csv) return 2;
    csv << "candidate_loads,budget_watts,greedy_watts,exact_watts,subset_match,result\n";
    csv << loads.size() << ',' << budget << ',' << greedyWatts << ',' << exact.watts << ','
        << (greedyMask == exact.mask ? "true" : "false") << ',' << (result ? "PASS" : "FAIL") << '\n';

    std::printf("EXACT OPTIMALITY: %s\n", result ? "PASS" : "FAIL");
    std::printf("Loads: %zu, greedy: %.1f W, exact: %.1f W\n", loads.size(), greedyWatts, exact.watts);
    return result ? 0 : 1;
}
