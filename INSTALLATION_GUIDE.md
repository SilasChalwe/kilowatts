# Installation Guide

This guide takes Kilowatts from a flashed board to a real installation using the clean power-planning model.

The main rule is simple: **configure the installation power budget first, then use INA219 or simulation to observe actual instantaneous power.**

## 1. Survey the installation

Before configuration, record:

- each Load and its expected operating watts,
- which Loads must be FIXED and which can be AUTO,
- the total power the installation is allowed to allocate (`P_budget`),
- how much power should stay unused (`P_reserve`),
- battery nominal voltage and capacity in Ah,
- desired minimum battery SoC,
- optional required runtime,
- INA219 shunt resistance and expected measurement current range.

Do not invent electrical limits that are not represented by real hardware or a justified planning requirement.

## 2. Configure the installation power plan

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

FIXED_ON Loads are deducted before AUTO planning:

```text
P_auto_available = planningAllowance - P_fixed
```

Best-First Search receives only `P_auto_available`.

If no runtime target is required, use:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=0
```

## 3. Configure battery and INA219 metadata

Use the actual battery and sensor specifications:

```text
battery configure shunt_ohms=0.005 max_sensor_amps=40 ema_alpha=0.2 capacity_ah=200 initial_soc=70 nominal_voltage=15
```

Important distinctions:

- `capacity_ah` and `nominal_voltage` are used for energy/runtime calculations.
- `initial_soc` initializes the battery SoC estimate.
- `shunt_ohms` and `max_sensor_amps` describe the INA219 measurement setup.
- `max_sensor_amps` is **not** a software current-protection limit.

For the INA219 shunt measurement range:

```text
shunt_ohms × max_sensor_amps <= 0.32 V
```

## 4. Test using simulation

Simulation is only another input source for voltage/current/SoC. It does not run a separate planner.

Enable it:

```text
sensor sim
```

Feed values:

```text
simulation values voltage=15 current=1.5 soc=70
```

The system calculates:

```text
P_measured = 15 × 1.5 = 22.5 W
```

Then run:

```text
dashboard
optimize run
```

Check the values:

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

Configure realistic Loads and test different conditions before connecting the real installation.

## 5. Configure Loads

Example Central-local Load:

```text
load add pin=16 name=Lamp power=10 priority=5 type=DC active_high=off mode=AUTO_OFF schedule=none
```

Example Smart Node Load:

```text
load add mac=AA:BB:CC:DD:EE:FF pin=4 name=Fan power=25 priority=10 type=DC active_high=off mode=AUTO_OFF schedule=18:30-20:00
```

Use realistic `power=` values because the planner uses each Load's configured `powerRatingWatts`.

## 6. Check FIXED and AUTO allocation

Suppose:

```text
P_budget  = 200 W
P_reserve = 20 W
P_fixed   = 80 W
```

Then:

```text
P_usable         = 180 W
P_auto_available = 100 W
```

Best-First may select AUTO Loads up to the available 100 W according to its existing algorithm and priorities.

If it selects 70 W:

```text
P_auto      = 70 W
P_remaining = 200 - (80 + 70)
            = 50 W
```

## 7. Check the runtime target

For a 200 Ah, 15 V battery at 70% SoC with a 20% minimum SoC and a 24-hour target:

```text
usableEnergyWh = 200 × 15 × (70 - 20) / 100
               = 1500 Wh

P_runtime = 1500 / 24
          = 62.5 W
```

If FIXED_ON demand is 40 W:

```text
P_auto_available = 62.5 - 40
                 = 22.5 W
```

If FIXED_ON demand is greater than the runtime allowance, AUTO allocation becomes 0 and the runtime target is reported as not achievable.

## 8. Switch to the real INA219

After simulation tests are satisfactory:

1. Verify the real sensor wiring and battery specifications.
2. Confirm `battery configure` contains the real values.
3. Select hardware input:

```text
sensor ina219
```

4. Check:

```text
battery status
dashboard
```

The real INA219 now supplies voltage/current to the same measurement path used during simulation.

## 9. Compare measured power with the plan

INA219 gives:

```text
P_measured = voltage × current
```

The configured installation budget remains:

```text
P_budget
```

If:

```text
P_measured > P_budget
```

Kilowatts can publish the `MEASURED_POWER_BUDGET` warning.

This warning says only that measured instantaneous power is above the configured planning budget. It does not prove the physical cause and it is not a replacement for electrical protection hardware.

## 10. Commission Smart Nodes

For each Smart Node:

```text
node commission AA:BB:CC:DD:EE:FF name=Kitchen
```

Check:

```text
nodes
node show AA:BB:CC:DD:EE:FF
loads
```

## 11. MQTT verification

Confirm MQTT connection:

```text
mqtt status
```

Monitor:

- `kilowatts/v1/state/system`
- `kilowatts/v1/state/loads`
- `kilowatts/v1/state/nodes`
- `kilowatts/v1/alerts`
- `kilowatts/v1/acks`

Schema 4 publishes the exact power-flow fields described above. See `lib/MqttManager/README.md`.

## 12. Final installation checks

Before leaving the site:

- confirm every expected Node is online,
- verify every Load's mode, watts and priority,
- verify `P_budget` and `P_reserve`,
- verify SoC/runtime settings if used,
- compare `P_measured` with the expected active Load demand,
- verify relay/GPIO commands work as intended,
- verify the physical installation has correctly rated fuses, breakers, BMS or other required electrical protection.

Kilowatts software performs planning, monitoring and actuation commands. Physical electrical protection remains a separate hardware responsibility.

See also:

- `USER_MANUAL.md`
- `TECHNICAL_REFERENCE.md`
- `lib/MqttManager/README.md`
- `LIMITATIONS.md`
