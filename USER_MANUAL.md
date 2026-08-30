# Kilowatts — User Manual

This manual describes the current `clean-power-budget` firmware model.

Kilowatts is a load-allocation and monitoring system. The firmware decides which configured Loads should be requested ON or OFF, then writes the configured GPIO/control pin locally or sends the command to a Smart Node over ESP-NOW.

The firmware does not verify the physical appliance or relay after the GPIO pin unless separate feedback hardware is added. It must not be treated as a replacement for fuses, breakers, BMS cut-offs, or other electrical protection hardware.

## 1. Devices

- **Central** owns power planning, battery measurement, Wi-Fi, MQTT, the node registry, and Best-First Search.
- **Smart Nodes** report their configured Loads and receive control/configuration commands from Central over ESP-NOW.

Build examples:

```bash
pio run -e central
pio run -e smart
```

Upload examples:

```bash
pio run -e central -t upload
pio run -e smart -t upload
```

## 2. Power model

The configured installation budget is the starting point for planning.

```text
P_budget
    total configured power the installation is allowed to allocate

P_reserve
    watts intentionally kept unused

P_usable
    P_budget - P_reserve

P_fixed
    total power rating of FIXED_ON Loads

P_auto_available
    power passed to Best-First Search

P_auto
    total power of AUTO Loads selected by Best-First Search

P_remaining
    P_budget - (P_fixed + P_auto)

P_measured
    instantaneous voltage × current from INA219 or simulation
```

Main formulas:

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

If Best-First selects 100 W of AUTO Loads:

```text
P_auto      = 100 W
P_remaining = 200 - (80 + 100)
            = 20 W
```

## 3. Runtime target and battery energy

Battery capacity is energy storage information. It is not instantaneous power.

When a runtime target is configured:

```text
usableEnergyWh =
    batteryCapacityAh × nominalVoltageVolts ×
    max(0, SoC - minimumSoC) / 100

P_runtime = usableEnergyWh / remainingRuntimeHours

planningAllowance = min(P_usable, P_runtime)
```

Example:

```text
Battery capacity    = 200 Ah
Nominal voltage     = 15 V
SoC                 = 70%
Minimum SoC reserve = 20%
Runtime target      = 24 h
```

Then:

```text
usableEnergyWh = 200 × 15 × 0.50
               = 1500 Wh

P_runtime = 1500 / 24
          = 62.5 W
```

If `P_fixed = 40 W`:

```text
P_auto_available = 62.5 - 40
                 = 22.5 W
```

If fixed demand already exceeds the runtime allowance, `P_auto_available` becomes 0 and the system reports that the requested runtime is not currently achievable. It does not pretend that this is physical electrical protection.

## 4. INA219 and simulation

INA219 and simulation are not two different planning systems.

They are two sources for the same measurement input:

```text
if simulation:
    voltage/current come from simulated values
else:
    voltage/current come from INA219

P_measured = voltage × current
```

After that, the same Central planning code runs.

Example:

```text
Voltage = 15 V
Current = 1.5 A

P_measured = 22.5 W
```

`P_measured` tells you what is flowing at that moment. It does not replace a configured `P_budget = 200 W`.

The system can publish a warning if:

```text
P_measured > P_budget
```

This is a monitoring warning only. It does not identify the physical cause and does not claim to provide hardware protection.

## 5. Load modes

Supported modes:

- `FIXED_ON`
- `FIXED_OFF`
- `AUTO_ON`
- `AUTO_OFF`

FIXED_ON Loads are accounted before AUTO planning:

```text
P_fixed = sum of FIXED_ON power ratings
```

Best-First Search receives:

```text
P_auto_available
```

The Best-First Search algorithm itself is unchanged by the power-flow cleanup.

Each Load has a configured `powerRatingWatts`. This is the expected operating demand used for planning; it is not a live per-Load current measurement.

## 6. First setup: Wi-Fi and MQTT

MQTT requires Wi-Fi and broker credentials first.

```text
wifi status
wifi scan
wifi setup
wifi set ssid=HOME_WIFI password=PASSWORD
wifi clear

mqtt status
mqtt set host=BROKER port=1883 tls=off [username=USER] [password=PASSWORD]
mqtt clear
```

After changing Wi-Fi or MQTT credentials, reboot Central.

## 7. Serial console

Open the Central monitor:

```bash
pio device monitor -e central
```

Useful commands:

```text
status
dashboard
battery
battery status
nodes
loads
optimize
```

Use `<command> help` for command-specific syntax.

## 8. Configure the power plan

Example:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=24
```

Meaning:

```text
budget=200
    P_budget = 200 W

reserve=20
    P_reserve = 20 W

min_soc=20
    keep 20% battery SoC outside the runtime energy calculation

runtime_hours=24
    target 24 hours of remaining operation
```

To disable runtime-based allocation:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=0
```

The configured `P_budget` remains the installation power allocation. The runtime calculation can reduce the amount made available to AUTO Loads when the requested runtime requires it.

## 9. Configure the battery/INA219

Example:

```text
battery configure shunt_ohms=0.005 max_sensor_amps=40 ema_alpha=0.2 capacity_ah=200 initial_soc=70 nominal_voltage=15
```

Fields:

| Field | Meaning |
|---|---|
| `shunt_ohms` | INA219 shunt resistance |
| `max_sensor_amps` | Expected measurement range used to validate the INA219/shunt configuration |
| `ema_alpha` | Measurement smoothing factor |
| `capacity_ah` | Battery capacity used for SoC/runtime energy calculations |
| `initial_soc` | Initial SoC estimate |
| `nominal_voltage` | Battery nominal voltage used for energy calculations |

`max_sensor_amps` is not a software current-protection limit. It is used to validate whether the configured shunt/current range fits the INA219 measurement range.

For the INA219 ±320 mV shunt range:

```text
shunt_ohms × max_sensor_amps <= 0.32 V
```

## 10. Simulation

Select simulation:

```text
sensor sim
```

or:

```text
simulation start
```

Feed voltage/current:

```text
simulation values voltage=15 current=1.5
```

Feed SoC:

```text
simulation values soc=70
```

Or supply both:

```text
simulation values voltage=15 current=1.5 soc=70
```

Return to INA219:

```text
sensor ina219
```

or:

```text
simulation stop
```

Simulation voltage and current use the same calculation as the real measurement path:

```text
P_measured = voltage × current
```

Simulation does not create another Best-First Search or another power-budget formula.

## 11. Configure Loads

Add a Central-local Load:

```text
load add pin=16 name=Lamp power=10 priority=5 type=DC active_high=off mode=AUTO_OFF schedule=none
```

Add a Smart Node Load:

```text
load add mac=AA:BB:CC:DD:EE:FF pin=4 name=Fan power=25 priority=10 type=DC active_high=off mode=AUTO_OFF schedule=18:30-20:00
```

Change an existing Load:

```text
load set AA:BB:CC:DD:EE:FF 4 priority=20
load set AA:BB:CC:DD:EE:FF 4 mode=AUTO_ON schedule=19:00-21:00
```

Remove a Load:

```text
load remove AA:BB:CC:DD:EE:FF 4
```

Inspect Loads:

```text
loads
load show 16
load show AA:BB:CC:DD:EE:FF 4
```

## 12. Node commands

```text
nodes
node show AA:BB:CC:DD:EE:FF
node commission AA:BB:CC:DD:EE:FF name=Kitchen
node rename AA:BB:CC:DD:EE:FF name=Kitchen2
node decommission AA:BB:CC:DD:EE:FF
```

## 13. Optimizer

Run immediately:

```text
optimize
```

or:

```text
optimize run
```

Check interval:

```text
optimize status
```

Change interval:

```text
optimize interval seconds=30
```

Best-First receives `P_auto_available` and chooses the AUTO combination that fits within that value using the existing algorithm.

## 14. Dashboard values

The dashboard reports the same power names used internally:

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

Example:

```text
P_budget          : 200.00 W
P_reserve         : 20.00 W
P_usable          : 180.00 W
P_fixed           : 80.00 W
P_auto_available  : 100.00 W
P_auto            : 70.00 W
P_remaining       : 50.00 W
P_measured        : 153.00 W
```

## 15. MQTT schema 4

Default topic namespace:

```text
kilowatts/v1
```

Important topics:

| Topic | Purpose |
|---|---|
| `state/system` | Battery measurement and clean power-flow state |
| `state/loads` | Load state/configuration |
| `state/nodes` | Node state |
| `commands/config` | Node, Load, battery and power-planning configuration |
| `commands/load` | Existing Load changes |
| `commands/system` | System commands |
| `commands/simulation` | Simulation input |
| `alerts` | Transition warnings/information |
| `acks` | Command results |

### Configure power planning through MQTT

```json
{
  "commandId": 1001,
  "action": "CONFIGURE_POWER_PLANNING",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "powerPlanning": {
    "P_budget": 200.0,
    "P_reserve": 20.0,
    "minimumStateOfChargePercent": 20.0,
    "requiredRuntimeHours": 24.0
  }
}
```

### Simulate through MQTT

Enable:

```json
{
  "commandId": 2001,
  "action": "ENABLE"
}
```

Set values:

```json
{
  "commandId": 2002,
  "action": "SET_VALUES",
  "values": {
    "batteryVoltageVolts": 15.0,
    "batteryCurrentAmps": 1.5,
    "stateOfChargePercent": 70.0
  }
}
```

The full MQTT field reference is in `lib/MqttManager/README.md`.

## 16. MQTT monitoring alerts

Current relevant alerts include:

- `BATTERY_RESERVE`
- `RUNTIME_TARGET`
- `BATTERY_SENSOR`
- `MEASURED_POWER_BUDGET`
- `NODE_OFFLINE`

`MEASURED_POWER_BUDGET` means the measured instantaneous power crossed above or back below configured `P_budget`.

It is a monitoring condition, not a claim that the software electrically protects the installation.

## 17. Reset

Reboot Central without erasing configuration:

```text
system reset
```

Factory reset Central:

```text
system factory-reset confirm=RESET
```

Factory reset a Smart Node:

```text
system factory-reset mac=AA:BB:CC:DD:EE:FF confirm=RESET
```

Factory reset erases persisted configuration.

## 18. Hardware-protection boundary

Kilowatts firmware performs:

- power planning,
- voltage/current/power measurement,
- SoC and energy estimation,
- runtime calculations,
- Best-First load allocation,
- monitoring and alerts,
- GPIO/relay commands.

Actual electrical protection should be provided by correctly rated physical devices such as:

- fuses,
- circuit breakers,
- BMS current/voltage cut-offs,
- thermal protection,
- other installation-specific protection hardware.

A software comparison such as `P_measured > P_budget` is useful monitoring, but it is not a substitute for those devices.
