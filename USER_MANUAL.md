# Kilowatts — User Manual

Kilowatts is a load-allocation and power-monitoring system. Central accounts for FIXED loads, gives the remaining AUTO allowance to the existing Best-First Search, and commands relays. INA219 or simulation supplies voltage/current into the same measurement path.

The software does not replace fuses, breakers, BMS cut-offs or other electrical protection hardware.

## 1. Power model

The canonical values are:

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

Main formulas:

```text
P_usable = max(0, P_budget - P_reserve)

P_auto_available = max(0, planningAllowance - P_fixed)

P_remaining = max(0, P_budget - (P_fixed + P_auto))

P_measured = voltage * current
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

If Best-First selects 100 W of AUTO loads:

```text
P_auto      = 100 W
P_remaining = 200 - (80 + 100) = 20 W
```

## 2. Runtime target

Battery capacity is energy, not instantaneous power.

When runtime is configured:

```text
usableEnergyWh =
    capacityAh * nominalVoltage *
    max(0, SoC - minimumSoC) / 100

P_runtime = usableEnergyWh / remainingRuntimeHours

planningAllowance = min(P_usable, P_runtime)
```

Example:

```text
200 Ah * 15 V * (70% - 20%) = 1500 Wh
1500 Wh / 24 h = 62.5 W
```

If `P_fixed = 40 W`, then:

```text
P_auto_available = 62.5 - 40 = 22.5 W
```

If fixed demand already exceeds the runtime allowance, AUTO allocation becomes 0 and the runtime target is reported as not achievable.

## 3. INA219 and simulation

There is one measurement path.

```text
if simulation:
    use simulated voltage/current
else:
    use INA219 voltage/current

P_measured = voltage * current
```

`P_measured` is actual/simulated instantaneous consumption. It does not replace `P_budget`.

The firmware can warn when:

```text
P_measured > P_budget
```

That is monitoring only, not hardware protection.

## 4. Console

Open the Central monitor:

```bash
pio device monitor -e central
```

The console is intentionally small:

```text
status
dashboard
battery
sensor
nodes
node
loads
load
optimize
wifi
mqtt
system
clear
```

Use `<command> help` for syntax.

### Power planning

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=24
```

Disable runtime planning with:

```text
battery planning budget=200 reserve=20 min_soc=20 runtime_hours=0
```

### Battery / INA219 configuration

```text
battery configure shunt_ohms=0.005 max_sensor_amps=40 ema_alpha=0.2 capacity_ah=200 initial_soc=70 nominal_voltage=15
```

`max_sensor_amps` describes the expected INA219/shunt measurement range. It is not a software current-protection setting.

### Select measurement input

Real INA219:

```text
sensor ina219
```

Simulation:

```text
sensor sim
```

Provide simulated values:

```text
sensor values voltage=15 current=1.5 soc=70
```

Simulation and INA219 then continue through the same PowerManager logic.

## 5. Loads

Modes:

```text
FIXED_ON
FIXED_OFF
AUTO_ON
AUTO_OFF
```

FIXED_ON power becomes `P_fixed`. Best-First receives only `P_auto_available` and selects AUTO loads using the existing algorithm.

Add a Central load:

```text
load add pin=16 name=Lamp power=10 priority=5 type=DC active_high=off mode=AUTO_OFF schedule=none
```

Add a Smart Node load:

```text
load add mac=AA:BB:CC:DD:EE:FF pin=4 name=Fan power=25 priority=10 type=DC active_high=off mode=AUTO_OFF schedule=18:30-20:00
```

Inspect or change loads:

```text
loads
load show 16
load show AA:BB:CC:DD:EE:FF 4
load set AA:BB:CC:DD:EE:FF 4 priority=20
load set AA:BB:CC:DD:EE:FF 4 mode=AUTO_ON schedule=19:00-21:00
load remove AA:BB:CC:DD:EE:FF 4
```

## 6. Nodes

```text
nodes
node show AA:BB:CC:DD:EE:FF
node commission AA:BB:CC:DD:EE:FF name=Kitchen
node rename AA:BB:CC:DD:EE:FF name=Kitchen2
node decommission AA:BB:CC:DD:EE:FF
```

## 7. Optimizer

Run one cycle:

```text
optimize
```

Check/change the interval:

```text
optimize status
optimize interval seconds=30
```

The Best-First Search algorithm itself is unchanged.

## 8. Dashboard

Run:

```text
dashboard
```

The important output is:

```text
Measurement source
Voltage
Current
P_measured
Battery SoC
P_budget
P_reserve
P_usable
P_fixed
P_auto_available
P_auto
P_remaining
P_runtime     (when active)
```

## 9. Wi-Fi and MQTT

Configure Wi-Fi:

```text
wifi status
wifi scan
wifi setup
wifi set ssid=HOME_WIFI password=PASSWORD
wifi clear
```

Configure MQTT:

```text
mqtt status
mqtt set host=BROKER port=1883 tls=off [username=USER] [password=PASSWORD]
mqtt clear
```

The external MQTT API has only five topics:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

`state` contains the useful system, load and node state in one retained message.

All commands go to `command` and include one of these types:

```text
load
system
config
simulation
```

Example power-planning command:

```json
{
  "type": "config",
  "commandId": 1001,
  "action": "CONFIGURE_POWER_PLANNING",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "powerPlanning": {
    "P_budget": 200,
    "P_reserve": 20,
    "minimumStateOfChargePercent": 20,
    "requiredRuntimeHours": 24
  }
}
```

Example simulation input:

```json
{
  "type": "simulation",
  "commandId": 1002,
  "action": "SET_VALUES",
  "values": {
    "batteryVoltageVolts": 15,
    "batteryCurrentAmps": 1.5,
    "stateOfChargePercent": 70
  }
}
```

See `lib/MqttManager/README.md` for the MQTT message reference.

## 10. Alerts

Useful alerts include:

```text
BATTERY_RESERVE
RUNTIME_TARGET
BATTERY_SENSOR
MEASURED_POWER_BUDGET
NODE_OFFLINE
```

`MEASURED_POWER_BUDGET` means measured instantaneous power crossed above or back below configured `P_budget`.

## 11. Reset

Reboot Central:

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

## 12. Hardware boundary

Kilowatts software performs measurement, SoC/energy estimation, runtime planning, Best-First allocation, monitoring and relay/GPIO commands.

Physical electrical protection remains the responsibility of appropriately rated hardware such as fuses, breakers, BMS cut-offs and thermal protection.
