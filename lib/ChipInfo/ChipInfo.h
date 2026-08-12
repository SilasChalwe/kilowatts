/**
 * @file ChipInfo.h
 * @brief Declares ESP32 chip and hardware information access.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */

#ifndef CHIP_INFO_H
#define CHIP_INFO_H


#include <array>
#include <cstdint>


class ChipInfo
{
public:

    /**
     * One complete ESP32 MAC address contains 6 bytes.
     *
     * Example:
     * 24:6F:28:AA:BB:01
     *
     * macAddress[0] through macAddress[5] are the six bytes
     * of ONE complete MAC address.
     */
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


private:

    void printChip() const;

    void printFeatures() const;

    void printFlash() const;

    void printMemory() const;

    void printSoftware() const;

    void printMacAddress() const;
};


#endif