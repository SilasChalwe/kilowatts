# AvailablePowerManager

The power-accounting step that runs between `LoadFilter` and the future
Best-First Search stage: turns an externally supplied Total Available
Power figure into Power Available for Auto Loads.

## Responsibility

```cpp
AvailablePowerManager availablePowerManager;

if (availablePowerManager.calculateAvailablePower(totalAvailablePowerWatts, loadFilter)) {
    availablePowerManager.getTotalAvailablePowerWatts();
    availablePowerManager.getFixedOnRunningPowerWatts();
    availablePowerManager.getPowerAvailableForAutoLoadsWatts();
}
```

Three clearly named concepts:

- **Total Available Power** — the power currently available to the system
  for supplying Loads. `AvailablePowerManager` does not measure or
  calculate this: it is supplied by the caller as the
  `totalAvailablePowerWatts` argument. The real source will later be the
  Central Node's own power-source measurements (battery/solar) — that is
  a separate, later phase. Nothing in this module fabricates it.
- **Fixed ON Running Power** — recalculated from scratch on every call by
  traversing the already-classified `loadFilter`'s Fixed ON collection and
  summing `LoadPower::runningWatts`. This deliberately uses *configured*
  Running Power, never `LoadMeasurements.powerWatts` — a live INA219
  reading and a Load's configured Running Power serve different purposes,
  and this planning calculation always uses the latter.
- **Power Available for Auto Loads** — `Total Available Power - Fixed ON
  Running Power`, clamped to never go below zero (when Fixed ON Running
  Power is at or above Total Available Power, this becomes exactly `0`).

`calculateAvailablePower()` rejects (`false`, previous values left
unchanged) a `totalAvailablePowerWatts` that is negative, `NaN` or
infinite. `0` is a valid input.

## Boundaries

`AvailablePowerManager` is not a hardware driver. It does not read INA219
registers or know how Total Available Power was physically measured, does
not calculate battery State of Charge, battery voltage/current, solar
voltage/current, does not classify Loads (`LoadFilter` does that — this
module only traverses an already-classified one), and is not connected to
`BestFirstSearch` yet — this phase implements and tests the arithmetic in
isolation.

No placeholder Total Available Power value is written into production
code anywhere in this project. A controlled value (e.g. `100.0F`) is valid
inside a unit test, where a deterministic input is required — it must
never appear in `src/central/main.cpp` or `src/smart/main.cpp` presented
as if it came from real hardware.
