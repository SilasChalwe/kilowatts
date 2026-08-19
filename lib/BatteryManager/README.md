# BatteryManager

Battery/power sensing and accounting — measurement (INA219Monitor),
state-of-charge estimation (BatteryStateOfCharge), Fixed-ON power
accounting (AvailablePowerManager) and the P_available/P_remaining
limit calculation (SafePowerLimitCalculator) — grouped by domain, each
class still strictly single-responsibility per its own section below.

---

## BatteryStateOfCharge


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
`SafePowerLimitCalculator`'s and `BestFirstSearch`'s configuration), does not
calculate the power limit, and does not run Best-First Search.

---

## INA219Monitor


Manages every INA219 current/power sensor physically wired to the current
Node, on one shared I2C bus, and produces real `LoadMeasurements` for the
Loads those sensors measure.

## Responsibility

A Node can have several Loads, and individual Loads can each have their own
INA219 sensor. `INA219Monitor` is one object per Node that manages every
INA219 on that Node — not one monitor object per sensor:

```cpp
INA219Monitor powerMonitor;
powerMonitor.initializeBus(busConfiguration);
powerMonitor.addSensor(sensorConfigurationA);
powerMonitor.addSensor(sensorConfigurationB);
```

It initialises the I2C bus, registers/configures multiple INA219 devices,
identifies each by its I2C address, associates each with the relay pin of
the Load it measures, and reads real bus voltage, current and power.

It does **not** perform ESP-NOW communication, know remote Node topology,
store a Node MAC address, run Best-First Search, filter Loads, calculate
system Available Power, perform MQTT, control relays, or decide which
Loads should be ON. Node ownership of a sensor's readings is already known
by the surrounding Node/Load architecture — INA219Monitor only needs the
relay pin of the Load a sensor is wired to, never a MAC address:

```
INA219 address 0x40
      |
relay GPIO16
      |
Node.getLoadByRelayPin(16)
      |
LoadMeasurements updated
```

That last step (looking a Load up on the Node and calling
`Load::setMeasurements()`) is the caller's job, not INA219Monitor's — this
module exposes measurements, it does not own or reach into a `Node`.

`INA219Monitor` is used identically by a Smart Node or by the Central Node
when Central has its own directly-wired Loads; nothing about it is
Smart-only.

## Sensor identity

Locally, one INA219 is identified only by:

- its **I2C address** (device identity on the bus), and
- the **relay pin** of the Load it measures (Load association).

Never a sensor MAC address, a Node MAC address, or an arbitrary integer ID.

```cpp
struct INA219SensorConfiguration {
    std::uint8_t i2cAddress;
    std::uint8_t relayPin;
    float shuntResistanceOhms;
    float maximumExpectedCurrentAmps;
    float emaAlpha;                          // 0 < alpha <= 1 (Equations 4.2-4.4)
    LoadMeasurements developmentMeasurement;  // used only in development mode
};
```

`shuntResistanceOhms` and `maximumExpectedCurrentAmps` are physical
properties of the sensor board wired to that relay pin. They are required
arguments, never defaulted or guessed — correct calibration depends on
them (see below).

`addSensor()` rejects: an address outside the INA219's real address space
(0x40-0x4F), a duplicate I2C address, a duplicate relay pin, a
non-positive shunt resistance or max current, a shunt/current combination
whose full-scale shunt voltage exceeds the sensor's measurable range, and
an `emaAlpha` outside `(0, 1]`.

## Development vs. production sensor source

`KILOWATTS_DEVELOPMENT_MODE` (`include/DevelopmentMode.h`) changes only
*where* `readMeasurements()` gets its raw voltage/current/power — every
other method, and everything downstream of it (calibration, EMA
filtering, `Load::setMeasurements()`, battery SoC, the power limit,
Best-First Search), is the identical production code path in both modes:

- **Development** (`KILOWATTS_DEVELOPMENT_MODE = 1`): returns each
  sensor's configured `developmentMeasurement` instead of performing a
  real I2C transaction. Logged as `SENSOR INPUT SOURCE: DEVELOPMENT` —
  never presented as if it came from real hardware.
- **Production** (`KILOWATTS_DEVELOPMENT_MODE = 0`): performs the real
  I2C register communication described below. Logged as
  `SENSOR INPUT SOURCE: INA219 HARDWARE`.

This switch only ever takes effect while running as real firmware on the
ESP32 target (inside the `ESP_PLATFORM` branch) — a plain host build
still always returns `false` from every hardware-touching method
regardless of `KILOWATTS_DEVELOPMENT_MODE`, since there is no firmware
runtime there to be "in development mode" within (see
`test/INA219Monitor/`'s "host build never fabricates hardware" test,
which passes unchanged either way).

## Measurement flow

```cpp
LoadMeasurements filtered{};
if (powerMonitor.readFilteredMeasurementsForRelayPin(16U, filtered)) {
    Load *load = node.getLoadByRelayPin(16U);
    if (load != nullptr) load->setMeasurements(filtered);
}
```

`readMeasurements()`/`readMeasurementsForRelayPin()` return the
*instantaneous* reading (Equation 4.1); `readFilteredMeasurements()`/
`readFilteredMeasurementsForRelayPin()` additionally fold it into that
sensor's running Exponential Moving Average (`P_bar(t) = alpha P(t) + (1 -
alpha) P_bar(t-1)`, Equations 4.2-4.4) — this filtered snapshot, not the
instantaneous one, is what the rest of the system (battery SoC, the power
limit, Best-First Search) must consume (Section 4.6.1.1). Both return
`false` — and leave their output unchanged — on any I2C failure, on an
unregistered address/relay pin, or when the sensor's own math-overflow
flag is set. INA219Monitor never fabricates a voltage/current/power
value, and never reports success for a reading that did not actually
happen.

Calibration offsets/scaling factors (`setCalibration()`/
`getCalibration()`, System Requirement 3) are applied to the raw reading
*before* filtering: `correctedVoltage = raw + voltageOffsetVolts`,
`correctedCurrent = (raw + currentOffsetAmps) * currentScaleFactor`,
power recomputed from the corrected values. `setCalibration()` rejects a
non-finite offset or a non-positive scale factor, and persists the result
to NVS (namespace `kw_ina219`, ESP32 target only) so it survives a reboot
or power interruption; a corrupt persisted value is discarded (falling
back to the identity calibration `{0, 0, 1}`) rather than silently
accepted.

Live INA219 readings are not the same thing as a Load's configured Running
Power / Startup Power (`LoadPower`). This module only ever writes
`LoadMeasurements`; it never touches `LoadPower`.

## Host build vs. ESP32 build

`INA219Monitor.h` includes no ESP-IDF header — bus/device handles are
stored internally as `void*`. `INA219Monitor.cpp` is split on
`ESP_PLATFORM` (defined by every ESP-IDF build):

- **Always compiled**: `addSensor()`'s validation and bookkeeping,
  `getSensor()`/`findSensorByI2CAddress()`/`findSensorByRelayPin()`. Pure
  C++, no I2C, host-testable — see `test/INA219Monitor/`.
- **Compiled only under `ESP_PLATFORM`**: `initializeBus()`,
  `isSensorPresent()`, `readMeasurements()`, `printDiagnosticReport()`, and
  the register I/O helpers. A host build's version of every one of these
  simply returns `false` (or does nothing) — it never simulates hardware.

This mirrors how `Node`/`Load` are hardware-free and host-tested today
while `ChipInfo`/`EspNowCommunication` are ESP-IDF-only.

## ESP-IDF I2C API used

The new I2C **master** driver, `driver/i2c_master.h` (ESP-IDF v5.2+,
confirmed present in the installed ESP-IDF v5.5.4):

- `i2c_new_master_bus()` / `i2c_del_master_bus()` — one bus per Node.
- `i2c_master_bus_add_device()` / `i2c_master_bus_rm_device()` — one
  device handle per registered INA219 address.
- `i2c_master_probe()` — presence check (FOUND/NOT FOUND).
- `i2c_master_transmit()` — register writes (Configuration, Calibration).
- `i2c_master_transmit_receive()` — register reads (register pointer byte,
  repeated start, 2 data bytes back).

## Register map (TI INA219 datasheet, SBOS448)

| Register | Address | Used for |
|---|---|---|
| Configuration | 0x00 | Written once per sensor: `0x399F` — BRNG=32V, PGA=/8 (+-320mV full-scale), 12-bit/532us conversion, continuous shunt+bus. This is the datasheet's own POR default, written explicitly. |
| Calibration | 0x05 | Written once per sensor: `trunc(0.04096 / (Current_LSB x shuntResistanceOhms))`, where `Current_LSB = maximumExpectedCurrentAmps / 32768`. |
| Bus Voltage | 0x02 | Read every measurement. Bits [15:3] = data, 4 mV/LSB. Bit 1 = math-overflow flag (OVF) — a set OVF discards the sample. |
| Shunt Voltage | 0x01 | Read every measurement as a sanity check: signed, 10 uV/LSB. A magnitude at or beyond +-320mV discards the sample even if OVF was not set. |
| Current | 0x04 | Read every measurement: signed, `Current_LSB` per LSB. |
| Power | 0x03 | Read every measurement: unsigned, `Power_LSB = 20 x Current_LSB` per LSB. |

None of these constants (0.04096, 20x, 4 mV, 10 uV, the register
addresses, or `0x399F`) are invented — they are the datasheet's own
formulas and default register value. The two values this module cannot
know on its own — `shuntResistanceOhms` and `maximumExpectedCurrentAmps` —
must be supplied by the caller's real hardware configuration.

## Required hardware values (PENDING HARDWARE VERIFICATION)

`SmartNodeConfig.h` and `CentralNodeConfig.h` now supply every value a
real `INA219Monitor` needs for each Node — I2C pins/port, and per-sensor
I2C address, shunt resistance and maximum expected current — so the
production code path is complete and compiles/runs today. What is still
pending is *physical confirmation* of those specific numbers against the
real boards once they are wired, not missing implementation:

- I2C SDA pin, I2C SCL pin, and I2C port number for that Node.
- For every physically wired INA219: its I2C address, the relay pin of the
  Load it measures, the shunt resistor value (ohms) on that specific
  breakout board, and the maximum current that board/Load is expected to
  draw.

## First real hardware test checklist

1. Wire the INA219 breakout's SDA/SCL to the Node's chosen I2C pins, with
   pull-ups (either the board's own or the driver's internal pull-ups).
2. Confirm the board's shunt resistor value and its address-select
   strapping (A0/A1) with a multimeter or the board's silkscreen/datasheet
   — do not assume 0x40 or a 0.1 ohm shunt.
3. Call `initializeBus()` once with the real pins, then `addSensor()` once
   per physically wired INA219 with its real address, relay pin, shunt
   resistance and max current.
4. Call `printDiagnosticReport()` and confirm every configured address
   reports FOUND with a plausible voltage/current/power before wiring this
   into the Node/Load reporting path.

---

## AvailablePowerManager


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

---

## SafePowerLimitCalculator


Calculates the safe total power limit for one planning cycle
(Section 4.6.2.3, Equations 4.9-4.14) — the `totalAvailablePowerWatts`
`BestFirstSearch` and `AvailablePowerManager` both consume.

## Responsibility

```cpp
SafePowerLimitCalculator::Inputs inputs{};
inputs.stateOfChargePercent = batteryStateOfCharge.getStateOfChargePercent();
inputs.minimumStateOfChargePercent = config.socMinPercent;
inputs.nominalBatteryVoltageVolts = config.nominalBatteryVoltageVolts;
inputs.batteryCapacityAmpHours = config.batteryCapacityAmpHours;
inputs.targetRuntimeHours = config.targetRuntimeHours;
inputs.batteryBusVoltageVolts = filteredBatteryMeasurements.voltageVolts;
inputs.maximumBatteryDischargeCurrentAmps = config.maximumBatteryDischargeCurrentAmps;
inputs.maximumMainCurrentAmps = config.maximumMainCurrentAmps;
inputs.safetyFactor = config.safetyFactor;

SafePowerLimitCalculator limit;
limit.calculate(inputs);

float pAvailable = limit.getAvailablePowerWatts();   // -> AvailablePowerManager::calculateAvailablePower()
```

```
E_rated   = V_nom * C_B                                          (4.9)
E_usable  = E_rated * max(0, (SoC - SoC_min) / 100)               (4.10)
P_runtime = E_usable / T_target                                   (4.11)
P_battery,max = V_B * I_B,max                                     (4.12)
P_main,max    = V_B * I_main,max                                  (4.13)
P_available   = rho * min(P_runtime, P_battery,max, P_main,max)   (4.14)
```

When `SoC <= SoC_min`, Equation 4.10's `max(0, ...)` clamp already forces
`E_usable`, and therefore `P_runtime` and `P_available`, to zero — the
"`P_available` = 0 for normal Auto-load allocation" rule Section 4.6.2.3
states in words falls directly out of these equations, so one uniform
calculation path (not a special-cased branch) produces a consistent
`P_available` for the whole planning cycle regardless of battery state
(see `test/SafePowerLimitCalculator/`'s SoC-at-or-below-minimum test).

`calculate()` rejects (previous result, if any, unchanged): a
`stateOfChargePercent`/`minimumStateOfChargePercent` outside `[0, 100]`,
a non-positive `nominalBatteryVoltageVolts`/`batteryCapacityAmpHours`/
`targetRuntimeHours`/`batteryBusVoltageVolts`, a negative current limit,
a `safetyFactor` outside `(0, 1]`, or any non-finite input.

## Boundary

Battery policy fields (`SoC_min`, `V_nom`, `C_B`, `T_target`, current
limits, `rho`) are configuration this class never invents defaults for —
they come from `CentralNodeConfig.h`. `stateOfChargePercent` comes from
`BatteryStateOfCharge`; `batteryBusVoltageVolts` comes from the central
INA219's filtered measurement (`INA219Monitor`). This class does not read
hardware, does not perform Fixed-Load allocation
(`AvailablePowerManager`), and does not run Best-First Search.

