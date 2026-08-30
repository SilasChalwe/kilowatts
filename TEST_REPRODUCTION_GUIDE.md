# Reproducing the Validation Tests

This guide reproduces the four measured scenarios in
`QA_REPORT_LOAD_SELECTION_SCENARIOS.md`. It uses the same eight-load
configuration and the same simulated battery inputs as the recorded hardware
session in `test-evidence/logs/`.

These are planner and firmware tests on a physical Central board. The battery
measurements are simulated, and the test does not prove the accuracy of a real
INA219, simultaneous multi-node ESP-NOW operation, or the behavior of an
appliance connected after a GPIO. Those boundaries are documented in
`LIMITATIONS.md`.

## Prerequisites

- A Central Node connected over USB at 115200 baud.
- Current Central firmware flashed on the board.
- A serial terminal at the `kilowatts >` prompt.
- Valid wall-clock time from NTP or the configured manual-time path for the
  schedule-window scenario.
- Safe GPIO wiring. Physical relays and appliances are not required to inspect
  the planner results. If relays are connected, confirm their polarity and
  electrical safety before issuing any command.

Replace `<MAC>` below with the Central Node's MAC address. It is shown at boot
and by `node show`.

## 1. Create the eight-load QA configuration

Ensure the Central Node has exactly these Loads. `load add` can be used to add
or replace an entry; remove conflicting test entries first if necessary.

```text
load add mac=<MAC> pin=4  name=Fridge         power=8  priority=1 type=AC active_high=off mode=FIXED_ON  schedule=none
load add mac=<MAC> pin=26 name=WaterHeater    power=15 priority=1 type=AC active_high=off mode=FIXED_OFF schedule=none
load add mac=<MAC> pin=5  name=Lights         power=3  priority=8 type=DC active_high=off mode=AUTO_OFF  schedule=none
load add mac=<MAC> pin=18 name=WaterPump      power=10 priority=5 type=DC active_high=off mode=AUTO_OFF  schedule=none
load add mac=<MAC> pin=19 name=Fan            power=5  priority=3 type=DC active_high=off mode=AUTO_OFF  schedule=none
load add mac=<MAC> pin=23 name=Router         power=2  priority=9 type=DC active_high=off mode=AUTO_OFF  schedule=none
load add mac=<MAC> pin=25 name=SecurityCamera power=4  priority=7 type=DC active_high=off mode=AUTO_OFF  schedule=none
load add mac=<MAC> pin=27 name=PhoneCharger   power=1  priority=2 type=DC active_high=off mode=AUTO_OFF  schedule=none
loads
```

The recorded board used active-low relay channels, hence `active_high=off`.
Use the polarity required by the actual hardware if reproducing this on a
different rig. The polarity does not change Best-First's subset calculation,
but it changes the electrical GPIO state requested for ON/OFF.

## 2. Configure the battery profile and simulation

The recorded QA values came from a 50 Ah, 12.6 V battery profile with a 20%
reserve and a 24-hour runtime target. Entering the profile is necessary to
reproduce the same energy/runtime budget. In simulation, the INA219-specific
shunt and current values are stored but no physical sensor is read.

```text
sensor sim
battery configure shunt_ohms=0.01 max_sensor_amps=20 ema_alpha=0.2 capacity_ah=50 initial_soc=75 nominal_voltage=12.6
battery limits min_soc=20 max_discharge_amps=10 max_main_amps=15 runtime_hours=24
simulation start
simulation values voltage=12.6 current=1.5 soc=75
optimize run
```

Selecting simulation first ensures that entering the battery profile does not
attempt to initialize an unavailable INA219. It does not create default
profile values; the following two commands still provide the explicit profile
and policy used by this test.

Use `dashboard`, `loads`, and `load show <PIN>` to inspect each result. The
available-power value may move slightly between cycles because the simulated
current and SoC pass through the same filtering and elapsed-time logic used by
the firmware.

## 3. Scenario 1 — baseline selection

Restore the baseline before measuring:

```text
load set <MAC> 25 priority=7 schedule=none
simulation values voltage=12.6 current=1.5 soc=75
optimize run
dashboard
loads
```

Expected result:

- Fridge remains `FIXED_ON`; WaterHeater remains `FIXED_OFF`.
- Router, Lights, and PhoneCharger are `AUTO_ON`.
- WaterPump, Fan, and SecurityCamera are `AUTO_OFF`.
- The selected AUTO power is 6 W with total configured priority 19.
- Available AUTO power is approximately 6.4 W under the recorded profile.

## 4. Scenario 2 — active schedule priority boost

Choose a non-empty schedule window that contains the Central Node's current
local time. For example, if the current local time is 14:30, use
`14:00-15:00`:

```text
load set <MAC> 25 schedule=14:00-15:00
optimize run
dashboard
loads
```

Expected result while the window is active:

- SecurityCamera receives the +5 effective-priority boost.
- Router and SecurityCamera are selected, using 6 W.
- Lights and PhoneCharger are displaced.

If the board's time is not inside `14:00-15:00`, replace it with a window that
does contain the current time. A schedule outside the current time receives no
boost; it does not make the Load ineligible.

Clean up before the next scenario:

```text
load set <MAC> 25 schedule=none
```

## 5. Scenario 3 — equal individual priorities

```text
load set <MAC> 25 priority=9
optimize run
dashboard
loads
```

Expected result:

- Router and SecurityCamera now have the same individual priority.
- Router, Lights, and PhoneCharger still win as a combination because their
  total priority is 19, versus 18 for Router plus SecurityCamera.

This scenario demonstrates that Best-First compares complete feasible
combinations, not a pair of individual Loads in isolation.

Restore SecurityCamera before continuing:

```text
load set <MAC> 25 priority=7
```

## 6. Scenario 4 — power-budget scaling

```text
simulation values voltage=12.6 current=1.5 soc=90
optimize run
dashboard
loads
```

Expected result:

- Available AUTO power rises to approximately 10.78 W.
- Router, Lights, PhoneCharger, and SecurityCamera are selected.
- Selected AUTO power is 10 W.
- WaterPump and Fan remain off because neither fits in the remaining budget.

## 7. Restore the recorded baseline

```text
load set <MAC> 25 priority=7 schedule=none
simulation values voltage=12.6 current=1.5 soc=75
optimize run
dashboard
loads
```

The result should again match Scenario 1.

## Command reference

| Command | Purpose |
|---|---|
| `loads` | List every configured Load and its current mode. |
| `load show PIN` | Show one Central-local Load in detail. |
| `load set MAC PIN priority=N schedule=HH:MM-HH:MM\|none` | Change an existing Load's priority or schedule window. |
| `sensor sim` | Select simulated battery measurements. |
| `simulation start` | Enable the simulator. |
| `simulation values voltage=V current=A soc=PERCENT` | Supply the simulated measurement. |
| `optimize run` | Force an immediate planning and dispatch cycle. |
| `dashboard` | Show the current battery and power-budget calculation. |

For full syntax, enter a command with no arguments or use its help form. See
`USER_MANUAL.md` for console operation and `TECHNICAL_REFERENCE.md` for the
formulas and ranking rules.
