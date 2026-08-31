# Kilowatts — User Manual

Kilowatts is a load-allocation and power-monitoring system. Central accounts for FIXED loads, gives `P_auto_available` to the existing Best-First Search, and commands the selected loads ON or OFF. INA219 or simulation supplies voltage/current into the same measurement path.

## Power names

These are the power names used by the system:

```text
P_budget          configured installation power limit
P_reserve         watts the user intentionally keeps unused
P_fixed           total watts of FIXED_ON loads
P_auto_available  watts passed to Best-First Search
P_auto            total watts of AUTO loads selected by Best-First
P_remaining       unused part of P_budget after FIXED + selected AUTO loads
P_measured        instantaneous power from voltage × current
```

Without a runtime target:

```text
P_auto_available = max(0, P_budget - P_reserve - P_fixed)
```

After Best-First selects AUTO loads:

```text
P_remaining = max(0, P_budget - (P_fixed + P_auto))
```

Example:

```text
P_budget          = 200 W
P_reserve         = 20 W
P_fixed           = 80 W
P_auto_available  = 100 W
```

If Best-First selects 100 W:

```text
P_auto       = 100 W
P_remaining  = 20 W
```

## Runtime

Runtime is an optional planning condition. It does not create another public power variable.

The system uses battery capacity, nominal voltage, current SoC, minimum SoC, and requested runtime to work out how much AUTO power can be allowed while trying to meet the requested runtime. The result is reflected directly in `P_auto_available`.

If the requested runtime cannot be met because FIXED loads already consume too much, then:

```text
P_auto_available = 0 W
Runtime target = NOT ACHIEVABLE
```

FIXED demand is still reported honestly.

## INA219 and simulation

There is one measurement path:

```text
if simulation:
    voltage/current = simulated input
else:
    voltage/current = INA219 input

P_measured = voltage × current
```

`P_measured` tells the system what is actually being consumed at that moment. It does not replace `P_budget`.

If `P_measured > P_budget`, Kilowatts can report a monitoring warning. This is software monitoring, not electrical hardware protection.

## Load behavior

Supported modes are:

```text
FIXED_ON
FIXED_OFF
AUTO_ON
AUTO_OFF
```

`FIXED_ON` loads are counted first in `P_fixed`.

AUTO loads are candidates for Best-First Search. Best-First receives only:

```text
P_auto_available
```

The Best-First Search algorithm is unchanged.

## Main console commands

```text
status
dashboard
battery
sensor ina219
sensor sim
sensor values voltage=15 current=8 soc=70
nodes
loads
optimize
wifi status
mqtt status
```

Configure the power plan:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=24
```

Disable runtime planning while keeping the same budget/reserve:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=0
```

Configure battery/INA219 information:

```text
battery configure shunt_ohms=0.005 max_sensor_amps=40 ema_alpha=0.2 capacity_ah=200 initial_soc=70 nominal_voltage=15
```

`max_sensor_amps` describes the expected sensor/shunt measurement range. It is not a software current-protection limit.

## MQTT

MQTT has only five external topics:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

`state` contains system, load, and node state.

The power state uses only:

```text
P_budget
P_reserve
P_fixed
P_auto_available
P_auto
P_remaining
P_measured
```

All MQTT commands go to `command` and use a `type` field.

See `lib/MqttManager/README.md` for examples.

## What the system answers

For each planning cycle, Kilowatts answers:

1. How much power is configured? → `P_budget`
2. How much is reserved? → `P_reserve`
3. How much are FIXED_ON loads already using? → `P_fixed`
4. How much can AUTO loads use? → `P_auto_available`
5. Which AUTO loads fit best? → existing Best-First Search
6. How much AUTO power was selected? → `P_auto`
7. How much configured power remains? → `P_remaining`
8. What is actually being consumed now? → `P_measured`

Central then sends relay commands for the selected load states.

## Hardware boundary

Kilowatts performs planning, monitoring, and relay/GPIO commands. Real electrical protection must come from correctly rated hardware such as fuses, breakers, BMS cut-offs, and other installation-specific protection devices.
