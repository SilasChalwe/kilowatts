/**
 * @file ChipInfo.h
 * @brief Declares ESP32 chip and hardware information access.
 */

#ifndef CHIP_INFO_H
#define CHIP_INFO_H


#include <array>
#include <cstddef>
#include <cstdint>


class ChipInfo
{
public:

    using MacAddress =
        std::array<std::uint8_t, 6>;


    /**
     * Prints all available ESP32 system information.
     */
    void printAll() const;


    /**
     * Reads the Wi-Fi station MAC address of this ESP32.
     *
     * macAddress is filled with the six bytes of the MAC address.
     *
     * Returns true when the MAC address was read successfully.
     * Returns false when the MAC address could not be read.
     */
    bool getMacAddress(
        MacAddress& macAddress
    ) const;


    /**
     * Writes a short, stable chip-identity text (target + CPU core count,
     * e.g. "esp32:2core") into buffer, truncated to fit bufferSize with a
     * terminating null always present. Used by the commissioning identity
     * report (see CommissioningPackets::IdentityReportPacket) so Central
     * can record what kind of chip a Node is without duplicating the
     * esp_chip_info() read this class already performs for printChip().
     *
     * Returns false (buffer left as an empty string) when buffer is
     * nullptr or bufferSize is 0.
     */
    bool getChipModelText(
        char* buffer,
        std::size_t bufferSize
    ) const;


    /** Silicon revision as major*100+minor, matching esp_chip_info_t::revision. */
    unsigned int getSiliconRevision() const;

    /** Number of CPU cores reported by esp_chip_info(). */
    unsigned int getCpuCores() const;

    /** Current free heap, in bytes. */
    std::uint32_t getFreeHeapBytes() const;

    /** Minimum free heap ever observed since boot, in bytes. */
    std::uint32_t getMinFreeHeapBytes() const;

    /** Physical flash size in bytes, or 0 when it could not be determined. */
    std::uint32_t getFlashSizeBytes() const;

    /** PSRAM size in bytes, or 0 when PSRAM is not initialized/present. */
    std::uint32_t getPsramSizeBytes() const;

    /** Current CPU clock frequency in MHz. */
    std::uint32_t getCpuFrequencyMhz() const;

    /**
     * Reads this chip's internal die temperature into outCelsius.
     *
     * Returns false (outCelsius left unchanged) when the flashed target has
     * no temperature sensor peripheral (the original ESP32 used by
     * Central has none - only ESP32-S2/S3/C-series/H2 do) or the sensor
     * could not be read. Never fabricates a reading for unsupported
     * hardware.
     */
    bool getTemperatureCelsius(float& outCelsius) const;

    /**
     * Writes a short, stable reset-reason text (e.g. "POWERON", "PANIC",
     * "BROWNOUT") into buffer, truncated to fit bufferSize with a
     * terminating null always present.
     *
     * Returns false (buffer left as an empty string) when buffer is
     * nullptr or bufferSize is 0.
     */
    bool getResetReasonText(
        char* buffer,
        std::size_t bufferSize
    ) const;


private:

    void printChip() const;

    void printFeatures() const;

    void printFlash() const;

    void printMemory() const;

    void printSoftware() const;

    void printMacAddress() const;
};


#endif