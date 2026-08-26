#!/usr/bin/env bash
#
# Builds and runs the host-native Kilowatts behavioral test suites with
# plain g++ - no ESP32 and no PlatformIO involved.
#
# ESP_PLATFORM is left undefined, so PowerManager/Load/BestFirstSearch/
# CurrentTimeProvider each take the host-safe/pure-calculation #else branch
# of their existing #ifdef ESP_PLATFORM split - NOT the same branch the
# ESP32 firmware executes.
#
# `pio run -e central` / `pio run -e smart` are the separate, required
# check that the ESP-IDF firmware path itself still compiles.

set -Eeuo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="$PROJECT_ROOT/test/.build"
MAIN_BINARY="$BUILD_DIR/kilowatts_tests"
SCHEDULE_BINARY="$BUILD_DIR/schedule_window_tests"

mkdir -p "$BUILD_DIR"

COMMON_INCLUDES=(
    -I"$PROJECT_ROOT/lib/BatteryManager"
    -I"$PROJECT_ROOT/lib/LoadManager"
    -I"$PROJECT_ROOT/lib/LoadManager/Central"
    -I"$PROJECT_ROOT/lib/BestFirstSearch"
    -I"$PROJECT_ROOT/lib/FirmwareManager"
)

COMMON_SOURCES=(
    "$PROJECT_ROOT/lib/LoadManager/Load.cpp"
    "$PROJECT_ROOT/lib/LoadManager/Central/LoadScheduleEvaluator.cpp"
    "$PROJECT_ROOT/lib/BestFirstSearch/BestFirstSearch.cpp"
    "$PROJECT_ROOT/lib/FirmwareManager/CurrentTimeProvider.cpp"
)

g++ -std=c++17 -O0 -g -Wall -Wextra \
    "${COMMON_INCLUDES[@]}" \
    "$PROJECT_ROOT/test/main.cpp" \
    "$PROJECT_ROOT/lib/BatteryManager/PowerManager.cpp" \
    "${COMMON_SOURCES[@]}" \
    -o "$MAIN_BINARY"

"$MAIN_BINARY"

g++ -std=c++17 -O0 -g -Wall -Wextra \
    "${COMMON_INCLUDES[@]}" \
    "$PROJECT_ROOT/test/schedule_window_tests.cpp" \
    "${COMMON_SOURCES[@]}" \
    -o "$SCHEDULE_BINARY"

"$SCHEDULE_BINARY"
