# Kilowatts Installation Test Report

Board tested: ESP32-D0WDQ6, MAC `A4:CF:12:0E:32:C0` (Central role), reflashed as `smart_esp32` for the console test and back. All results below are from the live physical board, not simulated/predicted — commands run over the real serial console, alerts captured over a real TLS MQTT subscription to the live broker.

This report is the reference for what the firmware actually does today. Use it at each install site as a checklist: run the same commands, expect the same behavior.

## What changed and why

Three real problems were found and fixed:

1. **Battery configuration was becoming hardcoded.** An earlier fix for "`sensor sim` needs manual setup first" had silently injected fixed numbers (shunt resistance, capacity, voltage) the first time a source was selected. This is wrong for a system installed in different houses with different batteries — removed entirely. The real fix: simulation mode doesn't need INA219 calibration data at all (it never reads it), so `PowerManager::initialize()` now only requires that data in hardware mode. Nothing is invented anywhere.
2. **No MQTT alerts existed.** Every fault-worthy condition (battery hit reserve, runtime target unachievable, INA219 stopped responding, a Smart Node went offline) was a passive field in a retained topic, refreshed once a minute, with no push notification — which is why the owner never saw an alert in their MQTT client. Added a real `alerts` topic, edge-triggered (fires only on the transition, not every cycle).
3. **Smart Nodes had no local console.** All configuration went through Central over ESP-NOW. Added a minimal one: `status`, `loads`, `load show/add/remove/set`.

## 1. Battery configuration — no hardcoded values

**Claim:** `sensor sim` works immediately, with zero prior `battery configure` call. Only `battery limits` (the reserve/runtime policy — genuinely the installer's decision) needs to be set.

```
kilowatts > battery limits min_soc=20 max_discharge_amps=10 max_main_amps=15 runtime_hours=24
[ OK ] power limits saved; INA219 is not currently responding
kilowatts > sensor sim
[ OK ] Battery measurement source: SIMULATION
kilowatts > simulation start
[ OK ] simulation enabled; battery monitor ready
kilowatts > simulation values voltage=12.6 current=1.5 soc=75
[ OK ] simulated battery values applied
```

No `battery configure shunt_ohms=...` call anywhere in this sequence. **PASS.**

**Hardware mode still correctly requires real values** (this is not a regression — an installer's actual INA219 board has a real shunt resistor that must be entered accurately, or every reading is wrong):

```
kilowatts > sensor ina219
[ OK ] Battery measurement source: INA219
kilowatts > battery status
Measurement source : NONE
```

Reports `NONE` / "run battery configure/battery limits... first" rather than silently defaulting. **PASS.**

## 2. MQTT alerts — edge-triggered, verified on the real wire

**Claim:** a genuine push notification lands on `kilowatts/v1/alerts` when a fault condition changes, not just a passive field in a retained topic.

Subscribed a real `mosquitto_sub` client to the live HiveMQ broker over TLS, then pushed the board's simulated SoC to 15% (below the 20% reserve just configured above):

```
$ mosquitto_sub -h <broker> -p 8883 --cafile ... -u kilowatts -P '<password>' -t kilowatts/v1/alerts -v
kilowatts/v1/alerts {"schemaVersion":3,"alertType":"BATTERY_RESERVE","severity":"critical","detail":"battery state of charge reached the configured reserve"}
kilowatts/v1/alerts {"schemaVersion":3,"alertType":"RUNTIME_TARGET","severity":"warning","detail":"required runtime target is no longer achievable at the current load"}
```

Then recovered SoC to 75%, same subscription:

```
kilowatts/v1/alerts {"schemaVersion":3,"alertType":"BATTERY_RESERVE","severity":"info","detail":"battery state of charge recovered above the configured reserve"}
kilowatts/v1/alerts {"schemaVersion":3,"alertType":"RUNTIME_TARGET","severity":"info","detail":"required runtime target is achievable again"}
```

Both directions, both conditions, real broker, real TLS connection. **PASS.**

Also implemented (same edge-triggered mechanism, wired into the existing periodic/acquisition tasks — not independently wire-verified this session, but built and covered by the same code path as the two above): `BATTERY_SENSOR` (INA219 stops/resumes responding, hardware mode only) and `NODE_OFFLINE` (a Smart Node's online status flips). Worth a follow-up check with a physical INA219 and a second board.

## 3. Smart Node console — verified standalone on real hardware

Flashed the physical board as `smart_esp32` (a second, dedicated board is needed to run this alongside Central in a real deployment — this session only has the one board, swapped between roles).

```
smart > status
SMART NODE STATUS
Commissioned : NO
Loads        : 0
Radio channel: 7
Upstream hop : 65535
smart > load add pin=32 name=TestLoad power=6 priority=5 type=DC active_high=off mode=AUTO_OFF schedule=none
[ OK ] Load configured
smart > load set 32 priority=8 mode=FIXED_ON
[ OK ] Load updated
smart > load show 32
Priority       : 8
Mode           : FIXED_ON
smart > loads
LOADS
pin=32 | TestLoad         |    6.00 W | priority=8 | FIXED_ON  | DC
smart > load remove 32
[ OK ] Load removed
```

Add, show, in-place update (priority + mode both changed correctly, name/power/type preserved), list, and remove all verified working. **PASS.**

**Caveat found during testing, not a bug:** Central and a Smart Node share the same `NodeLoadHardwareStore` NVS key names (`kw_node_loads`/`loads`). On separate physical boards (a real deployment) this causes no collision — each device has its own flash. It only matters when testing both roles on one shared dev board, as this session did: Central's own loads (pins 4, 5, 18, 19, 23, 25, 26, 27) showed up as "already configured" when tested from the Smart firmware on the same physical chip. Testing note, not a firmware defect.

**Separately noted (pre-existing, out of this session's scope):** a Smart Node's persisted load configuration is only re-applied to the live Node object at boot if the node is already commissioned (`SmartApplication.cpp`). An uncommissioned node with stale persisted config ends up in an inconsistent state — the store has an entry, the live console reports none. Worth a look in a future session if it causes confusion at a real install.

## Summary

| # | What | Verified how | Result |
|---|---|---|---|
| 1 | No hardcoded battery values; simulation self-sufficient | Live console, zero `battery configure` calls | PASS |
| 1 | Hardware mode still requires real values | Live console | PASS |
| 2 | `BATTERY_RESERVE` alert, both directions | Real MQTT subscription to live broker | PASS |
| 2 | `RUNTIME_TARGET` alert, both directions | Real MQTT subscription to live broker | PASS |
| 2 | `BATTERY_SENSOR` / `NODE_OFFLINE` alerts | Code review + shared code path only | Built, not independently wire-verified |
| 3 | Smart Node console: add/show/set/list/remove | Live console on physical board (flashed `smart_esp32`) | PASS |

All three firmware targets (`central`, `smart`, `smart_esp32`) build clean; the 49-check host test suite passes throughout.
