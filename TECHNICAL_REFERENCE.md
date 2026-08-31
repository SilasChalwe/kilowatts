# Technical Reference — Power Flow

Kilowatts keeps planning and measurement separate.

## Exact power values

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
- `P_reserve`: configured watts intentionally left outside allocation.
- `P_fixed`: sum of `FIXED_ON` load ratings.
- `P_auto_available`: watts passed to Best-First Search.
- `P_auto`: sum of AUTO load ratings selected by Best-First Search.
- `P_remaining`: configured power left after FIXED + selected AUTO loads.
- `P_measured`: instantaneous voltage × current from INA219 or simulation.

No additional public power names are used.

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

Conceptually:

```text
usable battery energy =
    capacityAh × nominalVoltage ×
    max(0, SoC - minimumSoC) / 100

runtime allowance =
    usable battery energy / remainingRuntimeHours

P_auto_available = max(
    0,
    min(P_budget - P_reserve, runtime allowance) - P_fixed
)
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
P_auto_available = 22.5 W
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

`P_measured` is monitoring data. It is not `P_budget` and it is not used as the starting allocation budget.

## FIXED and AUTO

```text
P_fixed = sum(FIXED_ON powerRatingWatts)
```

Central calculates `P_auto_available` and sends the AUTO candidates plus that value to the existing Best-First Search. The Best-First Search algorithm itself is unchanged.

Selected AUTO loads are summed into `P_auto`, then:

```text
P_remaining = max(0, P_budget - (P_fixed + P_auto))
```

## Best-First result

The active planner uses only:

```text
NONE
POWER_BUDGET_EXCEEDED
```

Old battery-current, main-current and branch-current rejection reasons are not part of this model.

## Monitoring

The system can compare:

```text
P_measured > P_budget
```

If true, it publishes a monitoring warning. It does not guess the electrical cause and does not claim hardware protection.

## Console and frontend boundary

The Central serial console owns installation setup:

```text
Wi-Fi credentials
MQTT broker credentials
INA219/shunt technical configuration
```

These settings are not remotely exposed through MQTT commands or frontend state.

Frontend MQTT has five external topics:

```text
status
state
command
ack
alert
```

`status` is the authoritative Central liveness signal. Central publishes retained `online`; the MQTT Last Will publishes retained `offline` if Central disappears.

`state` contains:

```text
system
loads
nodes
```

The system state contains battery/measurement/power-flow data, not Wi-Fi or MQTT connection details.

The nodes state includes Central and Smart Node hardware/device information such as firmware version, chip model, relay capability and diagnostics. Central's node entry is not used as its liveness signal; `status` is.

Frontend `command` accepts only:

```text
node
load
battery
sensor
system
```

Node commands support add/update/delete. Load commands support add/update/delete. Battery commands set planning inputs. Sensor commands choose INA219/simulation or provide simulation values. System commands trigger optimization, change its interval, or restart Central.

## Hardware boundary

Firmware performs measurement, energy estimation, planning, Best-First allocation, monitoring, and relay/GPIO commands. Electrical protection remains the responsibility of appropriately rated physical hardware such as fuses, breakers, BMS cut-offs, and thermal protection.
