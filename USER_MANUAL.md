# Kilowatts — Current User Manual

This manual describes the current firmware in this ZIP. It covers the Central serial console and MQTT interface as they exist now.

Kilowatts is a battery-aware load-control system. The firmware decides which configured Loads should be requested ON or OFF, then writes the configured GPIO/control pin locally or sends the GPIO command to a Smart Node over ESP-NOW. The firmware does **not** verify what happens after the GPIO pin. It does not know whether a relay, transistor, MOSFET, contactor, appliance, or nothing is connected after that pin unless a separate feedback mechanism is added in a future architecture.

## 1. Devices

- **Central** (`pio run -e central`) owns the battery measurement, Wi-Fi, MQTT, node registry, load planning, and BestFirstSearch runtime decisions. Central is also a Node and may own local Loads.
- **Smart Node** (`pio run -e smart`) owns locally connected GPIO-controlled Loads and receives configuration/control commands from Central through ESP-NOW.

Build/upload examples:

```bash
pio run -e central -t upload
pio run -e smart -t upload
```

## 2. Core model

### Load

A Load contains configuration/planning data:

- Node MAC / Node identity
- `relayPin` protocol field, meaning the configured GPIO/control pin
- `relayActiveHigh` GPIO polarity
- `name`
- `powerRatingWatts`
- `powerType` (`AC` or `DC`)
- `priority`
- `mode`
- optional `schedule`

`powerRatingWatts` means the configured expected operating power consumed by that Load while it is ON. It is entered during installation. It is not a live per-Load sensor reading.

Each Node (Central or a Smart Node) supports up to 16 Loads.

Deleted/unsupported Load fields are not part of the current model: `startupWatts`, per-Load nominal voltage, per-Load nominal current, target relay state, applied state, confirmed state, or physical relay feedback.

### Modes

The supported modes are:

- `FIXED_ON` — user-configured ON boundary condition; not a BestFirstSearch candidate.
- `FIXED_OFF` — user-configured OFF boundary condition; not a BestFirstSearch candidate.
- `AUTO_ON` — automatic candidate with an ON default/preference.
- `AUTO_OFF` — automatic candidate with an OFF default/preference.

BestFirstSearch selects among AUTO Loads only. It must not rewrite the configured mode.

### Battery/runtime objective

Battery values may come from the real measurement path or the simulator. The downstream runtime/power-budget logic is the same.

The runtime budget is based on usable battery energy:

```text
fullBatteryEnergyWh = batteryCapacityAh * nominalVoltageVolts
usableSoCFraction = max(0, currentSoCPercent - reserveSoCPercent) / 100
usableEnergyWh = fullBatteryEnergyWh * usableSoCFraction
sustainableTotalPowerWatts = usableEnergyWh / remainingRequiredRuntimeHours
runtimeAutoBudgetWatts = max(0, sustainableTotalPowerWatts - fixedOnPowerWatts)
```

The final AUTO budget also respects the current immediate electrical limits. FIXED_ON Load power is deducted before AUTO Loads are planned. If FIXED_ON Loads alone exceed the sustainable power for the required runtime, the AUTO budget becomes zero and the runtime target is reported as not currently achievable; fixed modes are not secretly changed.

## 3. First setup: Wi-Fi and MQTT broker

MQTT cannot be used until Central has Wi-Fi and broker credentials. Use the console/captive portal first.

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

## 4. Serial console commands

Open the Central serial monitor at 115200 baud.

```bash
pio device monitor -e central
```

### Interactive console tips

- **Prompt & help:** The console prompt is `kilowatts >`. Type `help` to list top-level commands or `<command> help` to show usage for a specific command (for example `load help`).
- **Case and syntax:** Commands are matched case-sensitively; use the lower-case command names shown in this manual (`wifi`, `load`, `node`, etc.). Options use `name=value` with no spaces around `=` (for example `ssid=HOME_WIFI` or `pin=16`).
- **MAC and schedule formats:** MAC addresses use `AA:BB:CC:DD:EE:FF`. Schedules use `HH:MM-HH:MM` or `none`.
- **Navigation & completion:** Use the UP/DOWN arrow keys for history and press TAB to autocomplete command names.
- **Combined upload + monitor:** You can upload firmware and immediately open the monitor in one step:

```bash
pio run -e central -t upload -t monitor
```

### Example interactive session

Here is a short example of a monitor session showing `wifi scan` and `status`:

```text
kilowatts >  wifi scan
WI-FI NETWORKS DETECTED
No.  SSID                             Signal   Channel
1    HOME_WIFI                        -52      7
2    NEIGHBOR_WIFI                    -78      11
[ OK ] scan completed
kilowatts >  status
SYSTEM STATUS
Wi-Fi       : DISCONNECTED
MQTT        : DISCONNECTED
Smart Nodes : 1 / 1 online
Battery     : NOT AVAILABLE
Power budget: NOT AVAILABLE
Control errors: 0
```

Running `wifi scan` while already connected shows the connected network only, instead of scanning (a real scan would retune the shared radio and interrupt ESP-NOW):

```text
kilowatts >  wifi scan
Wi-Fi scan skipped: scanning would retune the shared radio and interrupt ESP-NOW to every Smart Node. Showing the connected network only.
CONNECTED WI-FI NETWORK
No.  SSID                             Signal   Channel
1    HOME_WIFI                        -52      7
[ OK ] scan completed
```

These examples demonstrate real console interactions; use `help` or `<command> help` for exact option names and required fields.


### Status and dashboard

```text
status
dashboard
battery
battery status
nodes
node show MAC
loads
load show PIN
load show MAC PIN
optimize
```

`load show PIN` uses Central's local MAC and is for Central-local Loads. `load show MAC PIN` can inspect a Load on any known Node.

### Node commands

```text
node commission MAC name=NAME
node rename MAC name=NAME
node decommission MAC
node show MAC
```

The console and MQTT use the same Central handler for these operations.

### Load add/config/remove

Add a Central-local Load by omitting `mac=`:

```text
load add pin=16 name=Lamp power=10 priority=5 type=DC active_high=off mode=AUTO_OFF schedule=none
```

Add or replace a Load on a Smart Node by providing its MAC:

```text
load add mac=AA:BB:CC:DD:EE:FF pin=4 name=Fan power=25 priority=10 type=DC active_high=off mode=AUTO_OFF schedule=18:30-20:00
```

Change an existing Load's priority, mode, and/or schedule:

```text
load set AA:BB:CC:DD:EE:FF 4 priority=20
load set AA:BB:CC:DD:EE:FF 4 mode=AUTO_ON schedule=19:00-21:00
load set AA:BB:CC:DD:EE:FF 4 schedule=none
```

Remove a Load:

```text
load remove AA:BB:CC:DD:EE:FF 4
```

Current fields:

| Field | Meaning |
|---|---|
| `mac=` | Optional for `load add`. Omit it to configure a Central-local Load. Provide it for a Smart Node Load. |
| `pin=` / `relayPin` | Configured GPIO/control pin field. The name remains `relayPin` in the protocol but does not imply physical relay feedback. |
| `active_high=` / `relayActiveHigh` | `on/true` means GPIO HIGH requests logical ON. `off/false` means GPIO LOW requests logical ON. |
| `power=` / `powerRatingWatts` | Expected operating watts when the Load is ON. |
| `type=` / `powerType` | `AC` or `DC`. |
| `priority=` | Higher priority means the AUTO Load is preferred when not all AUTO Loads fit the budget. |
| `mode=` | `FIXED_ON`, `FIXED_OFF`, `AUTO_ON`, or `AUTO_OFF`. |
| `schedule=` | Active window in `HH:MM-HH:MM` form, or `none`. Start and end must differ; windows may cross midnight. |

### Measurement source and simulation

```text
sensor sim
sensor ina219

simulation start
simulation stop
simulation values voltage=12.4 current=3.0
simulation values soc=80
simulation values voltage=12.4 current=3.0 soc=80
```

`sensor sim` works immediately — selecting the simulated measurement source does
not require `battery configure`/`battery limits`, and the firmware never invents
a shunt resistance, battery capacity, voltage, or site policy. Simulation mode
does not read the INA219 calibration fields because there is no physical sensor.
However, source selection by itself is not a complete sizing configuration:
meaningful current-limit decisions require `battery limits`, and the
energy/runtime formula requires the intended battery capacity and nominal
voltage stored by `battery configure`. The exact QA reproduction values are in
`TEST_REPRODUCTION_GUIDE.md`.

Simulation is only a test input source for battery measurements/SoC — the exact same downstream planning code runs either way (§2 "Battery/runtime objective"). It is not a second planner and does not validate physical hardware. This is also the intended way to size an installation: before ever touching a real INA219, use simulation to feed the loads you expect and observe what the battery/runtime budget can actually sustain, and use that to decide the real `battery limits` reserve/runtime values (see `INSTALLATION_GUIDE.md`).

### Battery sensor and power limits

`battery limits` is the installation's reserve/runtime **policy**. It is never
defaulted by the firmware:

```text
battery limits min_soc=20 max_discharge_amps=40 max_main_amps=40 runtime_hours=8
```

`runtime_hours=0` or omitting `runtime_hours` disables the required-runtime target.

`battery configure` stores both the real INA219 calibration and the battery
energy profile used by the runtime calculation:

```text
battery configure shunt_ohms=0.005 max_sensor_amps=40 ema_alpha=0.2 capacity_ah=100 initial_soc=80 nominal_voltage=12
```

Every value here belongs to the intended installation; the firmware does not
ship with or fall back to a default shunt/capacity/voltage. Simulation does not
read the shunt calibration, but it does use `capacity_ah` and
`nominal_voltage` when a required-runtime target is active. Before enabling
hardware mode, replace any provisional sizing values with the actual battery
and sensor specifications. `shunt_ohms × max_sensor_amps` must not exceed 0.32
(the INA219's ±320 mV shunt-voltage measurement range); an out-of-range pair is
rejected.

The battery interface tracks voltage, current, power, SoC, capacity, nominal voltage, reserve SoC, discharge/main current limits, and required runtime. It does not report charger states such as charging, charged, or full.

### System reset

Reboot Central without touching any persisted configuration:

```text
system reset
```

Factory-reset Central or a Smart Node — this erases persisted configuration:

```text
system factory-reset confirm=RESET
system factory-reset mac=AA:BB:CC:DD:EE:FF confirm=RESET
```

WARNING: `system factory-reset` erases persisted configuration on the target device. Use `system reset` when you only want to reboot.

To request a non-destructive control action (optimizer run) use the `optimize` command instead.

### Smart Node console

A Smart Node has its own minimal serial console, independent of Central — useful for bench setup or diagnosing a node without going through ESP-NOW/MQTT at all.

```bash
pio device monitor -e smart
```

```text
status
loads
load show PIN
load add pin=PIN name=NAME power=WATTS priority=0..10 type=AC|DC active_high=on|off mode=FIXED_OFF|FIXED_ON|AUTO_OFF|AUTO_ON schedule=HH:MM-HH:MM|none
load remove PIN
load set PIN priority=0..10 mode=... schedule=...
```

A Smart Node's own Load is always local, so these commands never take a MAC — unlike Central's console. `load set` changes priority/mode/schedule in place without touching the pin, name, power rating, or polarity. Whatever is configured here reaches Central on its next ESP-NOW report cycle.

## 5. MQTT interface

Default namespace:

```text
kilowatts/v1
```

Topics:

| Topic | Direction | Purpose |
|---|---|---|
| `kilowatts/v1/status` | publish | Availability: retained `online`, Last-Will `offline`. |
| `kilowatts/v1/state/system` | publish | Battery, runtime budget, power-flow, connectivity, optimization snapshot. |
| `kilowatts/v1/state/tree` | publish | Topology/Node tree. |
| `kilowatts/v1/state/loads` | publish | Configured Load inventory/status data. No fake physical confirmation state. |
| `kilowatts/v1/state/nodes` | publish | Node identity/status data. |
| `kilowatts/v1/config/nodes` | publish | Commissioned Node configuration mirror. |
| `kilowatts/v1/events` | publish | Discovery, offline/recovery, and runtime events. |
| `kilowatts/v1/alerts` | publish | Fault-condition transitions — pushed once when a condition changes, not repeated every cycle. |
| `kilowatts/v1/acks` | publish | Command acknowledgements. |
| `kilowatts/v1/commands/config` | subscribe | Node, Load, battery sensor, and power-limit configuration. |
| `kilowatts/v1/commands/load` | subscribe | Existing Load priority/mode/schedule updates. |
| `kilowatts/v1/commands/system` | subscribe | Optimization request and factory reset. |
| `kilowatts/v1/commands/simulation` | subscribe | Simulation enable/disable/set-values. |

Every MQTT command needs a `commandId`. Full field-by-field payload shapes for every topic are in `lib/MqttManager/README.md` — this section only shows representative examples.

### Alerts

Unlike the retained `state/*` topics (which only change value on the next periodic publish), `alerts` is pushed the moment a condition actually changes:

```json
{"schemaVersion":3,"alertType":"BATTERY_RESERVE","severity":"critical","detail":"battery state of charge reached the configured reserve"}
```

`alertType` is one of `BATTERY_RESERVE`, `RUNTIME_TARGET`, `BATTERY_SENSOR` (real INA219 stops/resumes responding), `NODE_OFFLINE` (a Smart Node's online status flips). Each fires once per transition in both directions (e.g. reserve reached, then again when recovered) — see `lib/MqttManager/README.md` for the full table.

### Node config examples

Commission:

```bash
mosquitto_pub -h <broker> -t 'kilowatts/v1/commands/config' -m '{
  "commandId": 1,
  "action": "COMMISSION_NODE",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "friendlyName": "Kitchen"
}'
```

Rename:

```json
{"commandId":2,"action":"RENAME_NODE","nodeMac":"AA:BB:CC:DD:EE:FF","friendlyName":"Kitchen 2"}
```

Decommission:

```json
{"commandId":3,"action":"DECOMMISSION_NODE","nodeMac":"AA:BB:CC:DD:EE:FF"}
```

### Load config examples

Configure a Load:

```json
{
  "commandId": 10,
  "action": "CONFIGURE_LOAD",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "load": {
    "name": "Fan",
    "relayPin": 4,
    "relayActiveHigh": false,
    "mode": "AUTO_OFF",
    "priority": 10,
    "powerType": "DC",
    "powerRatingWatts": 25.0,
    "schedule": null
  }
}
```

Remove a Load:

```json
{"commandId":11,"action":"REMOVE_LOAD","nodeMac":"AA:BB:CC:DD:EE:FF","relayPin":4}
```

Update priority/mode/schedule on an existing Load:

```json
{
  "commandId": 12,
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "relayPin": 4,
  "priority": 20,
  "mode": "AUTO_ON",
  "schedule": {"enabled": true, "hour": 18, "minute": 30}
}
```

`schedule` may also be `null` to disable scheduling.

### Battery config examples

Configure battery sensor/profile:

```json
{
  "commandId": 20,
  "action": "CONFIGURE_BATTERY_SENSOR",
  "nodeMac": "<CENTRAL_MAC>",
  "batterySensor": {
    "shuntResistanceOhms": 0.005,
    "maximumExpectedCurrentAmps": 40.0,
    "emaAlpha": 0.2,
    "batteryCapacityAmpHours": 100.0,
    "initialStateOfChargePercent": 80.0,
    "nominalVoltageVolts": 12.0
  }
}
```

Configure power/runtime limits:

```json
{
  "commandId": 21,
  "action": "CONFIGURE_POWER_LIMITS",
  "nodeMac": "<CENTRAL_MAC>",
  "powerLimits": {
    "minimumStateOfChargePercent": 20.0,
    "maximumBatteryDischargeCurrentAmps": 40.0,
    "maximumMainCurrentAmps": 40.0,
    "requiredRuntimeHours": 8.0
  }
}
```

### Simulation examples

Enable simulation:

```json
{"commandId":30,"action":"ENABLE"}
```

Set values:

```json
{
  "commandId": 31,
  "action": "SET_VALUES",
  "values": {
    "batteryVoltageVolts": 12.4,
    "batteryCurrentAmps": 3.0,
    "stateOfChargePercent": 80.0
  }
}
```

Disable simulation:

```json
{"commandId":32,"action":"DISABLE"}
```

Publish these to:

```text
kilowatts/v1/commands/simulation
```

### System commands

```json
{"commandId":40,"action":"REQUEST_OPTIMIZATION_CYCLE"}
{"commandId":41,"action":"FACTORY_RESET_NODE","targetNodeMac":"AA:BB:CC:DD:EE:FF","confirm":"FACTORY_RESET_CONFIRMED"}
{"commandId":42,"action":"FACTORY_RESET_CENTRAL","confirm":"FACTORY_RESET_CONFIRMED"}
{"commandId":43,"action":"REBOOT_CENTRAL"}
```

## 6. Command acknowledgement meaning

`acks` reports whether Central accepted/rejected a command and whether the operation completed locally or through the expected ESP-NOW command path.

For a Smart Node GPIO command, an ACK only means the Smart Node processed the command at firmware level. It does not prove the downstream device switched or consumed power.

## 7. Build and host tests

Firmware builds:

```bash
pio run -e central
pio run -e smart
```

Host-native behavioral tests:

```bash
./test/run_tests.sh
```

The host tests exercise the pure calculation/planning behavior without ESP32 hardware. They do not validate physical INA219, GPIO, relays, ESP-NOW radio, or downstream appliances.

## 8. Current known limits

- The firmware uses the protocol field name `relayPin`, but the architectural meaning is GPIO/control pin.
- There is no per-Load live power sensor in the current model. The planner uses configured `powerRatingWatts`.
- There is no physical downstream-device feedback mechanism.
- Wi-Fi and MQTT broker credential bootstrap remains console-side because MQTT cannot be used until networking and broker settings are configured.

See also: `lib/MqttManager/README.md` (complete MQTT API field reference), `INSTALLATION_GUIDE.md` (site survey and sizing process), `TECHNICAL_REFERENCE.md` (formulas), `FAQ.md`, `FUTURE_WORK.md`, `LIMITATIONS.md`.
