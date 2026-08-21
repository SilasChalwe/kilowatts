#include "simulation_support.h"

#include <cmath>
#include <cstdio>
#include <limits>

using best_first_simulation::RunResult;
using best_first_simulation::makeSyntheticLoads;
using best_first_simulation::run;

namespace {

double log2Value(std::size_t value) {
    return std::log(static_cast<double>(value)) / std::log(2.0);
}

int mainImpl() {
    const std::size_t loadCounts[] = {32U, 64U, 128U};
    const float capacities[] = {100.0F, 1000.0F};
    const unsigned repetitions[] = {100U, 1000U};

    std::ofstream csv("test/report/csv/scalability_summary.csv");
    if (!csv) return 2;
    csv << "loads,capacity_ah,soc_percent,repetitions,valid_runs,average_run_microseconds,min_run_microseconds,max_run_microseconds,normalized_nlogn,result\n";

    bool passed = true;
    double previousNormalized = 0.0;
    bool havePreviousNormalized = false;
    for (std::size_t loadCount : loadCounts) {
        const std::vector<best_first_simulation::LoadSpec> specs = makeSyntheticLoads(loadCount);
        for (float capacity : capacities) {
            for (unsigned repetitionCount : repetitions) {
                double totalMicroseconds = 0.0;
                double minimumMicroseconds = std::numeric_limits<double>::max();
                double maximumMicroseconds = 0.0;
                unsigned validRuns = 0U;
                for (unsigned repetition = 0U; repetition < repetitionCount; ++repetition) {
                    const RunResult result = run(specs, capacity, 75.0F);
                    if (result.valid && result.committedWatts <= result.availableWatts + 0.01F) {
                        ++validRuns;
                    }
                    totalMicroseconds += result.elapsedMicroseconds;
                    minimumMicroseconds = std::min(minimumMicroseconds, result.elapsedMicroseconds);
                    maximumMicroseconds = std::max(maximumMicroseconds, result.elapsedMicroseconds);
                }
                const double average = totalMicroseconds / static_cast<double>(repetitionCount);
                const double normalized = average /
                    (static_cast<double>(loadCount) * log2Value(loadCount));
                const bool groupPassed = validRuns == repetitionCount && std::isfinite(normalized);
                passed = groupPassed && passed;
                csv << loadCount << ',' << capacity << ",75," << repetitionCount << ',' << validRuns << ','
                    << average << ',' << minimumMicroseconds << ',' << maximumMicroseconds << ','
                    << normalized << ',' << (groupPassed ? "PASS" : "FAIL") << '\n';
                std::printf("scale: %3zu loads, %4.0f Ah, %4u runs, avg %8.3f us, nlogn %0.6f: %s\n",
                            loadCount, capacity, repetitionCount, average, normalized,
                            groupPassed ? "PASS" : "FAIL");
                if (repetitionCount == 1000U && capacity == 1000.0F) {
                    if (havePreviousNormalized && normalized > previousNormalized * 3.0) {
                        passed = false;
                    }
                    previousNormalized = normalized;
                    havePreviousNormalized = true;
                }
            }
        }
    }
    return passed ? 0 : 1;
}

} // namespace

int main() {
    return mainImpl();
}
