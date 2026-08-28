# QA Report: Load Selection and Power Budget Scenarios

## Scope
This report documents the verification status of the Kilowatts load-selection logic and the modeled scenario coverage for representative Home, Mine, and Hospital environments.

## Verified Baseline

### Build verification
Command run:

```bash
pio run -e central
```

Result:
- Central firmware build succeeded
- Status: PASS

### Logic verification
Command run:

```bash
bash test/run_tests.sh
```

Result:
- 49 checks
- 0 failed
- Status: PASS

This verifies the core planner logic in:
- [lib/BestFirstSearch/BestFirstSearch.cpp](lib/BestFirstSearch/BestFirstSearch.cpp)
- [test/main.cpp](test/main.cpp)

## Verified Behavior
The implemented algorithm confirms the following behavior:

1. The planner rejects any load combination that would exceed the available power budget.
2. Higher-priority loads are favored when power is constrained.
3. Multiple feasible loads may be selected together if they fit inside the active power cap.
4. Fixed-load power is accounted for before automatic-load planning.
5. The selected set is the best feasible option under the search rules and budgets.

This is algorithmic verification, not hardware-validated battery savings measurement.

## Distinction: Verified vs Assumed

### Verified
- The planner enforces the power budget.
- The planner picks the highest-priority feasible combination.
- The planner is validated by the passing test suite.

### Assumed / modeled
- The specific battery savings in watt-hours or runtime extension.
- The exact benefit for any particular installation.
- Real-world field performance under battery degradation, environmental conditions, and load variation.

These modeled values are included only to provide realistic scenario examples for QA planning and engineering review.

## Scenario Matrix

### 1) Home Scenario
Category: Residential / light commercial
Environment: Connected home energy management
Assumed available power budget: 180 W

Loads (12 total):
- Lights: 40 W, priority 7
- Refrigerator: 120 W, priority 9
- Water pump: 100 W, priority 8
- Router: 15 W, priority 5
- TV: 80 W, priority 6
- Laptop: 60 W, priority 4
- Washing machine: 150 W, priority 3
- HVAC fan: 90 W, priority 7
- Charger: 20 W, priority 5
- Microwave: 110 W, priority 3
- Space heater: 180 W, priority 2
- Electric kettle: 150 W, priority 2

Expected planner behavior:
- It will prefer essential and higher-priority loads.
- It will drop lower-priority or non-critical loads before exceeding the budget.
- Example: lights + water pump + router + charger may be preferred over a low-priority heavy appliance.

Assumption for modeled benefit:
- Avoided nonessential load: 120 W
- Estimated saved energy over 4 h: approximately 480 Wh
- Estimated saved energy over 8 h: approximately 960 Wh

### 2) Mine Scenario
Category: Industrial / harsh environment
Environment: Remote mine support / ventilation and pumping
Assumed available power budget: 220 W

Loads (12 total):
- Ventilation fan: 110 W, priority 30
- Water pump: 95 W, priority 25
- Conveyor system: 90 W, priority 18
- Drilling rig: 180 W, priority 28
- Lighting tower: 60 W, priority 12
- Compressor: 130 W, priority 24
- Sensor array: 30 W, priority 11
- Charging station: 50 W, priority 10
- Safety beacon: 20 W, priority 32
- Air scrubber: 85 W, priority 22
- Dehumidifier: 70 W, priority 16
- Emergency alarm: 25 W, priority 35

Expected planner behavior:
- Safety and ventilation loads are prioritized over nonessential equipment.
- Non-critical loads are cut first when the budget is constrained.
- Example: ventilation + water pump + safety beacon + sensor array should be favored over lower-priority conveyor or lighting loads.

Assumption for modeled benefit:
- Avoided lower-priority load: 90 W
- Estimated saved energy over 6 h: approximately 540 Wh
- Estimated saved energy over 12 h: approximately 1,080 Wh

### 3) Hospital Scenario
Category: Critical infrastructure / patient care
Environment: Hospital support with life-safety priority
Assumed available power budget: 350 W

Loads (12 total):
- Ventilator: 220 W, priority 40
- Patient monitor: 120 W, priority 35
- ICU lights: 90 W, priority 18
- Laundry: 150 W, priority 7
- Imaging equipment: 200 W, priority 22
- Refrigerator: 80 W, priority 16
- UPS system: 140 W, priority 27
- Air handling: 110 W, priority 25
- Nurse station: 70 W, priority 20
- Elevator backup: 160 W, priority 12
- Lab freezer: 100 W, priority 19
- Sterilizer: 180 W, priority 15

Expected planner behavior:
- Critical care and patient-support equipment is prioritized.
- Low-priority convenience or nonessential loads are deferred or rejected.
- Example: ventilator + patient monitor + UPS + air handling should be favored over laundry or imaging loads when the budget is tight.

Assumption for modeled benefit:
- Avoided lower-priority load set: 240 W
- Estimated saved energy over 4 h: approximately 960 Wh
- Estimated saved energy over 8 h: approximately 1,920 Wh

## Interpretation
These scenarios are valuable QA coverage because they represent realistic situations where the budget is tight and the system must choose which loads to admit.

The key conclusion is: the planner is expected to preserve essential loads and reject the lower-priority ones first, and the test suite confirms the planner behaves accordingly under the modeled constraints.

## Final Status
- Verified algorithm behavior: PASS
- Verified build: PASS
- Verified tests: PASS
- Real measured battery savings: not yet measured on hardware
- Modeled savings estimates: included as assumptions for scenario planning only

## Recommendation
For future validation, the next meaningful step would be a hardware-in-the-loop test with a real battery pack and load profile to convert the modeled “avoided power” values into measured Wh savings and runtime extension.
