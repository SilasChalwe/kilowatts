# BatteryStateOfCharge

Maintains the battery State of Charge (SoC) estimate by coulomb counting
(Section 4.6.2.2, Equation 4.8), and persists it across reboots.

## Responsibility

```cpp
BatteryStateOfCharge soc;
soc.initialize(/* batteryCapacityAmpHours = */ 100.0F, /* defaultStateOfChargePercent = */ 80.0F);

// Every measurement cycle, using the central INA219's filtered battery
// current (discharge positive) and the elapsed interval:
soc.update(batteryCurrentAmps, deltaTimeSeconds);

float currentSoc = soc.getStateOfChargePercent();

// Periodically (e.g. once per Optimisation Task cycle):
soc.persist();
```

```
SoC(t) = clamp[ SoC(t-Dt) - 100 I_B(t) Dt / (3600 C_B), 0, 100 ]   (4.8)
```

`I_B(t)` is the battery current in amperes with **discharge positive** —
a negative (charging) current therefore *increases* the estimate. `Dt` is
the elapsed interval in seconds since the previous `update()`. `C_B` is
the configured battery capacity in ampere-hours, supplied once to
`initialize()`.

On the ESP32 target, `initialize()` first checks for a valid SoC
persisted by an earlier `persist()` call and resumes from it — "the value
stored in non-volatile memory provides the initial estimate after
restart" (Section 4.6.2.2) — falling back to
`defaultStateOfChargePercent` only when nothing valid has ever been
persisted. A corrupt persisted value is discarded rather than accepted. A
host build has no persistent storage, so it always starts from
`defaultStateOfChargePercent`.

`update()` is rejected (SoC left unchanged) before `initialize()`, or
when `batteryCurrentAmps` is not finite or `deltaTimeSeconds` is not a
finite positive value. The result is always clamped to `[0, 100]`.

## Host build vs. ESP32 target

The coulomb-counting math in `update()`/`initialize()`'s default-fallback
path is plain, hardware-free C++, always compiled and fully exercised by
`test/BatteryStateOfCharge/`. NVS persistence (namespace `kw_battery`) is
compiled only under `ESP_PLATFORM`; a host build's `persist()` always
reports failure rather than fabricating durable storage, matching the
pattern already used by `CurrentTimeProvider` and `INA219Monitor`'s
calibration storage.

## Boundary

This module does not read INA219 hardware itself — the caller (Central's
Sensor Acquisition/Optimisation Task, via `INA219Monitor`) supplies the
already-measured/filtered battery current and elapsed interval. It does
not know `SoC_min`/`SoC_warn` policy thresholds (those are
`PowerBudgetCalculator`'s and `BestFirstSearch`'s configuration), does not
calculate the safe power budget, and does not run Best-First Search.
