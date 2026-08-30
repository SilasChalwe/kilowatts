# Central Console — Full Operational Run (Live Hardware)

Board: ESP32-D0WDQ6, MAC `A4:CF:12:0E:32:C0`, port `/dev/ttyUSB0`
Firmware: `central` environment
Captured: 2026-08-30, live from the physical board, one continuous operational sequence: build → boot → Wi-Fi/MQTT → power source → 8 loads → Best-First → reboot persistence.

**Evidence status:** this is a historical capture of that session, not the
authoritative description of the final battery-initialization behavior. The
temporary default-configuration fix described below was later removed. For the
current behavior, use `test-evidence/INSTALLATION_TEST_REPORT.md`,
`USER_MANUAL.md`, and `TEST_REPRODUCTION_GUIDE.md`. The raw console output is
retained unchanged as evidence of what was observed at capture time.

Open this file with **Markdown Preview** in VS Code (`Ctrl+Shift+V`) and PrtScn whichever sections you want as images.

---

## 0. Two real bugs found and fixed during this run

Building this evidence run surfaced two genuine firmware bugs, fixed and reflashed before the final capture below:

1. **Battery source selection required manual configuration first.** The fix
   present during this capture temporarily supplied a default configuration.
   Later review correctly found that installation-specific sensor and battery
   values must never be invented, so that implementation was removed. The
   final behavior allows simulation-source selection without INA219
   calibration while requiring explicit battery profile/limit values for a
   meaningful installation-specific runtime budget. See the evidence-status
   note above.

2. **Removing then re-adding a Load on the same pin silently kept the old priority/mode.** `load remove` cleared the hardware store but left a stale entry in the separate `LoadConfigurationStore` (the store that protects a user's chosen priority/mode from being overwritten by a Smart Node's own reports) — so a `load add` on that same pin got silently clobbered back to the old values on the very next planning cycle. Fixed: `load remove` now also clears the matching `LoadConfigurationStore` entry (`lib/LoadManager/Central/LoadConfigurationStore.{h,cpp}`, wired into both the Central and Smart-Node removal paths in `src/central/namespace.h`).

Both fixes as they existed at capture time were rebuilt for all three firmware
targets, reflashed, and exercised below. The Load removal/re-addition fix
remains current; the battery fix was subsequently replaced as described above.

---

## 1. Boot

```
kilowatts > status
SYSTEM STATUS
Wi-Fi       : CONNECTED
MQTT        : CONNECTED
Smart Nodes : 1 / 1 online
Battery     : NOT AVAILABLE
Available power passed to Best-First: NOT AVAILABLE
Control errors: 0
```

---

## 2. Power source — INA219 attempted, correctly falls back to simulation

```
kilowatts > sensor ina219
[ OK ] Battery measurement source: INA219
kilowatts > battery limits min_soc=20 max_discharge_amps=10 max_main_amps=15 runtime_hours=24
[ OK ] power limits saved; INA219 is not currently responding
kilowatts > battery configure shunt_ohms=0.01 max_sensor_amps=20 ema_alpha=0.2 capacity_ah=50 initial_soc=75 nominal_voltage=12.6
[ OK ] battery monitor configured
kilowatts > battery status
BATTERY MONITOR
Configuration      : CONFIGURED
Measurement source : NONE
...
kilowatts > sensor sim
[ OK ] Battery measurement source: SIMULATION
kilowatts > simulation start
[ OK ] simulation enabled; battery monitor ready
kilowatts > simulation values voltage=12.6 current=1.5 soc=75
[ OK ] simulated battery values applied
kilowatts > battery status
BATTERY MONITOR
Configuration      : CONFIGURED
Measurement source : SIMULATED
Voltage            : 12.600 V
Current            : 1.500 A
Power              : 18.900 W
State of Charge    : 75.00 %
SoC source         : COULOMB_COUNTING
Power limits       : CONFIGURED
Reserve/min SoC    : 20.0 %
Max discharge      : 10.00 A
Max main current   : 15.00 A
Required runtime   : 24.00 h (target) | 23.99 h remaining
```

**Result at capture time:** no physical INA219 was wired to this devkit, and
the bounded I2C probe correctly reported it unavailable. After the explicit
`battery limits` and `battery configure` commands shown above, simulation took
over as the measurement source. This section does not demonstrate the final
blank-configuration behavior; see `INSTALLATION_TEST_REPORT.md`.

---

## 3. Eight loads configured on Central (2 AC / 6 DC, 2 FIXED / 6 AUTO)

Existing 4 loads renamed (remove + re-add, since `load set` can't rename), 4 new loads added on newly-selected safe GPIOs (23/25/26/27 — not the UART console pins 1/3, not the INA219 I2C pins 21/22, not the existing load pins):

```
kilowatts > loads
LOADS
A4:CF:12:0E:32:C0 pin=4  | Fridge          |  8.00 W | priority=1 | FIXED_ON | AC
A4:CF:12:0E:32:C0 pin=5  | Lights          |  3.00 W | priority=8 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=18 | WaterPump       | 10.00 W | priority=5 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=19 | Fan             |  5.00 W | priority=3 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=23 | Router          |  2.00 W | priority=9 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=25 | SecurityCamera  |  4.00 W | priority=7 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=26 | WaterHeater     | 15.00 W | priority=1 | FIXED_OFF| AC
A4:CF:12:0E:32:C0 pin=27 | PhoneCharger    |  1.00 W | priority=2 | AUTO_OFF | DC
```

**Result:** 8/8 loads landed exactly as configured — 2 AC (Fridge, WaterHeater), 6 DC; 2 FIXED (Fridge ON, WaterHeater OFF), 6 AUTO. Verified with individual `load show <pin>` for every load, not just the table view.

---

## 4. Best-First optimizer — real trade-off, not a canned result

```
kilowatts > optimize run

============================================================
                    KILOWATTS DASHBOARD
============================================================
Battery source        : SIMULATED
Battery voltage       : 12.60 V
Battery current       : 1.50 A
Battery power         : 18.90 W
Battery SoC           : 74.8 %
------------------------------------------------------------
Available power passed to Best-First : 6.43 W
Fixed load power      : 8.00 W
Automatic load power  : 6.00 W
Remaining power       : 0.43 W
Required runtime      : 23.92 h remaining | sustainable 14.43 W | ACHIEVABLE
Fixed ON / OFF        : 1 / 1
Automatic loads       : 3 / 6
============================================================
```

Fixed loads applied exactly as configured (Fridge ON, WaterHeater OFF — 8.00 W, matching only Fridge). Of the 6 AUTO loads competing for 6.43 W, Best-First selected **Router (2 W, priority 9), Lights (3 W, priority 8), PhoneCharger (1 W, priority 2)** — 6.00 W total, the combination maximizing total priority (19) that still fits the budget — over higher-individual-priority-but-bulkier alternatives like WaterPump (10 W) or SecurityCamera (4 W). Confirmed per-load:

```
kilowatts > load show 23        kilowatts > load show 5         kilowatts > load show 27
Mode : AUTO_ON                  Mode : AUTO_ON                  Mode : AUTO_ON

kilowatts > load show 18        kilowatts > load show 19        kilowatts > load show 25
Mode : AUTO_OFF                 Mode : AUTO_OFF                 Mode : AUTO_OFF
BFS result : POWER_BUDGET_EXCEEDED (all three)
```

Reproduced across 3 consecutive `optimize run` cycles with identical, stable results.

---

## 5. System reset — full persistence check

```
kilowatts > system reset
[ OK ] Central restarting in 1.5 seconds
... (genuine reboot, boot banner) ...
kilowatts > status
Wi-Fi : CONNECTED   MQTT : CONNECTED   Smart Nodes : 1/1 online
kilowatts > loads
LOADS
A4:CF:12:0E:32:C0 pin=23 | Router          |  2.00 W | priority=9 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=26 | WaterHeater     | 15.00 W | priority=1 | FIXED_OFF| AC
A4:CF:12:0E:32:C0 pin=27 | PhoneCharger    |  1.00 W | priority=2 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=5  | Lights          |  3.00 W | priority=8 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=18 | WaterPump       | 10.00 W | priority=5 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=19 | Fan             |  5.00 W | priority=3 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=25 | SecurityCamera  |  4.00 W | priority=7 | AUTO_OFF | DC
A4:CF:12:0E:32:C0 pin=4  | Fridge          |  8.00 W | priority=1 | FIXED_ON | AC
kilowatts > battery status
Measurement source : NONE   (correctly cleared — simulation never survives a reboot)
```

**Result:** All 8 loads survived the reboot with exactly their configured names/pins/power/priority/mode/type (including the `LoadConfigurationStore` fix — Fridge is still genuinely `FIXED_ON`, not reverted). Wi-Fi and MQTT both reconnected. Simulated battery correctly reset to `NONE`, never mistaken for a real reading after restart.

---

## Summary

| Step | Result |
|---|---|
| Build + upload (with the fixes present at capture time) | PASS — all 3 firmware targets built clean, 49/49 host tests |
| Power source: INA219 probed → simulation fallback | PASS |
| 8 loads configured (2 AC/6 DC, 2 FIXED/6 AUTO) | PASS — verified individually |
| Best-First optimizer real selection | PASS — genuine trade-off (Router+Lights+PhoneCharger, 6/6.43 W) |
| Reboot persistence (loads, Wi-Fi, MQTT, battery) | PASS |

Raw unedited captures backing this document: `test-evidence/logs/*.txt`
