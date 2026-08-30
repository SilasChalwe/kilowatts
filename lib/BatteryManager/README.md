# BatteryManager

The current battery subsystem is implemented by `PowerManager`.

There is no `PowerBudgetCalculator`, `AvailablePowerManager`, `MaximumPowerAllowedForAllActiveLoadsCalculator`, or separate simulation planner in the current architecture.

## Measurement sources

`PowerManager` accepts battery measurements from one of two acquisition sources:

```text
real INA219 input ----\
                      -> same PowerManager state -> same runtime/budget calculation
simulation input -----/
```

The planning code does not use a different formula when simulation is enabled.

Battery measurements include voltage, current and power. State of Charge (SoC) is maintained by the same `PowerManager` state. Simulation can explicitly set controlled voltage/current values and a controlled SoC value for testing.

## Installation configuration

The Central battery configuration includes:

- INA219 shunt resistance,
- maximum expected sensor current,
- EMA alpha,
- battery capacity in ampere-hours (Ah),
- initial SoC,
- nominal battery voltage.

Power limits include:

- minimum/reserve SoC,
- maximum battery discharge current,
- maximum main current,
- user-required runtime in hours (`0` disables the runtime target).

## Runtime objective

When a runtime target is active, usable battery energy is estimated as:

```text
fullEnergyWh = capacityAh * nominalVoltageV
usableSoCFraction = max(0, currentSoC - reserveSoC) / 100
usableEnergyWh = fullEnergyWh * usableSoCFraction
sustainableTotalPowerW = usableEnergyWh / remainingRuntimeHours
```

Fixed ON Loads are already committed:

```text
fixedOnPowerW = sum(powerRatingWatts of FIXED_ON Loads)
runtimeAutoBudgetW = max(0, sustainableTotalPowerW - fixedOnPowerW)
```

Immediate electrical limits are calculated independently from battery/main current limits. The AUTO budget is the stricter of the immediate electrical budget and the runtime-sustainability budget.

Fixed power is not subtracted twice from the same limit. It is subtracted once from each independent *total* limit before those limits are compared.

If fixed loads alone exceed the sustainable runtime power, the AUTO budget becomes zero and the runtime target is reported as unachievable. Fixed modes are not silently changed.

When the requested runtime horizon is reached, the runtime constraint is fulfilled and is disabled; the ordinary electrical/current safety limits continue to apply.

## Simulation

Simulation is an input source for testing, not another system. Supported controlled values are:

- battery voltage,
- battery current,
- State of Charge.

These values feed the same `PowerManager` state used by runtime planning. Host-native tests validate the budget/planning math but do not claim physical INA219 or battery validation.
