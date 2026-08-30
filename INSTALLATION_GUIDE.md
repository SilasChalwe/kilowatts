# Installation Guide

This is the process for taking Kilowatts firmware from a flashed board to a real, working installation at a customer's site. It is deliberately sequenced: **simulate before you connect anything real.**

## 0. Before you touch a real site: learn the system first

Do not install this on a live battery/appliance setup as your first exposure to it. Before any real installation, spend time running the system in simulation mode on a bench — connect Wi-Fi/MQTT, add a few Loads, feed `simulation values` through a range of voltage/current/SoC, and watch `dashboard`/`state/system` respond. Understand what `battery limits` (reserve SoC, max discharge/main current, required runtime) actually controls, and how it relates to what Best-First selects (`TECHNICAL_REFERENCE.md`). An installer who understands this relationship can size and configure a real site confidently and quickly; one who doesn't will be guessing at the customer's premises.

## 1. Survey the site

Before configuring anything, learn from the customer:
- What appliances/loads do they want managed, and their approximate power draw (watts) and criticality (must never turn off vs. can be deferred)?
- What battery do they have (or plan to install) — nominal voltage, capacity in Ah?
- Do they have a target minimum runtime (hours the system must sustain critical loads)?
- Is a real INA219 battery sensor available yet, or is this visit purely for sizing?

## 2. Size the system in simulation — before any real sensor is connected

This is the core of what simulation mode is for. It is not a "fake" or "demo" mode — it's the real pre-installation validation step, using the exact same planning code a live installation uses (`USER_MANUAL.md` §2, §4 "Measurement source and simulation").

1. `sensor sim` then `simulation start` — works immediately, no prior configuration needed.
2. Configure each Load the customer described (`load add ...`), with realistic wattages, priorities, and FIXED/AUTO modes.
3. Feed `simulation values voltage=... current=... soc=...` across the range you expect the real battery to operate in.
4. Watch `dashboard`/`optimize run` and observe which Loads get selected at each simulated level, and whether the customer's stated runtime target is achievable (`Required runtime: ... ACHIEVABLE`/`NOT ACHIEVABLE`).
5. Adjust priorities, or discuss trade-offs with the customer, until the simulated behavior matches what they actually need.
6. Once satisfied, set the real `battery limits` (reserve SoC, max discharge/main current, required runtime hours) — these are the values simulation just helped you find, not invented defaults.

## 3. Connect the real INA219 sensor

Only after step 2 is complete:

1. Enter the sensor's real, physical specifications with `battery configure shunt_ohms=... max_sensor_amps=... ema_alpha=... capacity_ah=... initial_soc=... nominal_voltage=...` — every one of these is specific to the actual battery and sensor board installed at this site; none of it is defaulted by the firmware (see `LIMITATIONS.md` and `USER_MANUAL.md` §4).
2. `sensor ina219`, then `battery status` — confirm `Measurement source : HARDWARE` with sane readings. If it reports `NONE`/"not currently responding", check wiring before proceeding.
3. Re-check `dashboard` against the real reading. The `battery limits` policy you set in step 2.6 does not need to change — that's the point of having sized it in simulation first. Only adjust the reserve/runtime numbers if the real battery's behavior genuinely differs from what was simulated.

## 4. Commission Smart Nodes (if any)

For each Smart Node board at the site: flash it (`pio run -e smart -t upload`), power it up, and either commission it from Central's console/MQTT (`node commission MAC name=...` / `commands/config` `COMMISSION_NODE`) or configure its Loads directly at the node using its own local console (`USER_MANUAL.md` §4 "Smart Node console") if you're standing right at it.

## 5. Final checks before leaving site

- `status` / `state/nodes` — every expected node shows online.
- `loads` / `state/loads` — every configured Load matches what was agreed with the customer.
- Subscribe to `alerts` for a few minutes and confirm nothing unexpected fires (see `lib/MqttManager/README.md`).
- Leave the customer or their app pointed at `status` for availability monitoring (`USER_MANUAL.md` §"Availability").

See also: `USER_MANUAL.md`, `TECHNICAL_REFERENCE.md`, `lib/MqttManager/README.md`, `LIMITATIONS.md`.
