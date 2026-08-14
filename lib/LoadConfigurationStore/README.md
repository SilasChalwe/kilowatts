# LoadConfigurationStore

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
