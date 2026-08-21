#include "simulation_support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

using best_first_simulation::LoadSpec;
using best_first_simulation::RunResult;
using best_first_simulation::run;

namespace {

struct BatteryStep {
    float storedWh;
    float dischargedWh;
    float unmetWh;
};

struct CycleTotals {
    float finalSoc;
    float requiredWh;
    float dischargedWh;
    float unmetWh;
};

struct CycleComparison {
    CycleTotals managed;
    CycleTotals unmanaged;
};

std::vector<LoadSpec> cycleLoads() {
    const auto nodeA = best_first_simulation::nodeMac(0U);
    const auto nodeB = best_first_simulation::nodeMac(1U);
    const auto nodeC = best_first_simulation::nodeMac(2U);
    const auto nodeD = best_first_simulation::nodeMac(3U);
    return {
        {"Fridge", nodeA, 16U, 60.0F, 150.0F, 1U, kilowatts::LoadMode::Fixed::ON, {false, 0U, 0U}},
        {"Lights A", nodeA, 17U, 18.0F, 18.0F, 4U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"TV A", nodeA, 18U, 90.0F, 100.0F, 6U, kilowatts::LoadMode::Auto::ON, {true, 18U, 0U}},
        {"Kettle A", nodeA, 19U, 800.0F, 900.0F, 9U, kilowatts::LoadMode::Auto::OFF, {true, 18U, 0U}},
        {"Lights B", nodeB, 20U, 12.0F, 12.0F, 4U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"AC B", nodeB, 21U, 350.0F, 450.0F, 8U, kilowatts::LoadMode::Auto::ON, {true, 8U, 0U}},
        {"Garage B", nodeB, 22U, 20.0F, 20.0F, 5U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Heater B", nodeB, 23U, 500.0F, 550.0F, 10U, kilowatts::LoadMode::Auto::OFF, {true, 19U, 0U}},
        {"Pump C", nodeC, 24U, 90.0F, 200.0F, 1U, kilowatts::LoadMode::Fixed::ON, {false, 0U, 0U}},
        {"Washer C", nodeC, 25U, 400.0F, 600.0F, 7U, kilowatts::LoadMode::Auto::OFF, {true, 10U, 0U}},
        {"Pool C", nodeC, 26U, 250.0F, 300.0F, 6U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Lights C", nodeC, 27U, 15.0F, 15.0F, 3U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Router D", nodeD, 28U, 10.0F, 10.0F, 2U, kilowatts::LoadMode::Fixed::ON, {false, 0U, 0U}},
        {"Fans D", nodeD, 29U, 30.0F, 45.0F, 3U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Charger D", nodeD, 30U, 250.0F, 300.0F, 7U, kilowatts::LoadMode::Auto::OFF, {true, 12U, 0U}},
        {"EV D", nodeD, 31U, 1500.0F, 1500.0F, 10U, kilowatts::LoadMode::Auto::OFF, {true, 20U, 0U}}
    };
}

BatteryStep applyBatteryStep(float storedWh, float capacityWh, float demandWatts) {
    const float minimumWh = capacityWh * 0.20F;
    const float netWh = demandWatts;
    float dischargedWh = 0.0F;
    float unmetWh = 0.0F;
    if (netWh >= 0.0F) {
        dischargedWh = std::min(netWh, std::max(0.0F, storedWh - minimumWh));
        unmetWh = netWh - dischargedWh;
        storedWh -= dischargedWh;
    }
    return {storedWh, dischargedWh, unmetWh};
}

CycleComparison runCycle(const std::vector<LoadSpec>& specs, float capacityAh, float startingSoc,
                         std::ofstream& detailCsv, unsigned scenarioId)
{
    const float capacityWh = capacityAh * 12.0F;
    float managedStoredWh = capacityWh * startingSoc / 100.0F;
    float unmanagedStoredWh = managedStoredWh;
    CycleTotals managed{startingSoc, 0.0F, 0.0F, 0.0F};
    CycleTotals unmanaged{startingSoc, 0.0F, 0.0F, 0.0F};

    for (unsigned hour = 0U; hour < 24U; ++hour) {
        const float managedSoc = 100.0F * managedStoredWh / capacityWh;
        const RunResult result = run(specs, capacityAh, managedSoc, hour);
        const BatteryStep managedStep = applyBatteryStep(
            managedStoredWh, capacityWh, result.committedWatts);
        const BatteryStep unmanagedStep = applyBatteryStep(
            unmanagedStoredWh, capacityWh, result.naiveWatts);
        managedStoredWh = managedStep.storedWh;
        unmanagedStoredWh = unmanagedStep.storedWh;
        managed.dischargedWh += managedStep.dischargedWh;
        managed.unmetWh += managedStep.unmetWh;
        managed.requiredWh += result.committedWatts;
        unmanaged.dischargedWh += unmanagedStep.dischargedWh;
        unmanaged.unmetWh += unmanagedStep.unmetWh;
        unmanaged.requiredWh += result.naiveWatts;

        detailCsv << scenarioId << ',' << hour << ',' << startingSoc << ',' << capacityAh << ','
                  << managedSoc << ',' << result.naiveWatts << ','
                  << result.committedWatts << ',' << managedStoredWh << ',' << unmanagedStoredWh << ','
                  << managedStep.dischargedWh << ',' << unmanagedStep.dischargedWh << ','
                  << managedStep.unmetWh << ',' << unmanagedStep.unmetWh << ','
                  << (result.valid ? "PASS" : "FAIL") << '\n';
        std::printf("  hour %02u: managed load %5.1f W, battery discharge %5.1f Wh, SoC %5.1f%%\n",
                    hour, result.committedWatts, managedStep.dischargedWh,
                    100.0F * managedStoredWh / capacityWh);
    }

    managed.finalSoc = 100.0F * managedStoredWh / capacityWh;
    unmanaged.finalSoc = 100.0F * unmanagedStoredWh / capacityWh;
    return CycleComparison{managed, unmanaged};
}

int mainImpl() {
    const std::vector<LoadSpec> specs = cycleLoads();
    const float capacities[] = {20.0F, 100.0F, 1000.0F};
    const float startingSocs[] = {25.0F, 50.0F, 75.0F, 100.0F};
    std::ofstream detailCsv("test/report/csv/battery_cycle.csv");
    std::ofstream summaryCsv("test/report/csv/battery_savings_summary.csv");
    if (!detailCsv || !summaryCsv) return 2;

    detailCsv << "scenario_id,hour,starting_soc_percent,capacity_ah,managed_soc_before_percent,"
                 "unmanaged_demand_watts,managed_demand_watts,managed_stored_wh,unmanaged_stored_wh,"
                 "managed_discharged_wh,unmanaged_discharged_wh,"
                 "managed_unmet_wh,unmanaged_unmet_wh,result\n";
    summaryCsv << "scenario_id,capacity_ah,starting_soc_percent,managed_final_soc_percent,"
                  "unmanaged_final_soc_percent,managed_required_wh,unmanaged_required_wh,"
                  "battery_energy_required_saved_wh,managed_discharged_wh,unmanaged_discharged_wh,"
                  "actual_discharge_saved_wh,stored_energy_saved_wh,battery_soc_saved_percent,managed_stored_wh,"
                  "unmanaged_stored_wh,managed_unmet_wh,unmanaged_unmet_wh,result\n";

    bool passed = true;
    unsigned scenarioId = 0U;
    for (float capacityAh : capacities) {
        for (float startingSoc : startingSocs) {
            ++scenarioId;
            const CycleComparison comparison = runCycle(specs, capacityAh, startingSoc, detailCsv, scenarioId);
            const CycleTotals& managed = comparison.managed;
            const CycleTotals& unmanaged = comparison.unmanaged;
            const float capacityWh = capacityAh * 12.0F;
            const float managedStoredWh = managed.finalSoc * capacityWh / 100.0F;
            const float unmanagedStoredWh = unmanaged.finalSoc * capacityWh / 100.0F;
            const float unmanagedFinalSoc = unmanaged.finalSoc;
            const float requiredSavedWh = unmanaged.requiredWh - managed.requiredWh;
            const float dischargeSavedWh = unmanaged.dischargedWh - managed.dischargedWh;
            const float storedEnergySavedWh = managedStoredWh - unmanagedStoredWh;
            const float socSaved = managed.finalSoc - unmanagedFinalSoc;
            const bool rowPassed = managed.finalSoc >= 20.0F && managed.finalSoc <= 100.0F &&
                                   unmanagedFinalSoc >= 20.0F && unmanagedFinalSoc <= 100.0F &&
                                   std::isfinite(requiredSavedWh) && std::isfinite(storedEnergySavedWh);
            passed = rowPassed && passed;
            summaryCsv << scenarioId << ',' << capacityAh << ',' << startingSoc << ',' << managed.finalSoc << ','
                       << unmanaged.finalSoc << ',' << managed.requiredWh << ',' << unmanaged.requiredWh << ','
                       << requiredSavedWh << ',' << managed.dischargedWh << ',' << unmanaged.dischargedWh << ','
                       << dischargeSavedWh << ',' << storedEnergySavedWh << ',' << socSaved << ','
                       << managedStoredWh << ',' << unmanagedStoredWh << ',' << managed.unmetWh << ','
                       << unmanaged.unmetWh << ',' << (rowPassed ? "PASS" : "FAIL") << '\n';
            std::printf("cycle %02u: %g Ah start %g%% -> managed %0.1f%%, unmanaged %0.1f%%, required battery saved %0.1f Wh, stored difference %0.1f Wh: %s\n",
                        scenarioId, capacityAh, startingSoc, managed.finalSoc, unmanagedFinalSoc,
                        requiredSavedWh, storedEnergySavedWh, rowPassed ? "PASS" : "FAIL");
        }
    }
    return passed ? 0 : 1;
}

} // namespace

int main() {
    return mainImpl();
}
