# Best-First Search library

This folder is reserved for the complete, hardware-independent Best-First
Search algorithm used by the Kilowatts central node.

## Files

- `BestFirstSearchTypes.h`: Data exchanged inside the algorithm, including
  candidate loads, search inputs, selected-load results and rejection reasons.
- `CandidateScorer.h` / `CandidateScorer.cpp`: Calculates the priority score
  used to rank candidate loads.
- `ConstraintChecker.h` / `ConstraintChecker.cpp`: Applies hard feasibility
  rules before a candidate can enter the final plan.
- `MinPriorityQueue.h`: Bounded binary min-heap that always exposes the
  highest-priority candidate without dynamic allocation.
- `BestFirstSearch.h` / `BestFirstSearch.cpp`: Coordinates scoring,
  constraint checking and queue processing to produce the selected-load plan.

## Boundary

Sensor acquisition, power-budget calculation, relay actuation, ESP-NOW,
Wi-Fi, MQTT and persistent storage do not belong in this library. They supply
inputs to the search or consume its result through separate modules.

## Dissertation mapping

- Best-First Search theory and equations: Section 4.6.3, PDF pages 31-35.
- Candidate evaluation: Algorithm 4.3, PDF page 37.
- Constraint checking: Algorithm 4.4, PDF pages 37-38.
- Min-heap scheduling: Algorithm 4.5, PDF page 38.
- Relay actuation is external: Algorithm 4.6, PDF page 39.

The source files currently contain structure markers only. Implementation will
be added one component at a time without mixing hardware code into the search.
