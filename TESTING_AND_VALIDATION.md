# Testing and Validation

This document records a hardware-in-the-loop validation session run against
the physical Central Node, covering: a relay-polarity defect that was found,
diagnosed and fixed on the real device, followed by six test cases that
exercise the Best-First Search load-selection algorithm and the power-budget
model against real, physically-connected relays.

Every result below was produced by sending live commands to the Central
Node's serial console and having the physical relay state confirmed by
visual inspection after each step — this is not a simulation of the
firmware, it is the firmware itself, running on the actual board, switching
actual relays.

## 1. Test environment

| Item | Value |
|---|---|
| Node under test | Central Node |
| MCU | ESP32-D0WDQ6 revision 1.0 |
| MAC address | `A4:CF:12:0E:32:C0` |
| Firmware environment | `pio run -e central` |
| Interface | Serial console, 115200 baud, `/dev/ttyUSB1` |
| Battery input source | `sensor sim` (simulated INA219 readings, same code path as real hardware — see §4) |
| Relay channels under test | GPIO 2, 4, 18, 25 |
| Relay hardware | Active-low relay boards (energise on GPIO LOW) |

Configured loads at the start of testing:

| Pin | Name | Power | Priority | Type | Initial mode |
|---|---|---|---|---|---|
| 4 | Lamp | 10.00 W | 1 | DC | FIXED_OFF |
| 2 | Load2 | 10.00 W | 2 | DC | AUTO_OFF |
| 18 | Load18 | 10.00 W | 3 | DC | AUTO_OFF |
| 25 | Load25 | 10.00 W | 1 | DC | AUTO_OFF |

Power limits configured for all tests except where a step deliberately
changes them: `min_soc=20%`, `max_discharge_amps=10A`, `max_main_amps=15A`,
`runtime_hours=24`.

## 2. Defect found and fixed: relay polarity

### 2.1 Symptom

All four relays (pins 2, 4, 18, 25) behaved as if permanently active-high:
regardless of whether a Load was set FIXED_ON or FIXED_OFF, the physical
relay stayed energised (LED on). Commanding a Load OFF turned its relay ON.

### 2.2 Root cause

The polarity-inversion logic itself was correct
(`lib/RelayManager/RelayController.cpp:277`):

```cpp
const bool driveHigh = relay.configuration.activeHigh ? on : !on;
```

For `activeHigh = false` (active-low), this correctly drives the GPIO LOW
to turn a relay on and HIGH to turn it off. The defect was in the **stored
configuration**, not the code: all four relays had been registered with
`relayActiveHigh = true`, even though the physical boards are active-low.
With that flag wrong, "commanded OFF" produced GPIO LOW, which energises an
active-low relay — precisely the reported symptom.

Every entry point that sets this flag (`load add active_high=on|off` at the
console, the equivalent MQTT `relayActiveHigh` field, and the ESP-NOW
packet forwarded to a Smart Node) requires the value explicitly and applies
no default — so the mistake was a value supplied once at Load-creation
time, not a code path that silently guesses wrong.

### 2.3 Fix applied

1. Added a `Relay polarity` / `Relay applied` line to the `load show`
   console command
   (`src/central/namespace.h:2837-2854`) so the actual stored polarity per
   pin is directly observable — previously `RelayController::printDiagnosticReport()`
   existed but was never called from anywhere, so this information was not
   visible at all.
2. Re-registered all four loads on the live device
   (`load remove` + `load add ... active_high=off ...`), correcting the
   stored polarity while preserving each Load's name, power rating,
   priority and mode.

### 2.4 Verification

```
kilowatts > load show 4
...
Relay polarity : active-LOW
Relay applied  : yes
```

confirmed for all four pins. Physically: all four relays dropped OFF the
moment the corrected records were written (previously all four had been
stuck ON). User-confirmed.

## 3. Algorithm validation test cases

Each test was run with `optimize run` (forces an immediate planning cycle)
after adjusting either the simulated battery reading (`simulation values`)
or the configured power limits (`battery limits`). Relay state was
confirmed visually after each run.

### Test 1 — Baseline AUTO admission

**Setup:** `sensor sim`, `simulation start`,
`simulation values voltage=12.6 current=0 soc=80`, all loads at their
initial mode (Lamp FIXED_OFF, the rest AUTO_OFF).

**Dashboard:**

```
AUTO available power  : 37.75 W
Fixed ON power         : 0.00 W
AUTO selected power    : 30.00 W
AUTO remaining power   : 7.75 W
Fixed ON / OFF          : 0 / 1
AUTO selected           : 3 / 3
```

**Expected:** healthy battery, no committed load → all three AUTO loads fit
comfortably and are admitted.

**Observed on relays:** pins 2, 18, 25 ON; pin 4 OFF. ✅ Matches.

### Test 2 — FIXED_ON deducted before AUTO, priority tie-break

**Setup:** pin 4 (Lamp) set to `mode=FIXED_ON`, same battery reading as
Test 1.

**Dashboard:**

```
AUTO available power  : 27.93 W
Fixed ON power          : 10.00 W
AUTO selected power     : 20.00 W
AUTO remaining power    : 7.93 W
Fixed ON / OFF           : 1 / 0
AUTO selected            : 2 / 3
```

**Expected:** Lamp's 10 W is deducted from the sustainable total *before*
the AUTO budget is derived (this is the same rule proven on host in
`test/main.cpp`, `caseFixedOnDeductedBeforeAuto`). With ~28 W left, only
two of the three 10 W AUTO loads can fit. Best-First Search breaks the tie
using accumulated priority
(`lib/BestFirstSearch/BestFirstSearch.cpp:105-108`: "a larger accumulated
priority wins"). Candidate pairs and their priority sums:
`{Load2(2)+Load18(3)}=5`, `{Load2(2)+Load25(1)}=3`, `{Load18(3)+Load25(1)}=4`
— `{Load2, Load18}` should win.

**Observed on relays:** pins 4, 2, 18 ON; pin 25 OFF. ✅ Matches the
predicted priority-sum winner exactly.

### Test 3 — SoC at/below the minimum reserve

**Setup:** pin 4 back to FIXED_ON (kept from Test 2),
`simulation values voltage=12.0 current=0 soc=15` (below the 20% reserve).

**Dashboard:**

```
AUTO available power  : 0.00 W
Fixed ON power          : 10.00 W
Required runtime        : sustainable 0.00 W | NOT ACHIEVABLE
Fixed ON / OFF           : 1 / 0
AUTO selected            : 0 / 3
```

**Expected:** per `PowerManager.cpp:1631-1638`, once SoC is at or below the
configured minimum, `P_available` is forced to exactly 0 — but this clamp
applies only to the AUTO allocation. A Fixed Load's relay state is read
directly from its configured mode (`Load::isOn()`), never gated by the
power budget — the code comment in `Load.h` states Fixed selection is
"a setting, changed only by the user," and `PowerManager.cpp:1621-1629`
explicitly notes Fixed ON loads are represented by `P_committed` only and
are not throttled here.

**Observed on relays:** pin 4 still ON; pins 2, 18, 25 OFF. ✅ Matches —
demonstrates the Fixed/AUTO override boundary is enforced correctly on
real hardware, not just in the host test suite.

### Test 4 — Max-discharge-current ceiling

**Setup:** pin 4 back to FIXED_OFF, healthy battery restored
(`voltage=12.6 soc=80`), `battery limits max_discharge_amps=1` (all other
limits unchanged).

**Dashboard:**

```
AUTO available power  : 12.60 W
AUTO selected power     : 10.00 W
AUTO selected            : 1 / 3
```

**Expected:** `PowerManager.cpp:1511-1514` computes
`P_battery_max = V_battery × maximumDischargeCurrentAmps` — a **static**
ceiling from the *configured* limit, not the live measured/simulated
current. At 12.6 V and a 1 A configured ceiling, `P_battery_max = 12.6 W`,
which becomes the binding constraint (below the ~38 W the SoC/runtime
model would otherwise allow). Only one 10 W load fits; Best-First Search
should pick the single highest-priority Load, Load18 (priority 3).

**Observed on relays:** pin 18 ON only. ✅ Matches, and the number matches
the `V × I_max` formula exactly (12.60 W predicted vs. 12.60 W reported).

**Note for the record:** an earlier attempt to trigger this same ceiling by
simulating a large *live* current draw (up to ~20 A, double the configured
10 A limit) had **no effect** on the AUTO budget at all. This is correct,
documented behaviour, not a bug — see §4.

### Test 5 — Required-runtime target becomes unachievable

**Setup:** discharge limit restored to `10 A`, pin 4 set to `FIXED_ON`,
`battery limits runtime_hours=200` (a deliberately unreachable target),
healthy battery (`voltage=12.6 soc=80`).

**Dashboard:**

```
Required runtime      : 200.00 h remaining | sustainable 4.50 W | NOT ACHIEVABLE
Fixed ON power          : 10.00 W
AUTO available power    : 0.00 W
Fixed ON / OFF            : 1 / 0
```

**Expected:** matches `test/main.cpp`'s `caseFixedOnExceedsSustainableTotal`
exactly: when the committed (Fixed ON) power exceeds what the runtime
target can sustain, the sustainable total collapses (here to 4.50 W), the
committed 10 W figure is reported back **unchanged** — the firmware never
silently clips or hides a Fixed commitment to make the runtime target look
achievable — `requiredRuntimeAchievable` is reported false, and the AUTO
budget is 0.

**Observed on relays:** pin 4 ON only; pins 2, 18, 25 OFF. ✅ Matches.

### Test 6 — AUTO schedule can override a Load's own priority ranking

**Setup:** same tight single-slot budget as Test 4
(`max_discharge_amps=1` → 12.60 W, room for exactly one 10 W Load), so the
winner is unambiguous. Under this budget with no schedules set, the
priority-highest Load (pin 18, priority 3) wins, as in Test 4.

**Step A:** `load set <MAC> 18 schedule=23:59` (a schedule that is not yet
due at any normal time of day), then `optimize run`.

**Result:** the winner **switched** from pin 18 (priority 3) to pin 2
(priority 2) — a *lower*-priority Load displaced the highest-priority one.

**Step B:** `load set <MAC> 18 schedule=00:00` (a schedule that is due at
essentially any time of day past midnight), then `optimize run`.

**Result:** the winner switched back to pin 18 — priority-driven selection
was restored.

**Why this happens — traced against the source, not assumed:**
`futureSchedulePenalty` is *not* used by the final tie-break function
(`isBetterReturnedCombination`, `BestFirstSearch.cpp:119-130`, which only
compares combination size then total priority) — reading only that
function, schedule should never affect the winner. But the schedule
penalty does feed into `calculateH()`
(`BestFirstSearch.cpp:286-329`), which sets `f = g + h`
(`calculateF`, `BestFirstSearch.cpp:332-335`), which controls the order
states are popped from the open list
(`std::stable_sort(..., hasBetterSearchMerit)`, `BestFirstSearch.cpp:187-195`).
The search loop has an early-exit: **the very first state popped that
cannot be extended further (no remaining Load fits the leftover budget)
is immediately taken as the final answer** (`BestFirstSearch.cpp:239-242`):

```cpp
if (!hasFeasibleChild) {
    bestCombination_ = currentState.combination;
    return;
}
```

With all three single-Load states tied on power (and normally tied on
`f`), the priority tie-break in `hasBetterSearchMerit` puts the
highest-priority Load first in the queue, so it is popped, found to be a
dead end (budget only fits one Load), and immediately returned — which is
why priority normally decides the winner in a tight budget. Adding the
schedule penalty to pin 18 alone raises *only its* `f`, so it no longer
sorts first; a lower-priority Load is popped first instead, is *also* a
dead end under this budget, and gets locked in immediately — before pin 18
is ever even considered.

**Assessment for the record:** this is a real, reproducible interaction,
confirmed on physical relays both directions (pins 4,2,18,25 photographed/
observed via console + physical check each way). Whether "a Load with an
active but not-yet-due schedule should be able to lose its ranking to a
lower-priority Load with no schedule" is the *intended* behaviour per the
dissertation's Chapter 3/4 algorithm definition, or an unreviewed side
effect of the early-exit-on-first-dead-end optimisation, is something to
check against that formal definition — the mechanism itself is now
precisely identified and reproducible either way.

### Summary table

| # | Scenario | Predicted | Observed | Result |
|---|---|---|---|---|
| 1 | Baseline AUTO admission | 3/3 AUTO admitted | Pins 2,18,25 ON, 4 OFF | ✅ PASS |
| 2 | FIXED_ON deducted + priority tie-break | {Load2,Load18} win (priority sum 5) | Pins 4,2,18 ON, 25 OFF | ✅ PASS |
| 3 | SoC ≤ reserve | AUTO→0, Fixed ON unaffected | Pin 4 ON, 2/18/25 OFF | ✅ PASS |
| 4 | Discharge-current ceiling | Budget = V×I_max = 12.60 W, 1 load fits | Pin 18 ON only, 12.60 W reported | ✅ PASS |
| 5 | Runtime target unachievable | sustainable=4.50W, commitment unclipped, NOT ACHIEVABLE | Pin 4 ON only | ✅ PASS |
| 6 | Schedule vs. priority interaction | Not-due schedule can flip the winner away from highest priority | pin18→pin2 (not due), pin2→pin18 (due) | ⚠️ CONFIRMED BEHAVIOUR — verify against Ch.3/4 spec |

## 4. Design note: static current limit vs. live current reading

`PowerManager::updatePowerBudget()` (`lib/BatteryManager/PowerManager.cpp:1489`)
deliberately does not use the live measured/simulated
`measurements_.currentAmps` anywhere in the power-budget calculation. The
electrical ceiling (`P_battery_max`, `P_main_max`) is derived only from the
**configured** `maximumDischargeCurrentAmps` / `maximumCurrentAmps` values
multiplied by the measured voltage. The live current reading is recorded
into the dashboard purely for display (`battery power`, `battery current`).

This is a correct design choice — the configured limits represent the
wiring/battery's rated capability, a fixed safety ceiling, not a reactive
throttle — but it is worth stating explicitly in a validation write-up so
it is clear this was checked and is intentional, rather than an oversight
discovered and left unaddressed.

## 5. Post-test state

After the final test the device was returned to a normal operating
baseline: `battery limits min_soc=20 max_discharge_amps=10 max_main_amps=15
runtime_hours=24`, Lamp (pin 4) set back to `FIXED_OFF`, healthy simulated
battery (`voltage=12.6 soc=80`). Result after one `optimize run`: pins 2,
18, 25 ON, pin 4 OFF — consistent with Test 1.
