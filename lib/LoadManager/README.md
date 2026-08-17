# LoadManager

Load domain model, Fixed/Auto classification, persisted Load
configuration and schedule evaluation — grouped by domain, each class
still strictly single-responsibility per its own section below.

## Folder layout: Central / shared

`Load` is the shared domain object both firmwares build, so it stays
directly in this folder. `LoadFilter`, `LoadConfigurationStore` and
`LoadScheduleEvaluator` are Central-only (classification, persisted
config and schedule evaluation only ever run on Central's planning
cycle) and live under `Central/`. `platformio.ini`'s shared `[env]`
`build_flags` adds `-Ilib/LoadManager/Central` so a bare
`#include "LoadFilter.h"` from another library (e.g.
`AvailablePowerManager.h`) can still find it — see `lib/NodeManager`'s
README for why this extra include path is necessary.

---

## Load


Represents one electrical load connected to one ESP32 Node — the
fundamental domain object every other Load/Node-aware module in this
project builds on.

## Identity

A Load's identity is its owning Node's MAC address plus its relay pin,
never an arbitrary integer ID:

```cpp
struct Load::Id {
    MacAddress macAddress;   // the owning Node's real Wi-Fi MAC address
    std::uint8_t relayPin;   // which relay pin on that Node
};
```

This lets Loads from many different Nodes be combined, filtered, searched
and reported back through the network without losing track of which
physical Node — and which relay pin on that Node — each one belongs to.

## What one Load stores

```cpp
Load fan(
    Load::Id{sittingRoomMac, 18U},
    "Fan",
    LoadPower{18.0F, 25.0F},   // running watts, startup watts
    3U,                        // priority
    LoadMode::Auto::ON
);
```

- **`LoadMode`** — one combined value for both how the Load is controlled
  and its current state: `Fixed::ON`, `Fixed::OFF`, `Auto::ON`,
  `Auto::OFF`. There is no separate on/off flag alongside a separate
  fixed/auto flag — `setMode()`/`getMode()` always move all four
  combinations together, and `isFixed()`/`isAuto()`/`isOn()`/`isOff()`
  are derived from that one value.
- **`LoadPower`** — the *configured* running/startup wattage used for
  planning (`startupWatts` must never be lower than `runningWatts`;
  `setPower()` rejects a violation).
- **`LoadElectricalRatings`** — installer/nameplate voltage and current
  used to derive the configured running wattage for planning. In the final
  Kilowatts hardware design, Smart Nodes use this pair because the only
  INA219 is on Central's battery bus. These values are estimates, not live
  per-load consumption.
- **`LoadMeasurements`** — a generic optional live voltage/current/power
  record retained for hardware variants that actually measure individual
  loads. `setMeasurements()` rejects a non-finite or negative value rather
  than silently storing it. The current Smart-Node reporting path does not
  populate it, so UI code must not display it as live data.
- **`AutoSchedule`** — an optional preferred running time (`hour`,
  `minute`) for an Auto Load only; `setSchedule()` rejects the call when
  the Load is Fixed, and `setMode()` clears any schedule the moment a
  Load becomes Fixed.
- **Confirmed relay state** (`setConfirmedRelayState()`/
  `getConfirmedRelayState()`) — the relay state last read back from real
  hardware (`RelayController::readBackState()`), deliberately separate
  from `getMode()`/`isOn()` (the configured/commanded intent). A caller
  must never treat one as the other, especially while a relay command is
  in flight or has failed.
- **`LoadHealth`** (`setHealth()`/`getHealth()`) — `AVAILABLE`,
  `FAULTED` or `UNAVAILABLE`, as last reported by the owning Node. Also
  independent of `LoadMode`: health describes whether the physical relay
  can currently be trusted, not how it is configured.

## Boundaries

`Load` only stores and validates one load's own state. It does not decide
which Loads should be ON (`BestFirstSearch`), does not classify Loads into
Fixed/Auto groups (`LoadFilter`), does not know about other Loads on the
same Node (`Node`), and does not read real sensor hardware
(`INA219Monitor`).

---

## LoadFilter


Separates every Load Central currently knows about into Fixed ON, Fixed
OFF, and Auto candidate groups — the preparation step that runs before
Best-First Search. Classification is its only responsibility.

## Responsibility

```cpp
LoadFilter loadFilter;

for (/* every Load known by CentralNodeRegistry */) {
    loadFilter.addLoad(load);
}

loadFilter.getNumberOfFixedOnLoads();
loadFilter.getFixedOnLoad(i);

loadFilter.getNumberOfAutoCandidateLoads();
loadFilter.getAutoCandidateLoad(i);         // BestFirstSearch decides ON/OFF for these
```

`addLoad()` classifies by the Load's current `LoadMode`:

| Mode | Goes to |
|---|---|
| `Fixed::ON` | Fixed ON collection |
| `Fixed::OFF` | Fixed OFF collection |
| `Auto::ON` or `Auto::OFF` | Auto candidate collection |

A Fixed Load's ON/OFF state is authoritative and is not reconsidered here.
An Auto Load's current ON/OFF state is only this planning cycle's starting
point — `AUTO_ON` and `AUTO_OFF` are therefore both Auto *candidates*;
whether each ends up ON or OFF is decided later by `BestFirstSearch`, not
by `LoadFilter`.

`LoadFilter` stores **pointers** to the existing `Load` objects, not
copies, so every Load keeps the identity `CentralNodeRegistry` already
established. A `LoadFilter` must not outlive the Loads it points to.
Call `reset()` before re-traversing the registry (for example after a new
`NODE_REPORT`) so a fresh pass never mixes in Loads left over from an
earlier classification.

## Boundaries

`LoadFilter` classifies Loads only. It does not calculate Fixed ON Running
Power, Total Available Power, or Power Available for Auto Loads — that
power accounting now belongs to `AvailablePowerManager`, which receives an
already-classified `LoadFilter` and traverses its Fixed ON collection.
`LoadFilter` also does not discover Nodes or receive ESP-NOW packets
(`EspNowCommunication`), does not calculate RSSI or Hop Count, does not
calculate battery State of Charge, does not run Best-First Search, does
not control relays, and does not publish MQTT.

---

## LoadConfigurationStore


Remembers the user's own choices for every Load Central has ever been
told about — priority, Fixed/Auto x ON/OFF mode, Auto schedule — across a
reboot, so a stale ESP-NOW `NodeReportPacket` can never silently overwrite
a change the user made through the mobile application.

## Responsibility

```cpp
LoadConfigurationStore store;
store.loadPersisted();   // resume whatever was saved last (ESP32 target)

// When an MQTT load-command arrives (see MqttManager):
store.setConfiguration(LoadConfigurationStore::ConfigurationEntry{
    load.getMacAddress(), load.getRelayPin(),
    newPriority, newMode, newSchedule
});
store.persist();

// Every planning cycle, for every known Load (see CentralNodeRegistry):
store.applyToLoad(*load);   // no-op (returns false) if the user has never configured this Load
```

Central's planning cycle order matters: apply each Node's latest
`NodeReportPacket` first (telemetry/measurements, and a sane bootstrap
default the first time a Load is ever seen), *then* call
`applyToLoad()` for every Load so any persisted user configuration wins
before Fixed/Auto separation and Best-First Search run. This is the
mechanism that keeps a user's MQTT-driven choice authoritative even
though the owning Smart Node keeps periodically reporting whatever it
last knew locally.

`applyToLoad()` applies priority, then mode, then schedule — schedule is
applied through `Load::setSchedule()` itself, so a stored schedule on a
Load whose stored mode is Fixed is a harmless no-op (never promotes the
Load to Auto). It returns `false`, leaving the Load completely
unchanged, when no entry has ever been stored for that Load's identity.

`setConfiguration()` upserts (keyed by `{Node MAC address, relay pin}` —
the same addressing as `Load::Id`) rather than accumulating duplicates,
and rejects an enabled schedule with an out-of-range hour/minute.

## Persistence

Every entry is stored as one fixed-layout record inside a single NVS blob
(namespace `kw_loadcfg`), not one NVS key per Load, so the number of
remembered Loads is not limited by NVS's per-device key count. A
malformed or corrupt persisted blob is discarded (`loadPersisted()`
returns `false`, leaving this object's in-memory entries — typically
empty at that point — unchanged) rather than silently accepted.

## Host build vs. ESP32 target

Bookkeeping (`setConfiguration()`/`findConfiguration()`/`applyToLoad()`)
is plain, hardware-free C++ and always compiled, so it is directly
host-testable (see `test/LoadConfigurationStore/`). NVS persistence
(`loadPersisted()`/`persist()`) is compiled only under `ESP_PLATFORM`; a
host build always reports failure rather than fabricating durable
storage.

## Boundary

This class does not run Best-First Search, does not calculate power,
does not perform ESP-NOW/MQTT itself, and does not actuate a relay — it
purely remembers and reapplies user configuration onto an already-existing
`kilowatts::Load` object supplied by the caller.

---

## LoadScheduleEvaluator


Evaluates an Auto Load's configured schedule against real local time and
produces the schedule terms Chapter 4 defines — a_i, d_i and the
future-schedule penalty r_i (Section 4.6.3.3 / Equation 4.32) — for
`BestFirstSearch` to consume as one input to h(i). It is the last
preparation step before Load selection.

## Responsibility

```cpp
LoadScheduleEvaluator scheduleEvaluator;

LoadScheduleEvaluation result{};
if (scheduleEvaluator.evaluateSchedule(autoLoad, currentTimeProvider, result)) {
    // result.hasEnabledSchedule  -> a_i
    // result.isScheduledTimeDue  -> d_i
    // result.futureSchedulePenalty -> r_i, ready to hand to BestFirstSearch::addLoad()
}
```

- **Not an Auto Load** (`Fixed::ON`/`Fixed::OFF`) → `evaluateSchedule()`
  returns `false`. A schedule never overrides Fixed Load semantics, and a
  Fixed Load is never converted into an Auto Load here.
- **No schedule enabled** → `a_i = false`, `d_i = false`,
  `r_i = 0.0F`.
- **Schedule enabled but not yet due**, including when the clock is not
  currently synchronized (an unsynchronized clock can never confirm a
  schedule is due — it is treated exactly like "not yet due", never
  fabricated as "due") → `a_i = true`, `d_i = false`, `r_i = 1.0F`.
- **Schedule enabled and due** (current local hour/minute at or after the
  scheduled hour/minute) → `a_i = true`, `d_i = true`, `r_i = 0.0F`.

`r_i = a_i(1 - d_i)` exactly matches Equation 4.32. It is a schedule
penalty, not a priority — this class never reads or produces a priority
value. `Load::getPriority()` (`W_i`) is a completely separate term that
`BestFirstSearch` normalizes and weighs on its own (`q_i`, `w_Q`); the two
are only ever combined together inside
`BestFirstSearch::calculateHeuristicCost()`, never here. A Load's own
stored priority is therefore never read or modified by this class.

`AutoSchedule` (`enabled`, `hour`, `minute` — see `Load.h`) has no
duration or day-of-week field, so "due" means exactly what those three
fields can express: local time has reached or passed the scheduled
hour:minute for the current day. No seconds, weekdays, date ranges,
duration, calendar recurrence or timezone fields are invented here —
timezone handling belongs to `CurrentTimeProvider`, not this class.

## Testing

This module cannot be host-tested through `run_cpp_test.sh`: it calls
`CurrentTimeProvider`'s real methods, whose implementations require
ESP-IDF's SNTP/NVS headers and only exist in `CurrentTimeProvider.cpp`,
which is not compiled by a `LoadScheduleEvaluator`-scoped test run.
Schedule behavior must be verified on real ESP32 hardware with real
current time. `BestFirstSearch`'s own host test instead exercises how a
supplied `r_i` value feeds into h(i) directly, using controlled float
inputs rather than a real `LoadScheduleEvaluator`/`CurrentTimeProvider`
pair.

`CurrentTimeProvider`'s Manual mode (see its own README) is a real
production feature, not a test backdoor, but it does make on-device
verification practical without waiting on internet connectivity: put the
device in Manual mode with a real, deliberately chosen date/time
(`setTimeMode(TimeMode::MANUAL)` + `setManualCurrentDateTime()`), then
construct Auto Loads with schedules (via `Load::setSchedule()`) before,
equal to, and after that chosen time, and confirm `evaluateSchedule()`
reports `a_i=true, d_i=false, r_i=1.0F` for the "not yet due" cases and
`a_i=true, d_i=true, r_i=0.0F` for the "due" case — for both `Auto::ON`
and `Auto::OFF` — and that a `Fixed::ON` or `Fixed::OFF` Load with the
same schedule fields set is rejected (`false`) rather than evaluated. The
same check can be repeated in Automatic mode once a real NTP
synchronization has completed. Either way, `LoadScheduleEvaluator` itself
is never modified or given a fake clock to make this pass — only
`CurrentTimeProvider`'s own real, source-agnostic time is used.

## Boundaries

`LoadScheduleEvaluator` does not own the clock (`CurrentTimeProvider`
does), does not implement NTP itself, does not modify the system clock,
does not read INA219, does not run Best-First Search, and does not read or
modify a Load's stored priority. It only prepares the schedule terms
`BestFirstSearch` will eventually consume.

