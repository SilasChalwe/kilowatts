# Technical Reference — Power Flow and Best-First Search

Kilowatts separates **power planning** from **power measurement**.

- `P_budget` is configured for the installation and is the source of truth for load allocation.
- INA219 or simulation supplies voltage and current.
- `P_measured = voltage × current` reports what is flowing at that moment.
- Best-First Search receives only `P_auto_available` and selects AUTO Loads that fit inside it.
- Simulation and INA219 use the same `PowerManager`; only the input source changes.

## 1. Canonical power variables

```text
P_budget          configured installation allocation
P_reserve         watts deliberately kept unused
P_usable          P_budget - P_reserve
P_fixed           sum of FIXED_ON Load ratings
P_auto_available  watts passed to Best-First Search
P_auto            sum of selected AUTO Load ratings
P_remaining       unallocated part of P_budget
P_measured        voltage * current from INA219/simulation
P_runtime         average power allowed by an active runtime target
```

Main equations:

```text
P_usable = max(0, P_budget - P_reserve)

P_auto_available = max(0, planningAllowance - P_fixed)

P_remaining = max(0, P_budget - (P_fixed + P_auto))
```

Without a runtime target:

```text
planningAllowance = P_usable
```

Example:

```text
P_budget  = 200 W
P_reserve = 20 W
P_fixed   = 80 W

P_usable         = 180 W
P_auto_available = 100 W
```

If Best-First selects `P_auto = 100 W`:

```text
P_remaining = 200 - (80 + 100) = 20 W
```

If it selects only 70 W:

```text
P_remaining = 200 - (80 + 70) = 50 W
```

That 50 W contains the 20 W reserve plus 30 W that was allocatable but not selected.

## 2. Runtime and battery energy

Power and energy remain separate.

```text
ratedEnergyWh = capacityAh * nominalVoltage

usableEnergyWh = ratedEnergyWh *
    max(0, SoC - minimumSoC) / 100
```

With a runtime target:

```text
P_runtime = usableEnergyWh / remainingRuntimeHours

planningAllowance = min(P_usable, P_runtime)

P_auto_available = max(0, planningAllowance - P_fixed)
```

Example:

```text
capacity       = 200 Ah
nominalVoltage = 15 V
SoC            = 70%
minimumSoC     = 20%
runtime        = 24 h

usableEnergyWh = 200 * 15 * 0.50 = 1500 Wh
P_runtime      = 1500 / 24 = 62.5 W
```

If `P_fixed = 40 W`:

```text
P_auto_available = 62.5 - 40 = 22.5 W
```

If fixed demand already exceeds the runtime allowance, AUTO allocation is 0 and the runtime target is reported as not currently achievable. Fixed demand is not hidden or clipped.

## 3. INA219 and simulation

The input selection is conceptually:

```text
if simulationEnabled:
    voltage/current = simulated input
else:
    voltage/current = INA219 input

P_measured = voltage * current
```

After that branch, calibration/filtering, telemetry and power-planning state use the same objects and code path.

Example:

```text
Voltage = 15 V
Current = 1.5 A
P_measured = 22.5 W
```

`P_measured` is instantaneous consumption. It does not replace `P_budget`.

This also avoids a zero-load deadlock: a sensor may measure about 0 W while no loads are active even though the configured installation can still start loads.

## 4. Measurement monitoring

The firmware can compare:

```text
P_measured > P_budget
```

When this condition changes, Central can publish `MEASURED_POWER_BUDGET`.

This means only that measured instantaneous power is above the configured planning budget. The software does not infer the physical cause and does not claim hardware electrical protection.

## 5. Best-First Search

Source: `lib/BestFirstSearch/BestFirstSearch.cpp`.

The algorithm is unchanged by this cleanup.

Central first calculates `P_fixed`, then `P_auto_available`, then calls the existing search with:

```text
BestFirstSearch(
    P_auto_available,
    automaticLoads,
    currentTimeProvider)
```

A candidate remains feasible only when its AUTO-load power fits within `P_auto_available`. Priority and tie-breaking remain those already implemented by Best-First Search.

## 6. FIXED and AUTO Loads

```text
P_fixed = sum(FIXED_ON powerRatingWatts)
```

AUTO loads are then considered by Best-First Search using the available value:

```text
P_auto_available = max(0, planningAllowance - P_fixed)
```

The final allocation is recorded as:

```text
P_auto = sum(selected AUTO powerRatingWatts)

P_remaining = max(0, P_budget - (P_fixed + P_auto))
```

## 7. Planner result codes

The active runtime uses only:

```text
NONE
POWER_BUDGET_EXCEEDED
```

Old battery-current, main-current and branch-current rejection reasons are not part of this model.

## 8. Console interface

Power values are visible with:

```text
dashboard
```

Power planning is configured with:

```text
battery planning budget=W reserve=W min_soc=PERCENT [runtime_hours=H]
```

Measurement input is controlled through one command:

```text
sensor ina219
sensor sim
sensor values voltage=V current=A [soc=PERCENT]
```

There is no separate simulation planner or separate simulation console subsystem.

## 9. MQTT interface

The external MQTT API has only five topics:

```text
status
state
command
ack
alert
```

With the default namespace:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

`state` combines current system, Load and Node state in one retained payload. The system section exposes the canonical power values including `P_budget`, `P_reserve`, `P_usable`, `P_fixed`, `P_auto_available`, `P_auto`, `P_remaining`, `P_measured` and `P_runtime`.

All incoming operations use the single `command` topic with a `type` field:

```text
load
system
config
simulation
```

Acknowledgements use `ack`. Monitoring warnings and informational events use `alert`.

See `lib/MqttManager/README.md` for message examples.

## 10. Hardware-protection boundary

Kilowatts firmware performs:

- measurement,
- battery/energy estimation,
- configured power allocation,
- runtime planning,
- Best-First optimization,
- monitoring/warnings,
- relay/GPIO commands.

Physical electrical protection remains the job of appropriately rated hardware such as fuses, breakers, BMS cut-offs and other installation-specific protection devices.
