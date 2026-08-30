# kilowatts

See [USER_MANUAL.md](USER_MANUAL.md) for how to flash, provision, and operate the system today via the console dashboard and MQTT.

## Operating model

Kilowatts has one Central application and one operating mode. The firmware and
MQTT contract do not expose alternate engineering or test states as device
states. Build and test tooling are internal engineering concerns.

The battery-input simulator is an explicit controlled input source for
diagnostics; it does not switch the Central application into another mode.
