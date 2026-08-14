/**
 * @file ChipInfo.cpp
 * @brief Implements ESP32 chip and hardware information access.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */


#include "ChipInfo.h"


#include <cinttypes>
#include <cstdio>


#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "sdkconfig.h"


static const char *TAG = "CHIP_INFO";



void ChipInfo::printAll() const
{
    ESP_LOGI(
        TAG,
        "========== ESP32 SYSTEM INFORMATION =========="
    );


    printChip();
    printFeatures();
    printFlash();
    printMemory();
    printSoftware();
    printMacAddress();


    ESP_LOGI(
        TAG,
        "=============================================="
    );
}



bool ChipInfo::getMacAddress(
    MacAddress& macAddress
) const
{
    /*
     * One complete MAC address contains 6 bytes.
     *
     * Example:
     * 24:6F:28:AA:BB:01
     *
     * macAddress[0] = 0x24
     * macAddress[1] = 0x6F
     * macAddress[2] = 0x28
     * macAddress[3] = 0xAA
     * macAddress[4] = 0xBB
     * macAddress[5] = 0x01
     *
     * These are NOT six different MAC addresses.
     * Together they form ONE Wi-Fi station MAC address.
     */
    const esp_err_t result =
        esp_read_mac(
            macAddress.data(),
            ESP_MAC_WIFI_STA
        );


    if (result != ESP_OK)
    {
        macAddress.fill(0U);


        ESP_LOGE(
            TAG,
            "Could not read Wi-Fi station MAC address"
        );


        return false;
    }


    return true;
}



bool ChipInfo::getChipModelText(
    char* buffer,
    std::size_t bufferSize
) const
{
    if (buffer == nullptr || bufferSize == 0U)
    {
        return false;
    }


    esp_chip_info_t info;
    esp_chip_info(&info);


    std::snprintf(
        buffer,
        bufferSize,
        "%s:%ucore",
        CONFIG_IDF_TARGET,
        static_cast<unsigned int>(info.cores)
    );


    return true;
}



void ChipInfo::printChip() const
{
    esp_chip_info_t info;
    esp_chip_info(&info);


    unsigned int majorRevision =
        info.revision / 100;

    unsigned int minorRevision =
        info.revision % 100;


    ESP_LOGI(
        TAG,
        "Target: %s",
        CONFIG_IDF_TARGET
    );

    ESP_LOGI(
        TAG,
        "CPU cores: %d",
        info.cores
    );

    ESP_LOGI(
        TAG,
        "Silicon revision: v%u.%u",
        majorRevision,
        minorRevision
    );
}



void ChipInfo::printFeatures() const
{
    esp_chip_info_t info;
    esp_chip_info(&info);


    ESP_LOGI(
        TAG,
        "Wi-Fi 2.4 GHz: %s",
        (info.features & CHIP_FEATURE_WIFI_BGN)
            ? "Yes"
            : "No"
    );

    ESP_LOGI(
        TAG,
        "Bluetooth Classic: %s",
        (info.features & CHIP_FEATURE_BT)
            ? "Yes"
            : "No"
    );

    ESP_LOGI(
        TAG,
        "Bluetooth LE: %s",
        (info.features & CHIP_FEATURE_BLE)
            ? "Yes"
            : "No"
    );

    ESP_LOGI(
        TAG,
        "Flash type: %s",
        (info.features & CHIP_FEATURE_EMB_FLASH)
            ? "Embedded"
            : "External"
    );
}



void ChipInfo::printFlash() const
{
    uint32_t flashSize = 0U;


    const esp_err_t result =
        esp_flash_get_physical_size(
            nullptr,
            &flashSize
        );


    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Physical flash: %" PRIu32 " MB",
            flashSize / (1024U * 1024U)
        );
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Could not determine flash size"
        );
    }
}



void ChipInfo::printMemory() const
{
    ESP_LOGI(
        TAG,
        "Free heap: %" PRIu32 " bytes",
        esp_get_free_heap_size()
    );


    ESP_LOGI(
        TAG,
        "Minimum free heap: %" PRIu32 " bytes",
        esp_get_minimum_free_heap_size()
    );


    if (esp_psram_is_initialized())
    {
        const size_t psramSize =
            esp_psram_get_size();


        ESP_LOGI(
            TAG,
            "PSRAM: %u MB",
            static_cast<unsigned int>(
                psramSize / (1024U * 1024U)
            )
        );
    }
    else
    {
        ESP_LOGW(
            TAG,
            "PSRAM: not initialized"
        );
    }
}



void ChipInfo::printSoftware() const
{
    ESP_LOGI(
        TAG,
        "ESP-IDF version: %s",
        esp_get_idf_version()
    );
}



void ChipInfo::printMacAddress() const
{
    MacAddress macAddress{};


    if (!getMacAddress(macAddress))
    {
        return;
    }


    ESP_LOGI(
        TAG,
        "Wi-Fi station MAC: %02X:%02X:%02X:%02X:%02X:%02X",
        macAddress[0],
        macAddress[1],
        macAddress[2],
        macAddress[3],
        macAddress[4],
        macAddress[5]
    );
}