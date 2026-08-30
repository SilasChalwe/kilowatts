# kilowatts

See [USER_MANUAL.md](USER_MANUAL.md) for how to flash, provision, and operate the system today via the console dashboard and MQTT.

## Documentation map

- [USER_MANUAL.md](USER_MANUAL.md) — console and MQTT operation.
- [INSTALLATION_GUIDE.md](INSTALLATION_GUIDE.md) — site survey, sizing,
  commissioning, and final checks.
- [TECHNICAL_REFERENCE.md](TECHNICAL_REFERENCE.md) — power-budget formulas and
  Best-First ranking rules.
- [QA_REPORT_LOAD_SELECTION_SCENARIOS.md](QA_REPORT_LOAD_SELECTION_SCENARIOS.md)
  — measured findings from the eight-load hardware session.
- [TEST_REPRODUCTION_GUIDE.md](TEST_REPRODUCTION_GUIDE.md) — exact procedure for
  reproducing the four QA scenarios.
- [test-evidence/INSTALLATION_TEST_REPORT.md](test-evidence/INSTALLATION_TEST_REPORT.md)
  — current installation-focused validation record.
- [LIMITATIONS.md](LIMITATIONS.md) and [FUTURE_WORK.md](FUTURE_WORK.md) — explicit
  scope boundaries and recommended next phases.

The completed deliverable is a dissertation software prototype validated within
the available single-board hardware scope. It is not represented as a fully
field-validated multi-node installation; the missing physical validation is
listed explicitly in `LIMITATIONS.md`.

## Operating model

Kilowatts has one Central application and one operating mode. The firmware and
MQTT contract do not expose alternate engineering or test states as device
states. Build and test tooling are internal engineering concerns.

The battery-input simulator is an explicit controlled input source for
diagnostics; it does not switch the Central application into another mode.
