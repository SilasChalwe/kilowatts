# Technical Reference — Power Flow and Best-First Search

Kilowatts separates **power planning** from **power measurement**.

- `P_budget` is configured for the installation and is the source of truth for load allocation.
- INA219 or simulation supplies voltage and current.
- `P_measured = voltage × current` reports what is flowing at that moment.
- Best-First Search receives only `P_auto_available` and selects AUTO Loads that fit inside it.

Simulation and INA219 use the same `PowerManager` logic. The only difference is the source of voltage/current input.

## 1. Core power variables

```text
P_budget
    configured installation power allocation

P_reserve
    configured watts deliberately kept unused

P_usable
    P_budget - P_reserve

P_fixed
    sum of all FIXED_ON Load power ratings

P_auto_available
    watts available to Best-First Search

P_auto
    sum of the AUTO Loads selected by Best-First Search

P_remaining
    P_budget - (P_fixed + P_auto)

P_measured
    measured/simulated voltage × current
```

The main equations are:

```text
P_usable = max(0, P_budget - P_reserve)

P_auto_available = max(0, planningAllowance - P_fixed)

P_remaining = max(0, P_budget - (P_fixed + P_auto))
```

Without a runtime target:

```text
planningAllowance = P_usable
```

### Example

For:

```text
P_budget  = 200 W
P_reserve = 20 W
P_fixed   = 80 W
```

then:

```text
P_usable         = 200 - 20 = 180 W
P_auto_available = 180 - 80 = 100 W
```

If Best-First selects AUTO Loads totaling 100 W:

```text
P_auto      = 100 W
P_remaining = 200 - (80 + 100) = 20 W
```

The remaining 20 W is the configured power reserve.

If Best-First selects only 70 W:

```text
P_remaining = 200 - (80 + 70) = 50 W
```

That 50 W contains the 20 W reserve plus 30 W of allocatable capacity that was not selected.

## 2. Battery energy and runtime target

Power and energy are kept separate.

Battery energy metadata:

```text
ratedEnergyWh = batteryCapacityAh × nominalVoltageVolts

usableSoCFraction =
    max(0, stateOfChargePercent - minimumStateOfChargePercent) / 100

usableEnergyWh = ratedEnergyWh × usableSoCFraction
```

When the user requests a runtime target:

```text
P_runtime = usableEnergyWh / remainingRuntimeHours

planningAllowance = min(P_usable, P_runtime)

P_auto_available = max(0, planningAllowance - P_fixed)
```

Example:

```text
Battery capacity       = 200 Ah
Nominal voltage        = 15 V
State of Charge        = 70%
Minimum SoC reserve    = 20%
Required runtime       = 24 h
```

Usable energy above the reserve:

```text
usableEnergyWh = 200 × 15 × (70 - 20) / 100
               = 1500 Wh
```

Runtime power:

```text
P_runtime = 1500 / 24
          = 62.5 W
```

If `P_fixed = 40 W`:

```text
P_auto_available = 62.5 - 40
                 = 22.5 W
```

If `P_fixed = 80 W`, fixed demand already exceeds the 62.5 W runtime allowance. The firmware reports the runtime target as not achievable and gives AUTO Loads 0 W. It does not alter or hide the real fixed demand.

## 3. INA219 and simulation

Both paths produce the same input variables:

```text
voltageVolts
currentAmps
P_measured = voltageVolts × currentAmps
```

Hardware mode obtains voltage/current from INA219.

Simulation mode accepts simulated voltage/current values.

After input acquisition, the same measurement and planning objects are used. There is no second simulation planner.

Example:

```text
Voltage = 15 V
Current = 1.5 A

P_measured = 15 × 1.5
           = 22.5 W
```

`22.5 W` is instantaneous measured power. It does **not** replace a configured `P_budget = 200 W`.

This distinction prevents the zero-load deadlock problem: when no load is drawing current, INA219 can read approximately 0 W even though the installation can still start loads.

## 4. Measurement vs configured budget

The firmware can compare:

```text
P_measured > P_budget
```

If true, it publishes a `MEASURED_POWER_BUDGET` warning through MQTT.

This means only that measured instantaneous power is above the configured installation budget. The firmware does not guess the physical cause.

Possible causes include an unaccounted load, a load drawing above its configured rating, inrush, a wiring/fault condition, configuration error, or measurement error.

This is **monitoring**, not hardware electrical protection.

## 5. Best-First Search

Source: `lib/BestFirstSearch/BestFirstSearch.cpp`.

The Best-First Search algorithm is unchanged by the clean power-flow migration.

The Central runtime first accounts for FIXED_ON Loads and then constructs:

```text
P_auto_available
```

The existing search is called with:

```text
BestFirstSearch(
    P_auto_available,
    automaticLoads,
    currentTimeProvider)
```

A candidate combination is feasible only when:

```text
total AUTO Load power <= P_auto_available
```

### Priority and search ranking

For an AUTO Load whose schedule window is active:

```text
effectivePriority = configuredPriority + ACTIVE_SCHEDULE_PRIORITY_BOOST
```

For a candidate combination:

```text
totalEffectivePriority = sum(effectivePriority)
g = number of selected Loads
h = average selected Load power
f = g + h
```

The existing algorithm ranks combinations using its current priority/tie-breaking rules. This migration changes the **power value passed into the algorithm**, not the algorithm itself.

## 6. FIXED and AUTO Loads

FIXED_ON power is accounted first:

```text
P_fixed = sum(FIXED_ON powerRatingWatts)
```

Then:

```text
P_auto_available = planningAllowance - P_fixed
```

AUTO Loads are the candidates sent to Best-First Search.

If fixed demand is already greater than the planning allowance:

```text
P_auto_available = 0
```

The software reports the condition rather than pretending to provide physical hardware protection.

## 7. Rejection result

The active planner uses:

```text
NONE
POWER_BUDGET_EXCEEDED
```

The old battery-current, main-current and branch-current rejection reasons are not part of the clean power-flow model.

## 8. Where the values are visible

### Console

`dashboard` and `optimize run` show:

```text
P_budget
P_reserve
P_usable
P_fixed
P_auto_available
P_auto
P_remaining
P_measured
P_runtime (when runtime is active)
```

Power planning is configured with:

```text
battery planning budget=W reserve=W min_soc=PERCENT [runtime_hours=H]
```

### MQTT

`state/system` publishes:

```text
battery.P_measured
battery.P_runtime
powerFlow.P_budget
powerFlow.P_reserve
powerFlow.P_usable
powerFlow.P_fixed
powerFlow.P_auto_available
powerFlow.P_auto
powerFlow.P_remaining
```

See `lib/MqttManager/README.md` for schema 4.

## 9. Hardware-protection boundary

Kilowatts firmware performs:

- measurement,
- battery/energy estimation,
- configured power allocation,
- optimization,
- monitoring/warnings,
- relay/GPIO commands.

Physical electrical protection remains the job of appropriately rated hardware such as fuses, breakers, BMS cut-offs and other protection devices. Software comparison of watts is not described as a replacement for those devices.
