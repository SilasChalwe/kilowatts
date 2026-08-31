# Kilowatts — User Manual

Kilowatts is a load-allocation and power-monitoring system. Central accounts for FIXED loads, gives `P_auto_available` to the existing Best-First Search, and commands selected AUTO loads ON or OFF. INA219 or simulation supplies voltage/current into the same measurement path.

## Power names

```text
P_budget          configured installation power limit
P_reserve         watts intentionally kept outside allocation
P_fixed           total watts of FIXED_ON loads
P_auto_available  watts passed to Best-First Search
P_auto            total watts of AUTO loads selected
P_remaining       P_budget minus FIXED + selected AUTO
P_measured        instantaneous voltage × current
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
P_auto            = 100 W
P_remaining       = 20 W
```

## Runtime

Runtime is optional. Battery capacity, nominal voltage, SoC, minimum SoC and requested runtime can reduce `P_auto_available` so the system can try to meet the requested runtime.

If FIXED loads already consume more than the runtime target allows:

```text
P_auto_available = 0 W
Runtime target = NOT ACHIEVABLE
```

## INA219 and simulation

There is one measurement path:

```text
if simulation:
    voltage/current = simulated input
else:
    voltage/current = INA219 input

P_measured = voltage × current
```

`P_measured` is actual instantaneous consumption monitoring. It does not replace `P_budget`.

## Console = installation setup

Wi-Fi and MQTT broker configuration are done only on the Central serial console. They are not frontend/MQTT commands.

Main commands:

```text
status
dashboard
battery
battery set budget=200 reserve=20 min_soc=20 runtime=24

sensor ina219
sensor sim
sensor values voltage=15 current=8 soc=70
sensor set shunt=0.005 max_amps=40 ema=0.2 capacity=200 soc=70 voltage=15

nodes
loads
optimize

wifi
wifi scan
wifi setup
wifi set ssid=MyWiFi password=MyPassword
wifi clear

mqtt
mqtt set host=192.168.1.10
mqtt clear
```

`wifi scan` lists the named Wi-Fi networks Central can currently detect, including signal strength and channel.

The shared Wi-Fi/ESP-NOW channel is automatic. `wifi set` detects the configured Access Point's current channel and saves it. If that Access Point later moves to another channel, Central detects the new channel, saves it and restarts automatically so Wi-Fi and ESP-NOW come back on the same channel. Smart Nodes search the radio channels for Central when normal discovery fails. There is no manual `wifi channel` command.

For MQTT, port and TLS are optional. Defaults are 1883 without TLS and 8883 with TLS.

`max_amps` describes the expected INA219/shunt measurement range. It is not a software hardware-protection limit.

## Frontend MQTT = operation

Only five external MQTT topics exist:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

### Central online/offline

The frontend must use:

```text
kilowatts/v1/status
```

Central publishes retained `online`. Its MQTT Last Will publishes retained `offline` if Central disappears from the broker.

Do not decide Central status from whether the browser/frontend itself is connected to MQTT.

### State

`state` contains:

```text
system
loads
nodes
```

The system section contains battery, sensor/measurement and power-flow information. Wi-Fi state and MQTT connection details are intentionally not published to the frontend.

The nodes section includes Central and Smart Node information such as:

```text
MAC address
role
name
lifecycle/sync state
firmware version
chip model
load count
relay pins
online state for Smart Nodes
hardware diagnostics
```

Diagnostics can include heap, flash, PSRAM, CPU cores/frequency, silicon revision, reset reason and temperature when available.

Central's online/offline status comes from the `status` topic; its retained node record is not used as the liveness signal.

### Frontend command types

Only these command types are accepted:

```text
node
load
battery
sensor
system
```

There is no frontend Wi-Fi or MQTT configuration command.

Node operations support add/commission, update/rename and delete/decommission.

Load operations support add, update and delete. Load update is used for mode, priority and schedule changes.

Battery commands configure the planning values such as `P_budget`, `P_reserve`, minimum SoC and runtime.

Sensor commands select INA219 or simulation and can provide simulated voltage/current/SoC values.

System commands can run optimization, change the optimization interval and restart Central.

See `lib/MqttManager/README.md` for exact JSON examples.

## What the system answers

For each planning cycle, Kilowatts answers:

1. What is the configured power budget? → `P_budget`
2. How much is reserved? → `P_reserve`
3. How much are FIXED_ON loads using? → `P_fixed`
4. How much can AUTO loads use? → `P_auto_available`
5. Which AUTO loads fit best? → existing Best-First Search
6. How much AUTO power was selected? → `P_auto`
7. How much configured power remains? → `P_remaining`
8. What is actually being consumed? → `P_measured`

Central then sends relay commands for the selected load states.

## Hardware boundary

Kilowatts performs planning, monitoring and relay/GPIO commands. Real electrical protection must come from correctly rated hardware such as fuses, breakers, BMS cut-offs and other installation-specific protection devices.
