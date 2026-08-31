# Installation Guide

## 1. Configure the power plan

Decide the installation power budget and reserve.

Example:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=24
```

This configures:

```text
P_budget  = 200 W
P_reserve = 20 W
```

If `FIXED_ON` loads total 80 W and no runtime limit reduces allocation:

```text
P_fixed = 80 W
P_auto_available = 200 - 20 - 80 = 100 W
```

Best-First receives `P_auto_available`.

If Best-First selects AUTO loads totaling 70 W:

```text
P_auto = 70 W
P_remaining = 200 - (80 + 70) = 50 W
```

## 2. Runtime

If `runtime_hours` is greater than zero, battery energy and SoC are used internally to reduce `P_auto_available` when necessary.

Runtime does not introduce another public power variable. The console/MQTT reports the runtime hours/status and the resulting `P_auto_available`.

To disable runtime planning:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=0
```

## 3. Configure battery and INA219

Use actual battery and sensor values:

```text
battery configure shunt_ohms=0.005 max_sensor_amps=40 ema_alpha=0.2 capacity_ah=200 initial_soc=70 nominal_voltage=15
```

`max_sensor_amps` describes the expected INA219/shunt measurement range. It is not a software protection limit.

## 4. Test with simulation

Simulation is only another input source.

```text
sensor sim
sensor values voltage=15 current=1.5 soc=70
```

The system calculates:

```text
P_measured = 15 × 1.5 = 22.5 W
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
P_fixed
P_auto_available
P_auto
P_remaining
P_measured
```

## 5. Switch to INA219

```text
sensor ina219
battery status
dashboard
```

INA219 now supplies voltage/current to the same measurement path used by simulation.

## 6. Configure loads

Example Central load:

```text
load add pin=16 name=Lamp power=10 priority=5 type=DC active_high=off mode=AUTO_OFF schedule=none
```

Example Smart Node load:

```text
load add mac=AA:BB:CC:DD:EE:FF pin=4 name=Fan power=25 priority=10 type=DC active_high=off mode=AUTO_OFF schedule=18:30-20:00
```

Use realistic `power=` values because planning uses each load's configured power rating.

## 7. Check operation

Use:

```text
status
loads
optimize
dashboard
```

The system should:

1. sum `FIXED_ON` loads into `P_fixed`;
2. calculate `P_auto_available`;
3. pass `P_auto_available` to Best-First;
4. command selected AUTO loads ON and unselected AUTO loads OFF;
5. calculate `P_auto` and `P_remaining`;
6. measure actual consumption as `P_measured`.

## 8. MQTT

MQTT exposes only:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

Use `state` for system/load/node state and `command` for all remote commands.

## 9. Final hardware check

Before using a real installation, verify:

- correct `P_budget` and `P_reserve`;
- correct FIXED/AUTO modes and load watt ratings;
- real INA219 wiring and shunt rating;
- relay operation;
- correctly rated physical fuses, breakers, BMS, and other required protection hardware.

Kilowatts software plans, monitors, and commands loads. It does not replace electrical protection hardware.
