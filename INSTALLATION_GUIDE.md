# Installation Guide

## 1. Configure the power plan

Example:

```text
battery set budget=200 reserve=20 min_soc=20 runtime=24
```

The public power values are exactly:

```text
P_budget
P_reserve
P_fixed
P_auto_available
P_auto
P_remaining
P_measured
```

If:

```text
P_budget = 200 W
P_reserve = 20 W
P_fixed = 80 W
```

and runtime does not reduce the AUTO allowance:

```text
P_auto_available = 200 - 20 - 80 = 100 W
```

Best-First receives only `P_auto_available`.

If Best-First selects AUTO loads totaling 70 W:

```text
P_auto = 70 W
P_remaining = 200 - (80 + 70) = 50 W
```

## 2. Runtime

When `runtime` is greater than zero, battery energy and SoC are used internally to reduce `P_auto_available` when necessary.

To disable runtime planning:

```text
battery set budget=200 reserve=20 min_soc=20 runtime=0
```

## 3. Configure INA219/battery measurement

Technical sensor setup stays on the Central console:

```text
sensor set shunt=0.005 max_amps=40 ema=0.2 capacity=200 soc=70 voltage=15
```

`max_amps` describes the expected INA219/shunt measurement range. It is not a software protection limit.

## 4. Test with simulation

```text
sensor sim
sensor values voltage=15 current=1.5 soc=70
```

The system calculates:

```text
P_measured = 15 × 1.5 = 22.5 W
```

Then run:

```text
dashboard
optimize
```

## 5. Switch to INA219

```text
sensor ina219
battery
dashboard
```

INA219 now supplies voltage/current to the same path used by simulation.

## 6. Configure Central networking

Wi-Fi and MQTT broker setup are local installation settings. They are not frontend MQTT commands.

Wi-Fi:

```text
wifi
wifi scan
wifi setup
wifi set ssid=MyWiFi password=MyPassword
wifi clear
```

Use `wifi scan` to confirm which networks Central can see, their signal strength, and their current channel.

The shared Wi-Fi/ESP-NOW channel is automatic. `wifi set` detects and stores the configured Access Point's channel. If the Access Point later changes channel, Central detects the new channel and restarts automatically with Wi-Fi and ESP-NOW aligned to it. Smart Nodes search channels for Central if normal discovery fails. There is no manual channel configuration command.

MQTT broker:

```text
mqtt
mqtt set host=192.168.1.10
mqtt clear
```

Optional MQTT settings include `port=`, `tls=on|off`, `username=` and `password=`.

## 7. Configure loads

Central load example:

```text
load add pin=16 name=Lamp power=10 priority=5 type=DC active_high=off mode=AUTO_OFF schedule=none
```

Smart Node load example:

```text
load add mac=AA:BB:CC:DD:EE:FF pin=4 name=Fan power=25 priority=10 type=DC active_high=off mode=AUTO_OFF schedule=18:30-20:00
```

Use realistic `power=` values because planning uses each load's configured rating.

## 8. Check operation

```text
status
nodes
loads
optimize
dashboard
```

Each planning cycle should:

1. sum `FIXED_ON` load ratings into `P_fixed`;
2. calculate `P_auto_available`;
3. pass `P_auto_available` to Best-First;
4. command selected AUTO loads ON and unselected AUTO loads OFF;
5. calculate `P_auto` and `P_remaining`;
6. measure actual instantaneous consumption as `P_measured`.

## 9. Frontend MQTT

Only five external topics are used:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

Use `status` for Central `online`/`offline` presence.

Use `state` for system, loads and nodes. The node state includes Central/Smart Node chip and diagnostic information. Wi-Fi and broker details are not published to the frontend.

Use `command` only for:

```text
node
load
battery
sensor
system
```

## 10. Hardware check

Before using a real installation, verify:

- correct `P_budget` and `P_reserve`;
- correct FIXED/AUTO modes and load watt ratings;
- real INA219 wiring and shunt rating;
- relay operation;
- correctly rated physical fuses, breakers, BMS, and other required protection hardware.

Kilowatts software plans, monitors, and commands loads. It does not replace electrical protection hardware.
