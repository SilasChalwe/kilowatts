# Kilowatts — User Manual (Console + MQTT Interface)

This is the manual for operating and demonstrating Kilowatts **today**,
using the two interfaces the firmware already exposes: the serial
**console dashboard** and **MQTT**. A dedicated mobile/installer-web app
(`kilowatts-mobile`, a separate Flutter + Firebase project) is under
development alongside this firmware, but nothing below depends on it —
everything in this manual works against the firmware alone.

---

## 1. What you're operating

- **Central** (`env:central`, ESP32 dev board) — the one node with a
  battery-bus INA219 sensor, MQTT/Wi-Fi connectivity, and the Best-First
  Search planner. There is exactly one Central per installation.
- **Smart Node(s)** (`env:smart`, ESP32-S3 dev board) — relay-driven load
  branches. Central discovers them over ESP-NOW, commissions them, and
  dispatches relay commands to them; they never talk MQTT directly.

Flash each device with its own PlatformIO environment:

```bash
pio run -e central -t upload
pio run -e smart   -t upload
```

(`platformio.ini`'s `upload_port`/`monitor_port` point at whichever
`/dev/ttyUSBx` your board enumerates as — check `ls /dev/ttyUSB*` and edit
if needed before uploading.)

---

## 2. First boot: Wi-Fi + MQTT provisioning (Central only)

Central boots with no Wi-Fi credentials the first time, so it opens its
own captive-portal access point instead of trying (and failing) to
connect to anything.

1. On your phone/laptop, look for a Wi-Fi network named
   **`Kilowatts-Setup-XXXX`** (last two bytes of Central's MAC address).
   Connect to it — no password.
2. A setup page should open automatically (captive-portal redirect); if
   not, browse to `http://192.168.4.1/`.
3. Fill in:
   - **Network name (SSID)** — pick from the scanned list, or choose
     "Other" and type it in the manual field.
   - **Password** — your real Wi-Fi password.
   - *(optional)* **MQTT broker host/port**, TLS checkbox,
     username/password — leave all of this blank to use the project's
     built-in cloud broker instead (see `include/KilowattsSecrets.h`).
4. Submit. Central saves the credentials, restarts, and joins your
   network. Once it has an IP address, it starts MQTT automatically.

If Central ever fails to reconnect to Wi-Fi repeatedly, it reopens this
same portal on its own — you don't need to factory-reset it to
re-provision.

**Demo tip — no internet at the venue:** run a local broker on your
laptop instead of relying on the cloud broker or venue Wi-Fi routing
MQTT out to the internet:

```bash
sudo apt install mosquitto mosquitto-clients   # once
sudo systemctl enable --now mosquitto
hostname -I                                    # your laptop's LAN IP
```

Enter that IP as the MQTT host in step 3 above (port `1883`, no TLS, no
username/password — fine for a closed bench network, never for a real
install).

---

## 3. Commissioning a Smart Node

A freshly flashed Smart Node broadcasts its identity over ESP-NOW as soon
as it boots. Central hears it and publishes a `NODE_DISCOVERED` event —
watch for it on the console or on the `events` MQTT topic (§5). A
discovered-but-uncommissioned node does not yet plan or run loads; you
commission it explicitly:

```bash
mosquitto_pub -h <broker> -t 'kilowatts/v1/commands/config' -m '{
  "commandId": 1,
  "action": "COMMISSION_NODE",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "friendlyName": "Living Room"
}'
```

Watch `kilowatts/v1/acks` for two acknowledgements: an immediate
`ACCEPTED`/`REJECTED` from Central, then `APPLIED`/`FAILED` once the Smart
Node itself confirms over ESP-NOW.

---

## 4. Configuring a Load on a commissioned node

```bash
mosquitto_pub -h <broker> -t 'kilowatts/v1/commands/config' -m '{
  "commandId": 2,
  "action": "CONFIGURE_LOAD",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "load": {
    "name": "Fridge",
    "relayPin": 4,
    "relayActiveHigh": true,
    "mode": "AUTO_OFF",
    "priority": 10,
    "nominalVoltageVolts": 230.0,
    "nominalCurrentAmps": 1.5,
    "startupWatts": 800.0,
    "schedule": {"enabled": false, "hour": 0, "minute": 0}
  }
}'
```

- `relayPin` is entirely your choice — there is no compiled-in per-board
  safe-pin whitelist. **You are responsible for knowing which GPIOs are
  safe relay outputs on your specific board** (avoid flash-bus pins,
  boot-strapping pins, input-only pins, and UART0 — these differ between
  a classic ESP32 like Central's and the ESP32-S3 Smart Nodes use). A
  wrong choice is applied as given; nothing in firmware will refuse or
  correct it for you.
- `mode` is one of `FIXED_OFF` / `FIXED_ON` / `AUTO_OFF` / `AUTO_ON` — the
  `FIXED`/`AUTO` split and starting state the Chapter 4 model expects.
- To remove a load instead: `"action": "REMOVE_LOAD"` with the same
  `nodeMac` and a top-level `"relayPin"`.

To change an **existing** load's priority/mode/schedule without
reconfiguring hardware, use `commands/load` instead:

```bash
mosquitto_pub -h <broker> -t 'kilowatts/v1/commands/load' -m '{
  "commandId": 3,
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "relayPin": 4,
  "priority": 25,
  "mode": "AUTO_ON"
}'
```

`priority`, `mode` and `schedule` are all optional here — include only
the field(s) you want to change.

---

## 5. Configuring Central's battery sensor

```bash
mosquitto_pub -h <broker> -t 'kilowatts/v1/commands/config' -m '{
  "commandId": 4,
  "action": "CONFIGURE_BATTERY_SENSOR",
  "nodeMac": "<Central'\''s own MAC>",
  "batterySensor": {
    "shuntResistanceOhms": 0.1,
    "maximumExpectedCurrentAmps": 40.0,
    "emaAlpha": 0.2,
    "batteryCapacityAmpHours": 100.0,
    "initialStateOfChargePercent": 80.0,
    "nominalVoltageVolts": 12.0
  }
}'
```

This is complete once Central durably applies it locally, so its ack
goes straight to `APPLIED`/`REJECTED` (no second Smart-Node round trip).

> **Right now this affects bookkeeping only, not the live reading**: the
> INA219 is in bench/simulation mode (see §8) until real hardware is
> confirmed wired, so `state/system` reports `batteryMeasurementSourceText:
> "SIMULATED"` regardless of what you configure here.

---

## 6. Other system commands (`commands/system`)

```jsonc
{"commandId": 5, "action": "REQUEST_OPTIMIZATION_CYCLE"}
{"commandId": 6, "action": "FACTORY_RESET_NODE", "targetNodeMac": "AA:BB:CC:DD:EE:FF", "confirm": "RESET"}
{"commandId": 7, "action": "FACTORY_RESET_CENTRAL", "confirm": "RESET"}
{"commandId": 8, "action": "APPLY_SAFETY_CONFIG", "safetyConfig": {
  "minimumStateOfChargePercent": 20.0,
  "warningStateOfChargePercent": 30.0,
  "targetRuntimeHours": 6.0,
  "safetyFactor": 1.2,
  "maximumBatteryDischargeCurrentAmps": 40.0,
  "maximumMainCurrentAmps": 30.0
}}
```

`REQUEST_OPTIMIZATION_CYCLE` is the fastest way to force an immediate
console refresh + MQTT state publish for a demo instead of waiting for
the next automatic cycle.

---

## 7. Reading the console dashboard

Open the serial monitor (`pio device monitor -e central`, or the same
`upload_port` at 115200 baud). Every optimization cycle prints a full
snapshot:

```
================ KILOWATTS ================
NODES
Total Nodes              : 2
Online Nodes             : 2
  CENTRAL | Central | AA:BB:CC:DD:EE:FF | ONLINE | Loads: 1
  SMART | Living Room | 11:22:33:44:55:66 | ONLINE | Loads: 1
    Fridge | Pin 4 | AUTO | ON | Pin: ON | 800.000 W

SOURCE READING
Voltage                  : 13.10 V
Current                  : 3.42 A
Measured Source Power    : 44.802 W
Measurement Source       : SIMULATED

BATTERY
...
Runtime Power Limit      : ... W
Safety Ceiling           : ... W

LOADS
Fixed ON Loads           : 0
Fixed OFF Loads          : 0
Auto Loads               : 1
Fixed ON Power           : 0.000 W

BEST-FIRST SEARCH
Power before Fixed Loads : ... W
Power passed to BFS      : ... W
Selected Auto Loads:
  Living Room | Fridge | Pin 4 | 800.000 W
Selected Auto Load Power : 800.000 W
Final Remaining Power    : ... W
===========================================
```

For a live demo, this is the single most useful thing to have on screen:
it shows discovered/online nodes, the (currently simulated) source
reading actually changing cycle to cycle, and exactly which Auto loads
Best-First Search admitted or rejected — no app required.

---

## 8. Watching via MQTT

```bash
mosquitto_sub -h <broker> -t 'kilowatts/v1/#' -v
```

| Topic | What you'll see |
|---|---|
| `state/system` | Battery/SoC/power-limit/connectivity snapshot, retained |
| `state/tree` | Commissioning topology tree |
| `state/loads` | Every Load's current planned/confirmed state |
| `state/nodes` | Node identity + online/offline + diagnostics |
| `config/nodes` | Commissioned-node configuration mirror |
| `events` | `NODE_DISCOVERED` / `NODE_OFFLINE` / `NODE_RECOVERED` / etc. |
| `acks` | Every command's ACCEPTED→APPLIED (or REJECTED/FAILED) result |

Running the console (§7) and `mosquitto_sub` (above) side by side is the
recommended way to present the system today: the console shows the
planning logic in plain text, MQTT proves the exact same data is
available for any future client to consume.

---

## 9. Known limitations worth disclosing up front

- **INA219 reading is currently simulated**, not read from real hardware
  — a deliberate bench-mode flag in `lib/BatteryManager/INA219Monitor.cpp`
  (`USE_SIMULATED_READING`), clearly labelled `SIMULATED` everywhere it
  surfaces (console, `state/system`). It now varies over time (a
  repeating wave) rather than sitting at one frozen value, so a
  hardware-free run still shows Best-First Search reacting to changing
  available power.
- **Automatic (NTP) time sync only works on Central** once Wi-Fi is
  provisioned (§2) — Smart Nodes have no internet path of their own and
  need Manual time mode until a time-distribution mechanism over ESP-NOW
  is added.
- **The dedicated mobile/installer-web app is a separate, early-stage
  project** (`kilowatts-mobile`) — everything in this manual is the
  supported way to operate and present the system in the meantime.

---

## 10. Quick reference

| Task | Topic | Key fields |
|---|---|---|
| Commission a node | `commands/config` | `action=COMMISSION_NODE`, `nodeMac`, `friendlyName` |
| Rename a node | `commands/config` | `action=RENAME_NODE`, `nodeMac`, `friendlyName` |
| Decommission a node | `commands/config` | `action=DECOMMISSION_NODE`, `nodeMac` |
| Add/replace a load | `commands/config` | `action=CONFIGURE_LOAD`, `nodeMac`, `load{...}` |
| Remove a load | `commands/config` | `action=REMOVE_LOAD`, `nodeMac`, `relayPin` |
| Configure Central's battery sensor | `commands/config` | `action=CONFIGURE_BATTERY_SENSOR`, `nodeMac`, `batterySensor{...}` |
| Change a load's priority/mode/schedule | `commands/load` | `nodeMac`, `relayPin`, optional `priority`/`mode`/`schedule` |
| Force an optimization cycle | `commands/system` | `action=REQUEST_OPTIMIZATION_CYCLE` |
| Apply safety policy | `commands/system` | `action=APPLY_SAFETY_CONFIG`, `safetyConfig{...}` |
| Factory reset a node/Central | `commands/system` | `action=FACTORY_RESET_NODE`/`FACTORY_RESET_CENTRAL`, `confirm` |

Every command needs a unique `commandId` (any `uint32_t`) and gets exactly
one (or two, for node-round-trip commands) acknowledgement on `acks`.
