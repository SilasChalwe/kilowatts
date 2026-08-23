#!/usr/bin/env bash
#
# Builds and runs the host-native Kilowatts behavioral test suite
# (test/main.cpp) with plain g++ - no ESP32 and no PlatformIO involved.
#
# ESP_PLATFORM is left undefined, so PowerManager/Load/BestFirstSearch/
# CurrentTimeProvider each take the host-safe/pure-calculation #else branch
# of their existing #ifdef ESP_PLATFORM split - NOT the same branch the
# ESP32 firmware executes (that branch is the ESP-IDF/hardware one, guarded
# by #ifdef ESP_PLATFORM, and is deliberately not compiled here).
#
# `pio run -e central` / `pio run -e smart` are the separate, required
# check that the ESP-IDF firmware path itself still compiles. This script
# proves the runtime/budget math is correct; it makes no claim about, and
# does not validate, physical hardware (INA219, GPIO, relays, ESP-NOW).

set -Eeuo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="$PROJECT_ROOT/test/.build"
BINARY="$BUILD_DIR/kilowatts_tests"

mkdir -p "$BUILD_DIR"

g++ -std=c++17 -O0 -g -Wall -Wextra \
    -I"$PROJECT_ROOT/lib/BatteryManager" \
    -I"$PROJECT_ROOT/lib/LoadManager" \
    -I"$PROJECT_ROOT/lib/LoadManager/Central" \
    -I"$PROJECT_ROOT/lib/BestFirstSearch" \
    -I"$PROJECT_ROOT/lib/FirmwareManager" \
    "$PROJECT_ROOT/test/main.cpp" \
    "$PROJECT_ROOT/lib/BatteryManager/PowerManager.cpp" \
    "$PROJECT_ROOT/lib/LoadManager/Load.cpp" \
    "$PROJECT_ROOT/lib/LoadManager/Central/LoadScheduleEvaluator.cpp" \
    "$PROJECT_ROOT/lib/BestFirstSearch/BestFirstSearch.cpp" \
    "$PROJECT_ROOT/lib/FirmwareManager/CurrentTimeProvider.cpp" \
    -o "$BINARY"

"$BINARY"
