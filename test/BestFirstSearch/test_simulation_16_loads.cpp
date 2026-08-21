#include "simulation_support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using best_first_simulation::LoadSpec;
using best_first_simulation::RunResult;
using best_first_simulation::run;

namespace {

std::vector<LoadSpec> realLoads() {
    const auto nodeA = best_first_simulation::nodeMac(0U);
    const auto nodeB = best_first_simulation::nodeMac(1U);
    const auto nodeC = best_first_simulation::nodeMac(2U);
    const auto nodeD = best_first_simulation::nodeMac(3U);
    return {
        {"Node A Fridge", nodeA, 16U, 60.0F, 150.0F, 1U, kilowatts::LoadMode::Fixed::ON, {false, 0U, 0U}},
        {"Node A Lights", nodeA, 17U, 18.0F, 18.0F, 4U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Node A TV", nodeA, 18U, 90.0F, 100.0F, 6U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Node A Kettle", nodeA, 19U, 800.0F, 900.0F, 9U, kilowatts::LoadMode::Auto::OFF, {true, 18U, 0U}},
        {"Node B Lights", nodeB, 20U, 12.0F, 12.0F, 4U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Node B AC", nodeB, 21U, 350.0F, 450.0F, 8U, kilowatts::LoadMode::Auto::ON, {true, 8U, 0U}},
        {"Node B Garage", nodeB, 22U, 20.0F, 20.0F, 5U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Node B Heater", nodeB, 23U, 500.0F, 550.0F, 10U, kilowatts::LoadMode::Auto::OFF, {true, 19U, 0U}},
        {"Node C Pump", nodeC, 24U, 90.0F, 200.0F, 1U, kilowatts::LoadMode::Fixed::ON, {false, 0U, 0U}},
        {"Node C Washer", nodeC, 25U, 400.0F, 600.0F, 7U, kilowatts::LoadMode::Auto::OFF, {true, 10U, 0U}},
        {"Node C Pool", nodeC, 26U, 250.0F, 300.0F, 6U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Node C Lights", nodeC, 27U, 15.0F, 15.0F, 3U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Node D Router", nodeD, 28U, 10.0F, 10.0F, 2U, kilowatts::LoadMode::Fixed::ON, {false, 0U, 0U}},
        {"Node D Fans", nodeD, 29U, 30.0F, 45.0F, 3U, kilowatts::LoadMode::Auto::ON, {false, 0U, 0U}},
        {"Node D Charger", nodeD, 30U, 250.0F, 300.0F, 7U, kilowatts::LoadMode::Auto::OFF, {true, 12U, 0U}},
        {"Node D EV", nodeD, 31U, 1500.0F, 1500.0F, 10U, kilowatts::LoadMode::Auto::OFF, {true, 20U, 0U}}
    };
}

} // namespace

int main() {
    const std::vector<LoadSpec> specs = realLoads();
    const float capacities[] = {20.0F, 100.0F, 1000.0F};
    const float socValues[] = {25.0F, 50.0F, 75.0F, 100.0F};
    std::ofstream csv("test/report/csv/real_16_load_scenarios.csv");
    if (!csv) return 2;
    csv << "capacity_ah,soc_percent,available_watts,naive_watts,coordinated_watts,selected_auto_loads,coordinated_runtime_hours,naive_runtime_hours,result\n";

    std::ofstream inventory("test/report/csv/real_16_load_inventory.csv");
    if (!inventory) return 2;
    inventory << "name,node_mac,relay_pin,running_watts,startup_watts,priority,mode,schedule_enabled,schedule_hour,schedule_minute\n";
    std::printf("LOAD INVENTORY\n");
    for (const LoadSpec& spec : specs) {
        const char* mode = spec.mode == kilowatts::LoadMode::Fixed::ON ? "FIXED_ON" :
                           spec.mode == kilowatts::LoadMode::Auto::ON ? "AUTO_ON" : "AUTO_OFF";
        std::printf("  %-18s MAC %02X:%02X:%02X:%02X:%02X:%02X pin %u mode %s schedule %s %02u:%02u\n",
                    spec.name, spec.node[0], spec.node[1], spec.node[2], spec.node[3],
                    spec.node[4], spec.node[5], spec.pin, mode,
                    spec.schedule.enabled ? "ON" : "OFF", spec.schedule.hour, spec.schedule.minute);
        inventory << spec.name << ','
                  << std::hex << static_cast<unsigned>(spec.node[0]) << ':'
                  << static_cast<unsigned>(spec.node[1]) << ':'
                  << static_cast<unsigned>(spec.node[2]) << ':'
                  << static_cast<unsigned>(spec.node[3]) << ':'
                  << static_cast<unsigned>(spec.node[4]) << ':'
                  << static_cast<unsigned>(spec.node[5]) << std::dec << ','
                  << static_cast<unsigned>(spec.pin) << ',' << spec.runningWatts << ','
                  << spec.startupWatts << ',' << spec.priority << ',' << mode << ','
                  << (spec.schedule.enabled ? "true" : "false") << ','
                  << static_cast<unsigned>(spec.schedule.hour) << ','
                  << static_cast<unsigned>(spec.schedule.minute) << '\n';
    }

    bool passed = true;
    float previousCapacityAtFullSoc = -1.0F;
    for (float capacity : capacities) {
        float previousSoc = -1.0F;
        float currentCapacityAtFullSoc = 0.0F;
        for (float soc : socValues) {
            const RunResult result = run(specs, capacity, soc);
            const float usableEnergy = 12.0F * capacity * std::max(0.0F, soc - 20.0F) / 100.0F;
            const float coordinatedRuntime = result.committedWatts > 0.0F ? usableEnergy / result.committedWatts : 0.0F;
            const float naiveRuntime = result.naiveWatts > 0.0F ? usableEnergy / result.naiveWatts : 0.0F;
            const bool scenarioPassed = result.valid &&
                result.committedWatts >= previousSoc - 0.01F &&
                std::isfinite(coordinatedRuntime) && std::isfinite(naiveRuntime);
            passed = scenarioPassed && passed;
            previousSoc = result.committedWatts;
            currentCapacityAtFullSoc = result.committedWatts;
            csv << capacity << ',' << soc << ',' << result.availableWatts << ',' << result.naiveWatts << ','
                << result.committedWatts << ',' << result.selectedLoads << ',' << coordinatedRuntime << ','
                << naiveRuntime << ',' << (scenarioPassed ? "PASS" : "FAIL") << '\n';
            std::printf("16-load scenario: %5.0f Ah, %3.0f%%, coordinated %.1f W, naive %.1f W: %s\n",
                        capacity, soc, result.committedWatts, result.naiveWatts,
                        scenarioPassed ? "PASS" : "FAIL");
        }
        passed = (previousCapacityAtFullSoc < 0.0F ||
                  currentCapacityAtFullSoc >= previousCapacityAtFullSoc - 0.01F) && passed;
        previousCapacityAtFullSoc = currentCapacityAtFullSoc;
    }
    return passed ? 0 : 1;
}
