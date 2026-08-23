# Reproducing the Validation Tests

This is a step-by-step procedure for reproducing every result in
`TESTING_AND_VALIDATION.md` on the physical Central Node. Follow the steps
in order — each test builds on the state left by the previous one, and the
exact commands used are given verbatim so the same numbers should come back
out.

## Prerequisites

- Central Node hardware connected over USB (in this session: `/dev/ttyUSB1`,
  115200 baud).
- Four relay channels wired to GPIO 2, 4, 18, 25, each an **active-low**
  board (energises when the GPIO is driven LOW).
- Four Loads already configured on those pins (10 W, DC, one per pin). If
  they are not yet configured, create them first — see "Creating the four
  test loads" below.
- A way to send lines to the serial console and read the reply. Any serial
  terminal works (`pio device monitor`, `screen /dev/ttyUSB1 115200`,
  PuTTY, etc.) — commands below are just typed at the `kilowatts>` prompt
  followed by Enter.

**GPIO 2 warning:** GPIO2 is one of the ESP32's boot-strapping pins. If a
relay is wired to it and its board pulls the pin high while floating,
flashing new firmware over serial can fail with
`Failed to communicate with the flash chip`. If you need to reflash before
running these tests, disconnect the relay wire from pin 2 first, flash,
then reconnect it — the other three pins (4, 18, 25) do not need to be
disconnected.

## Creating the four test loads (only if not already configured)

Replace `<MAC>` with the Central Node's own MAC address (shown at boot, or
via `node show` / the address burned into the board — in this session it
was `A4:CF:12:0E:32:C0`).

```
load add mac=<MAC> pin=4  name=Lamp   power=10 priority=1 type=DC active_high=off mode=FIXED_OFF schedule=none
load add mac=<MAC> pin=2  name=Load2  power=10 priority=2 type=DC active_high=off mode=AUTO_OFF   schedule=none
load add mac=<MAC> pin=18 name=Load18 power=10 priority=3 type=DC active_high=off mode=AUTO_OFF   schedule=none
load add mac=<MAC> pin=25 name=Load25 power=10 priority=1 type=DC active_high=off mode=AUTO_OFF   schedule=none
```

`active_high=off` is the important part for active-low relay boards — it
tells the firmware that GPIO LOW means the relay is ON. Getting this wrong
reproduces the original bug (see `TESTING_AND_VALIDATION.md` §2).

Check what actually got stored at any time with:

```
load show <PIN>
```

which prints, among other fields, `Relay polarity : active-LOW` or
`active-HIGH` and whether the hardware was actually applied.

## Switching to the simulated battery

The algorithm tests don't need a real INA219 sensor — the simulator feeds
the exact same code path (`PowerManager::updatePowerBudget()`), it's just
fed a value you set instead of an I2C reading.

```
sensor sim
simulation start
simulation values voltage=12.6 current=0 soc=80
```

`simulation values` can be re-run at any time with new numbers; you don't
need to repeat `sensor sim` / `simulation start` afterward.

Force an immediate planning cycle (instead of waiting for the automatic
5-minute interval) with:

```
optimize run
```

Check what happened with:

```
status
loads
```

`status` (typed with no argument, or via `dashboard`) prints the full
power-budget breakdown; `loads` lists every configured Load and its
current mode.

## Running the six tests

### Test 1 — Baseline

```
load set <MAC> 4 mode=FIXED_OFF
simulation values voltage=12.6 current=0 soc=80
optimize run
status
loads
```
Expect: all three AUTO loads (2, 18, 25) ON, pin 4 OFF.

### Test 2 — FIXED_ON deducted first + priority tie-break

```
load set <MAC> 4 mode=FIXED_ON
optimize run
status
loads
```
Expect: pin 4 ON, plus the two AUTO loads whose priorities sum highest
(pins 2 and 18, since 2+3 > 2+1 > 3+1), pin 25 OFF.

### Test 3 — SoC at/below reserve

```
simulation values voltage=12.0 current=0 soc=15
optimize run
status
loads
```
(pin 4 stays FIXED_ON from Test 2). Expect: pin 4 still ON, all three AUTO
loads OFF, dashboard shows `sustainable 0.00 W | NOT ACHIEVABLE`.

### Test 4 — Discharge-current ceiling

```
load set <MAC> 4 mode=FIXED_OFF
battery limits min_soc=20 max_discharge_amps=1 max_main_amps=15 runtime_hours=24
simulation values voltage=12.6 current=0 soc=80
optimize run
optimize run
status
loads
```
(a second `optimize run` lets the simulated current smooth through the
sensor's EMA filter before reading `status`). Expect `AUTO available
power` to read exactly `voltage × 1 A` (12.60 W at 12.6 V), and only the
single highest-priority AUTO load (pin 18) ON.

### Test 5 — Runtime target unachievable

```
battery limits min_soc=20 max_discharge_amps=10 max_main_amps=15 runtime_hours=200
load set <MAC> 4 mode=FIXED_ON
simulation values voltage=12.6 current=0 soc=80
optimize run
optimize run
status
loads
```
Expect `Required runtime` to show a small `sustainable` figure with
`NOT ACHIEVABLE`, `Fixed ON power` still reported as the full 10 W
(never clipped), all AUTO loads OFF, pin 4 ON.

### Test 6 — Schedule vs. priority

```
battery limits min_soc=20 max_discharge_amps=1 max_main_amps=15 runtime_hours=24
load set <MAC> 18 schedule=23:59
simulation values voltage=12.6 current=0 soc=80
optimize run
optimize run
loads
```
Expect: pin 2 ON (NOT pin 18, despite pin 18 having the highest priority) —
a not-yet-due schedule can knock a Load out of the running even though its
priority would otherwise win.

Then flip it:
```
load set <MAC> 18 schedule=00:00
optimize run
optimize run
loads
```
Expect: pin 18 ON again — with the schedule "due", priority-driven
selection returns to normal.

Clean up before continuing:
```
load set <MAC> 18 schedule=none
```

### Restoring normal operation afterward

```
battery limits min_soc=20 max_discharge_amps=10 max_main_amps=15 runtime_hours=24
load set <MAC> 4 mode=FIXED_OFF
simulation values voltage=12.6 current=0 soc=80
optimize run
status
loads
```
Expect: back to the Test 1 result (pins 2, 18, 25 ON, pin 4 OFF).

## Reference: commands used above

| Command | Purpose |
|---|---|
| `load show PIN` | Show one Load's full status, including relay polarity |
| `loads` | List every configured Load and its mode |
| `load set MAC PIN mode=...` | Change a Load's FIXED/AUTO + ON/OFF mode |
| `sensor sim` | Switch the battery measurement source to the simulator |
| `simulation start` | Enable the simulator |
| `simulation values voltage=V current=A soc=PERCENT` | Feed a fake battery reading |
| `battery limits min_soc=P max_discharge_amps=A max_main_amps=A runtime_hours=H` | Change the configured power-safety limits |
| `optimize run` | Force one immediate planning/dispatch cycle |
| `status` | Full power-budget dashboard |

Full syntax for any command (including `load add`) is printed by running
the command with no arguments, e.g. `load`, `battery`, `simulation`.
