# PowerBudgetCalculator

Calculates the safe total power budget for one planning cycle
(Section 4.6.2.3, Equations 4.9-4.14) — the `totalAvailablePowerWatts`
`BestFirstSearch` and `AvailablePowerManager` both consume.

## Responsibility

```cpp
PowerBudgetCalculator::Inputs inputs{};
inputs.stateOfChargePercent = batteryStateOfCharge.getStateOfChargePercent();
inputs.minimumStateOfChargePercent = config.socMinPercent;
inputs.nominalBatteryVoltageVolts = config.nominalBatteryVoltageVolts;
inputs.batteryCapacityAmpHours = config.batteryCapacityAmpHours;
inputs.targetRuntimeHours = config.targetRuntimeHours;
inputs.batteryBusVoltageVolts = filteredBatteryMeasurements.voltageVolts;
inputs.maximumBatteryDischargeCurrentAmps = config.maximumBatteryDischargeCurrentAmps;
inputs.maximumMainCurrentAmps = config.maximumMainCurrentAmps;
inputs.safetyFactor = config.safetyFactor;

PowerBudgetCalculator budget;
budget.calculate(inputs);

float pAvailable = budget.getAvailablePowerWatts();   // -> AvailablePowerManager::calculateAvailablePower()
```

```
E_rated   = V_nom * C_B                                          (4.9)
E_usable  = E_rated * max(0, (SoC - SoC_min) / 100)               (4.10)
P_runtime = E_usable / T_target                                   (4.11)
P_battery,max = V_B * I_B,max                                     (4.12)
P_main,max    = V_B * I_main,max                                  (4.13)
P_available   = rho * min(P_runtime, P_battery,max, P_main,max)   (4.14)
```

When `SoC <= SoC_min`, Equation 4.10's `max(0, ...)` clamp already forces
`E_usable`, and therefore `P_runtime` and `P_available`, to zero — the
"`P_available` = 0 for normal Auto-load allocation" rule Section 4.6.2.3
states in words falls directly out of these equations, so one uniform
calculation path (not a special-cased branch) produces a consistent
`P_available` for the whole planning cycle regardless of battery state
(see `test/PowerBudgetCalculator/`'s SoC-at-or-below-minimum test).

`calculate()` rejects (previous result, if any, unchanged): a
`stateOfChargePercent`/`minimumStateOfChargePercent` outside `[0, 100]`,
a non-positive `nominalBatteryVoltageVolts`/`batteryCapacityAmpHours`/
`targetRuntimeHours`/`batteryBusVoltageVolts`, a negative current limit,
a `safetyFactor` outside `(0, 1]`, or any non-finite input.

## Boundary

Battery policy fields (`SoC_min`, `V_nom`, `C_B`, `T_target`, current
limits, `rho`) are configuration this class never invents defaults for —
they come from `CentralNodeConfig.h`. `stateOfChargePercent` comes from
`BatteryStateOfCharge`; `batteryBusVoltageVolts` comes from the central
INA219's filtered measurement (`INA219Monitor`). This class does not read
hardware, does not perform Fixed-Load allocation
(`AvailablePowerManager`), and does not run Best-First Search.
