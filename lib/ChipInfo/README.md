# ChipInfo

Reads real ESP32 chip and hardware information directly from ESP-IDF —
target, silicon revision, Wi-Fi/Bluetooth features, flash size, free heap,
PSRAM, ESP-IDF version, and the Wi-Fi station MAC address.

## Responsibility

`ChipInfo` is a small, self-contained wrapper around ESP-IDF's system
information APIs (`esp_chip_info`, `esp_flash_get_physical_size`,
`esp_get_free_heap_size`, `esp_psram_is_initialized`,
`esp_get_idf_version`, `esp_read_mac`). It has exactly two jobs:

```cpp
ChipInfo chipInfo;
chipInfo.printAll();               // logs a full system-information block

ChipInfo::MacAddress macAddress{};
if (chipInfo.getMacAddress(macAddress)) {
    // macAddress[0..5] is this ESP32's real Wi-Fi station MAC address
}
```

`getMacAddress()` is the one method other modules actually depend on:
`EspNowCommunication` reads the local Node's real MAC address through it
rather than duplicating `esp_read_mac()` itself, so there is exactly one
place in the codebase that knows how to obtain the hardware MAC address.

`getChipModelText()` reuses the same `esp_chip_info()` read `printChip()`
already performs, formatted as a short stable identity string (target + CPU
core count, e.g. `"esp32:2core"`) rather than logged text — the commissioning
identity report (`CommissioningPackets::IdentityReportPacket`) uses it so
Central can record what kind of chip a Node is without a second, duplicate
`esp_chip_info()` call anywhere else in the codebase.

## Boundaries

`ChipInfo` does not perform ESP-NOW communication, does not know about
Nodes or Loads, and does not cache or interpret the MAC address beyond
returning the six raw bytes — it is a thin, honest read of real hardware
state, not a domain object.
