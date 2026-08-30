# Installation Guide

This guide takes Kilowatts from a flashed board to a real installation using the clean power-planning model.

The rule is simple: **configure the installation allocation, then use INA219 or simulation to observe instantaneous consumption.**

## 1. Record the installation values

Before configuration, record:

- each Load and its expected watts,
- which Loads are FIXED and which are AUTO,
- `P_budget`,
- `P_reserve`,
- battery nominal voltage and capacity,
- optional minimum SoC and runtime target,
- INA219 shunt resistance and expected measurement range.

Do not invent electrical protection limits that are not represented by real hardware or an intentional planning policy.

## 2. Configure power planning

Example:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=24
```

This gives:

```text
P_budget  = 200 W
P_reserve = 20 W
P_usable  = 180 W
```

FIXED_ON Loads are accounted before AUTO planning:

```text
P_auto_available = max(0, planningAllowance - P_fixed)
```

Best-First Search receives only `P_auto_available`.

If runtime planning is not needed:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=0
```

## 3. Configure battery and INA219 metadata

Use the actual battery and sensor specifications:

```text
battery configure shunt_ohms=0.005 max_sensor_amps=40 ema_alpha=0.2 capacity_ah=200 initial_soc=70 nominal_voltage=15
```

`max_sensor_amps` describes the intended INA219/shunt measurement range. It is not a software current-protection limit.

## 4. Test the same system with simulated input

Select simulation:

```text
sensor sim
```

Feed voltage/current and optional SoC:

```text
sensor values voltage=15 current=1.5 soc=70
```

Central calculates:

```text
P_measured = 15 * 1.5 = 22.5 W
```

Run:

```text
dashboard
optimize
```

Check:

```text
P_budget
P_reserve
P_usable
P_fixed
P_auto_available
P_auto
P_remaining
P_measured
P_runtime
```

Simulation is only an alternative input source. It does not run a separate planner.

## 5. Configure Loads

Central-local example:

```text
load add pin=16 name=Lamp power=10 priority=5 type=DC active_high=off mode=AUTO_OFF schedule=none
```

Smart Node example:

```text
load add mac=AA:BB:CC:DD:EE:FF pin=4 name=Fan power=25 priority=10 type=DC active_high=off mode=AUTO_OFF schedule=18:30-20:00
```

Use realistic `power=` values because planning uses each Load's configured `powerRatingWatts`.

## 6. Check FIXED and AUTO allocation

Example:

```text
P_budget  = 200 W
P_reserve = 20 W
P_fixed   = 80 W

P_usable         = 180 W
P_auto_available = 100 W
```

If Best-First selects 70 W:

```text
P_auto      = 70 W
P_remaining = 200 - (80 + 70) = 50 W
```

## 7. Check runtime planning

For a 200 Ah, 15 V battery at 70% SoC, with 20% minimum SoC and a 24-hour target:

```text
usableEnergyWh = 200 * 15 * (70 - 20) / 100
               = 1500 Wh

P_runtime = 1500 / 24
          = 62.5 W
```

If `P_fixed = 40 W`:

```text
P_auto_available = 62.5 - 40 = 22.5 W
```

If fixed demand already exceeds the runtime allowance, AUTO allocation becomes 0 and the runtime target is reported as not achievable.

## 8. Switch to the real INA219

Verify the real sensor wiring and configuration, then select:

```text
sensor ina219
```

Check:

```text
battery
dashboard
```

INA219 now supplies voltage/current to the same path used during simulation.

## 9. Compare measured consumption with the configured budget

```text
P_measured = voltage * current
```

The installation planning limit remains:

```text
P_budget
```

If:

```text
P_measured > P_budget
```

Kilowatts can publish `MEASURED_POWER_BUDGET` as a monitoring warning. It does not identify the physical cause and is not a replacement for electrical protection hardware.

## 10. Commission Smart Nodes

```text
node commission AA:BB:CC:DD:EE:FF name=Kitchen
nodes
node show AA:BB:CC:DD:EE:FF
loads
```

## 11. Verify MQTT

Check connection:

```text
mqtt status
```

The external API has only five topics:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

`state` combines current system, Load and Node state. All incoming operations use `command` with a `type` field. See `lib/MqttManager/README.md`.

## 12. Final checks

Before leaving the installation:

- confirm expected Nodes are online,
- verify every Load's mode, watts and priority,
- verify `P_budget` and `P_reserve`,
- verify SoC/runtime settings when used,
- compare `P_measured` with expected active demand,
- verify relay/GPIO commands work as intended,
- verify the installation has correctly rated fuses, breakers, BMS or other required electrical protection.

Kilowatts software performs planning, monitoring and actuation commands. Physical electrical protection remains a separate hardware responsibility.

See also:

- `USER_MANUAL.md`
- `TECHNICAL_REFERENCE.md`
- `lib/MqttManager/README.md`
- `LIMITATIONS.md`
