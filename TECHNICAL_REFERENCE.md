# Technical Reference — Power Flow

Kilowatts keeps planning and measurement separate.

## Canonical power values

```text
P_budget
P_reserve
P_fixed
P_auto_available
P_auto
P_remaining
P_measured
```

Meanings:

- `P_budget`: configured installation power allocation.
- `P_reserve`: configured watts intentionally left unused.
- `P_fixed`: sum of `FIXED_ON` load ratings.
- `P_auto_available`: watts passed to Best-First Search.
- `P_auto`: sum of AUTO load ratings selected by Best-First Search.
- `P_remaining`: configured power left after FIXED + selected AUTO loads.
- `P_measured`: instantaneous voltage × current from INA219 or simulation.

Without runtime planning:

```text
P_auto_available = max(0, P_budget - P_reserve - P_fixed)
```

After selection:

```text
P_remaining = max(0, P_budget - (P_fixed + P_auto))
```

## Runtime calculation

Runtime is an internal constraint on `P_auto_available`; it does not add another public power variable.

The internal calculation is:

```text
usableEnergyWh =
    capacityAh × nominalVoltage ×
    max(0, SoC - minimumSoC) / 100

runtime allowance = usableEnergyWh / remainingRuntimeHours

planning allowance =
    min(P_budget - P_reserve, runtime allowance)

P_auto_available =
    max(0, planning allowance - P_fixed)
```

Example:

```text
P_budget = 200 W
P_reserve = 20 W
Battery = 200 Ah at 15 V
SoC = 70%
minimum SoC = 20%
runtime = 24 h
P_fixed = 40 W
```

Usable battery energy is 1500 Wh. The internal runtime allowance is 62.5 W, therefore:

```text
P_auto_available = 62.5 - 40 = 22.5 W
```

Only `P_auto_available` is passed to Best-First.

## INA219 and simulation

Both use the same `PowerManager` path.

```text
if simulation enabled:
    voltage/current come from simulated values
else:
    voltage/current come from INA219

P_measured = voltage × current
```

`P_measured` is monitoring data. It is not the installation allocation budget.

This avoids the zero-load problem: a real sensor can measure approximately 0 W when all loads are OFF, while the installation can still have configured capacity to start loads.

## FIXED and AUTO

Central first computes:

```text
P_fixed = sum(FIXED_ON powerRatingWatts)
```

Then it computes `P_auto_available` and sends the AUTO candidates plus that value to the existing Best-First Search.

The Best-First Search algorithm itself is unchanged.

Selected AUTO loads are summed into:

```text
P_auto
```

Then:

```text
P_remaining = max(0, P_budget - (P_fixed + P_auto))
```

## Best-First result

The active planner uses only these rejection states:

```text
NONE
POWER_BUDGET_EXCEEDED
```

Old battery-current, main-current, and branch-current rejection reasons are not part of this model.

## Monitoring

The system can compare:

```text
P_measured > P_budget
```

If true, it publishes a monitoring warning. It does not guess the electrical cause and does not claim hardware protection.

## Interfaces

Console power output uses:

```text
P_budget
P_reserve
P_fixed
P_auto_available
P_auto
P_remaining
P_measured
```

Runtime is shown as hours/status, not as another named power value.

MQTT uses five external topics:

```text
status
state
command
ack
alert
```

The retained `state` topic exposes the same canonical power values.

## Hardware boundary

Firmware performs measurement, energy estimation, planning, Best-First allocation, monitoring, and relay/GPIO commands. Electrical protection remains the responsibility of appropriately rated physical hardware such as fuses, breakers, BMS cut-offs, and thermal protection.
