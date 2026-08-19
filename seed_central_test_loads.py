#!/usr/bin/env python3
"""Seeds a handful of example Loads directly onto Central's own local
relay pins, for exercising the CONFIGURE_LOAD-targeting-Central feature
(see handleConfigCommand() in src/central/main.cpp) without an installer
manually filling in the web dashboard form six times.

This is a development/test convenience only. It never touches firmware or
any compiled-in default - it publishes exactly the same commands/config
MQTT messages the installer portal itself would publish, so every seeded
Load goes through Central's normal validation, NVS persistence and
relay-pin board-safety check. Anything seeded this way is a completely
ordinary installer-configured Load afterwards: decommission/remove it the
same way you would remove any other Load a real installer added.

Author: Chalwe Silas
Programme: Final-Year Computer Engineering
Institution: The Copperbelt University

Usage:
    KILOWATTS_MQTT_PASSWORD=... python3 seed_central_test_loads.py

Required environment variable:
    KILOWATTS_MQTT_PASSWORD   Broker password. Never hardcoded here or
                               anywhere in source control.

Optional environment variables (defaults match this project's own
KilowattsSecrets.h non-secret values):
    KILOWATTS_MQTT_HOST        (default: the project's HiveMQ Cloud host)
    KILOWATTS_MQTT_PORT        (default: 8883)
    KILOWATTS_MQTT_USERNAME    (default: kilowatts)
    KILOWATTS_TOPIC_NAMESPACE  (default: kilowatts/v1)
    KILOWATTS_CENTRAL_MAC      (default: Central's known MAC, A4:CF:12:0E:32:C0)
    KILOWATTS_ACK_TIMEOUT_S    (default: 10 - seconds to wait for each ack)
"""

import json
import os
import ssl
import sys
import time
import uuid

import paho.mqtt.client as mqtt

DEFAULT_HOST = "f3937cb6e5ab4814a9e88fe931c628af.s1.eu.hivemq.cloud"
DEFAULT_PORT = 8883
DEFAULT_USERNAME = "kilowatts"
DEFAULT_NAMESPACE = "kilowatts/v1"
DEFAULT_CENTRAL_MAC = "A4:CF:12:0E:32:C0"
DEFAULT_ACK_TIMEOUT_S = 10

# Central's own board-declared safe relay GPIOs (see
# CentralNodeConfig::VERIFIED_RELAY_GPIO_PINS) - only the first 6 are used
# here, matching the six Loads below one-to-one.
CENTRAL_SAFE_RELAY_PINS = [4, 13, 14, 16, 17, 18]

# The six example Loads. Deliberately modest wattages and a mix of
# Fixed/Auto so the installation is immediately usable for exercising
# Best-First Search too, not just the CONFIGURE_LOAD path itself.
TEST_LOADS = [
    {
        "name": "Status LED",
        "relayPin": CENTRAL_SAFE_RELAY_PINS[0],
        "relayActiveHigh": True,
        "mode": "FIXED_ON",
        "priority": 1,
        "nominalVoltageVolts": 12.0,
        "nominalCurrentAmps": 0.17,
        "branchMaximumCurrentAmps": 5.0,
        "startupWatts": 2.0,
        "schedule": {"enabled": False, "hour": 0, "minute": 0},
    },
    {
        "name": "Cooling Fan",
        "relayPin": CENTRAL_SAFE_RELAY_PINS[1],
        "relayActiveHigh": True,
        "mode": "AUTO_ON",
        "priority": 5,
        "nominalVoltageVolts": 12.0,
        "nominalCurrentAmps": 1.25,
        "branchMaximumCurrentAmps": 5.0,
        "startupWatts": 25.0,
        "schedule": {"enabled": False, "hour": 0, "minute": 0},
    },
    {
        "name": "Aux Charger",
        "relayPin": CENTRAL_SAFE_RELAY_PINS[2],
        "relayActiveHigh": True,
        "mode": "AUTO_ON",
        "priority": 6,
        "nominalVoltageVolts": 12.0,
        "nominalCurrentAmps": 1.67,
        "branchMaximumCurrentAmps": 5.0,
        "startupWatts": 20.0,
        "schedule": {"enabled": False, "hour": 0, "minute": 0},
    },
    {
        "name": "Test Pump",
        "relayPin": CENTRAL_SAFE_RELAY_PINS[3],
        "relayActiveHigh": True,
        "mode": "AUTO_OFF",
        "priority": 7,
        "nominalVoltageVolts": 12.0,
        "nominalCurrentAmps": 2.5,
        "branchMaximumCurrentAmps": 5.0,
        "startupWatts": 45.0,
        "schedule": {"enabled": False, "hour": 0, "minute": 0},
    },
    {
        "name": "Backup Light",
        "relayPin": CENTRAL_SAFE_RELAY_PINS[4],
        "relayActiveHigh": True,
        "mode": "FIXED_OFF",
        "priority": 3,
        "nominalVoltageVolts": 12.0,
        "nominalCurrentAmps": 0.42,
        "branchMaximumCurrentAmps": 5.0,
        "startupWatts": 5.0,
        "schedule": {"enabled": False, "hour": 0, "minute": 0},
    },
    {
        "name": "Monitor Display",
        "relayPin": CENTRAL_SAFE_RELAY_PINS[5],
        "relayActiveHigh": True,
        "mode": "AUTO_ON",
        "priority": 4,
        "nominalVoltageVolts": 12.0,
        "nominalCurrentAmps": 0.83,
        "branchMaximumCurrentAmps": 5.0,
        "startupWatts": 10.0,
        "schedule": {"enabled": False, "hour": 0, "minute": 0},
    },
]


def env(name, default=None, required=False):
    value = os.environ.get(name, default)
    if required and not value:
        sys.exit(f"ERROR: required environment variable {name} is not set.")
    return value


def main():
    host = env("KILOWATTS_MQTT_HOST", DEFAULT_HOST)
    port = int(env("KILOWATTS_MQTT_PORT", str(DEFAULT_PORT)))
    username = env("KILOWATTS_MQTT_USERNAME", DEFAULT_USERNAME)
    password = env("KILOWATTS_MQTT_PASSWORD", required=True)
    namespace = env("KILOWATTS_TOPIC_NAMESPACE", DEFAULT_NAMESPACE)
    central_mac = env("KILOWATTS_CENTRAL_MAC", DEFAULT_CENTRAL_MAC)
    ack_timeout_s = float(env("KILOWATTS_ACK_TIMEOUT_S", str(DEFAULT_ACK_TIMEOUT_S)))

    commands_config_topic = f"{namespace}/commands/config"
    acks_topic = f"{namespace}/acks"

    pending = {}   # commandId -> load name, waiting for an ack
    results = {}   # commandId -> list of (status, reason) acks received

    def on_connect(client, userdata, flags, reason_code, properties=None):
        if reason_code != 0:
            sys.exit(f"ERROR: MQTT connect failed: {reason_code}")
        print(f"Connected to {host}:{port} as {username!r}.")
        client.subscribe(acks_topic, qos=1)

    def on_message(client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        command_id = payload.get("commandId")
        if command_id in pending:
            results.setdefault(command_id, []).append(
                (payload.get("status"), payload.get("reason"))
            )

    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(username, password)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    client.on_connect = on_connect
    client.on_message = on_message

    print(f"Seeding {len(TEST_LOADS)} test Load(s) onto Central ({central_mac})…")
    client.connect(host, port, keepalive=30)
    client.loop_start()

    # A brief pause lets the SUBSCRIBE to acks_topic complete before any
    # CONFIGURE_LOAD command is published, so no ack is ever missed.
    time.sleep(1.5)

    for load in TEST_LOADS:
        command_id = uuid.uuid4().int & 0xFFFFFFFF
        pending[command_id] = load["name"]
        message = {
            "commandId": command_id,
            "action": "CONFIGURE_LOAD",
            "nodeMac": central_mac,
            "load": load,
        }
        client.publish(commands_config_topic, json.dumps(message), qos=1)
        print(f"  -> published CONFIGURE_LOAD for {load['name']!r} "
              f"(relayPin={load['relayPin']}, commandId={command_id})")

    print(f"\nWaiting up to {ack_timeout_s:.0f}s for acknowledgements…")
    deadline = time.monotonic() + ack_timeout_s
    while time.monotonic() < deadline and len(results) < len(pending):
        time.sleep(0.2)

    client.loop_stop()
    client.disconnect()

    print("\n" + "=" * 70)
    print("RESULTS")
    print("=" * 70)
    failed = False
    for command_id, name in pending.items():
        acks = results.get(command_id)
        if not acks:
            print(f"  {name:<20s} NO RESPONSE (Central offline, or unreachable?)")
            failed = True
            continue
        for status, reason in acks:
            print(f"  {name:<20s} {status:<10s} {reason or ''}")
            if status not in ("ACCEPTED", "APPLIED"):
                failed = True

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
