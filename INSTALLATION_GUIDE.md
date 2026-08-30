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

1. Record the intended battery's nominal voltage and capacity. If the INA219
   board and shunt are already known, record those specifications too.
2. Run `sensor sim` then `simulation start`. Source selection itself can start
   without prior configuration and does not invent any profile values.
3. Enter the proposed profile with `battery configure ...`. Because simulation
   is already selected, this does not attempt to connect to the sensor; it
   supplies the battery capacity and nominal voltage needed by the runtime
   formula. Do not invent these values—use the intended installation
   specifications.
4. Set an initial `battery limits` policy (reserve SoC, max discharge/main
   current, required runtime). These are deliberate sizing assumptions, not
   firmware defaults.
5. Configure each Load the customer described (`load add ...`), with realistic
   wattages, priorities, schedules, and FIXED/AUTO modes.
6. Feed `simulation values voltage=... current=... soc=...` across the range
   expected from the real battery.
7. Watch `dashboard`/`optimize run` and observe which Loads get selected and
   whether the runtime target is `ACHIEVABLE` or `NOT ACHIEVABLE`.
8. Adjust priorities and `battery limits`, or discuss trade-offs with the
   customer, until the simulated behavior matches their requirements.

## 3. Connect the real INA219 sensor

Only after step 2 is complete:

1. Verify the proposed profile from step 2 against the actual battery, shunt,
   and sensor board. Correct it with `battery configure
   shunt_ohms=... max_sensor_amps=... ema_alpha=... capacity_ah=...
   initial_soc=... nominal_voltage=...` before selecting hardware mode. None of
   these values is defaulted by the firmware (see `LIMITATIONS.md` and
   `USER_MANUAL.md` §4).
2. `sensor ina219`, then `battery status` — confirm `Measurement source : HARDWARE` with sane readings. If it reports `NONE`/"not currently responding", check wiring before proceeding.
3. Re-check `dashboard` against the real reading. The `battery limits` policy
   set during simulation does not need to change unless the real battery's
   behavior genuinely differs from the sizing assumptions.

## 4. Commission Smart Nodes (if any)

For each Smart Node board at the site: flash it (`pio run -e smart -t upload`), power it up, and either commission it from Central's console/MQTT (`node commission MAC name=...` / `commands/config` `COMMISSION_NODE`) or configure its Loads directly at the node using its own local console (`USER_MANUAL.md` §4 "Smart Node console") if you're standing right at it.

## 5. Final checks before leaving site

- `status` / `state/nodes` — every expected node shows online.
- `loads` / `state/loads` — every configured Load matches what was agreed with the customer.
- Subscribe to `alerts` for a few minutes and confirm nothing unexpected fires (see `lib/MqttManager/README.md`).
- Leave the customer or their app pointed at `status` for availability monitoring (`USER_MANUAL.md` §"Availability").

See also: `USER_MANUAL.md`, `TECHNICAL_REFERENCE.md`, `lib/MqttManager/README.md`, `LIMITATIONS.md`.
