# Installation Guide

Kilowatts setup is separated into three clear parts:

```text
sensor setup   INA219 measurement settings
battery setup  battery capacity, nominal voltage and starting SoC
battery plan   allocation policy
```

## 1. Configure INA219 when using real hardware

```text
sensor setup shunt=R max_amps=A ema=X
sensor ina219
battery
```

Use the actual values for your INA219/shunt installation.

`max_amps` describes the expected INA219/shunt measurement range. It is not a software protection limit.

The `battery` command reports whether INA219 is configured and whether it is detected. If the sensor has not been configured, it reports `NOT CONFIGURED`; if configuration exists but the device does not respond on I2C, it reports `NOT DETECTED`.

Simulation does not require this INA219 setup.

## 2. Configure battery metadata

For SoC and runtime calculations, configure the actual battery information:

```text
battery setup capacity=AH voltage=V soc=PERCENT
```

Where:

```text
capacity  battery capacity in amp-hours
voltage   nominal/nameplate battery voltage
soc       starting state of charge
```

INA219 does not directly measure SoC. With real INA219 input, the starting SoC and capacity are used with measured current over time to estimate SoC. In simulation, SoC can be supplied directly with `sensor values`.

## 3. Configure the power plan

```text
battery plan budget=W reserve=W min_soc=PERCENT
```

Example:

```text
battery plan budget=200 reserve=20 min_soc=20
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

Without runtime:

```text
P_auto_available = max(0, P_budget - P_reserve - P_fixed)
```

Best-First receives only `P_auto_available`.

After Best-First selects AUTO loads:

```text
P_remaining = max(0, P_budget - (P_fixed + P_auto))
```

## 4. Optional runtime target

Add `runtime=H` only when you want the planner to try to make available battery energy last for that many hours:

```text
battery plan budget=200 reserve=20 min_soc=20 runtime=4
```

Runtime requires battery setup and a valid SoC.

Internally:

```text
E_usable = capacityAh × nominalVoltage × max(0, SoC - min_soc) / 100
runtime allowance = E_usable / runtimeHours
P_auto_available = max(0, min(P_budget - P_reserve, runtime allowance) - P_fixed)
```

A longer runtime target can reduce `P_auto_available`, so Best-First may select fewer AUTO loads. FIXED_ON loads remain ON. If FIXED_ON demand already exceeds the runtime allowance, AUTO receives 0 W and the runtime target is reported as not achievable.

To remove the runtime target, configure the plan without `runtime`:

```text
battery plan budget=200 reserve=20 min_soc=20
```

## 5. Test with simulation only

No INA219 setup is required.

Example:

```text
sensor sim
battery setup capacity=10 voltage=12 soc=80
battery plan budget=200 reserve=20 min_soc=20
sensor values voltage=12 current=5 soc=80
optimize
battery
dashboard
```

The system calculates:

```text
P_measured = 12 × 5 = 60 W
```

To test runtime using the same simulation:

```text
battery plan budget=200 reserve=20 min_soc=20 runtime=4
optimize
dashboard
```

## 6. Switch to real INA219

After the INA219 has been configured and wired:

```text
sensor ina219
battery
optimize
dashboard
```

INA219 supplies voltage/current to the same planning and monitoring path used by simulation.

## 7. Configure Central networking

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

## 8. Configure loads

Central load example:

```text
load add pin=16 name=Lamp power=10 priority=5 type=DC active_high=off mode=AUTO_OFF schedule=none
```

Smart Node load example:

```text
load add mac=AA:BB:CC:DD:EE:FF pin=4 name=Fan power=25 priority=10 type=DC active_high=off mode=AUTO_OFF schedule=18:30-20:00
```

Use realistic `power=` values because planning uses each load's configured rating.

## 9. Check operation

```text
status
battery
nodes
loads
optimize
dashboard
```

Each planning cycle should:

1. sum `FIXED_ON` load ratings into `P_fixed`;
2. calculate `P_auto_available`;
3. pass only `P_auto_available` to Best-First;
4. command selected AUTO loads ON and unselected AUTO loads OFF;
5. calculate `P_auto` and `P_remaining`;
6. measure actual instantaneous consumption as `P_measured`.

## 10. Frontend MQTT

Only five external topics are used:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

Use `status` for Central `online`/`offline` presence.

Use `state` for system, loads and nodes. Wi-Fi and broker credentials remain local to Central.

Use `command` only for:

```text
node
load
battery
sensor
system
```

## 11. Hardware check

Before using a real installation, verify:

- correct `P_budget` and `P_reserve`;
- correct FIXED/AUTO modes and load watt ratings;
- real INA219 wiring and shunt rating;
- correct battery capacity, nominal voltage and starting SoC;
- relay operation;
- correctly rated physical fuses, breakers, BMS, and other required protection hardware.

Kilowatts software plans, monitors, and commands loads. It does not replace electrical protection hardware.
