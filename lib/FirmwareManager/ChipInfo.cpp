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



unsigned int ChipInfo::getSiliconRevision() const
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    return static_cast<unsigned int>(info.revision);
}



unsigned int ChipInfo::getCpuCores() const
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    return static_cast<unsigned int>(info.cores);
}



std::uint32_t ChipInfo::getFreeHeapBytes() const
{
    return static_cast<std::uint32_t>(esp_get_free_heap_size());
}



std::uint32_t ChipInfo::getMinFreeHeapBytes() const
{
    return static_cast<std::uint32_t>(esp_get_minimum_free_heap_size());
}



std::uint32_t ChipInfo::getFlashSizeBytes() const
{
    std::uint32_t flashSize = 0U;

    const esp_err_t result =
        esp_flash_get_physical_size(
            nullptr,
            &flashSize
        );

    return result == ESP_OK ? flashSize : 0U;
}



std::uint32_t ChipInfo::getPsramSizeBytes() const
{
#ifdef CONFIG_SPIRAM
    if (!esp_psram_is_initialized())
    {
        return 0U;
    }

    return static_cast<std::uint32_t>(esp_psram_get_size());
#else
    return 0U;
#endif
}



bool ChipInfo::getResetReasonText(
    char* buffer,
    std::size_t bufferSize
) const
{
    if (buffer == nullptr || bufferSize == 0U)
    {
        return false;
    }

    const char* text = "UNKNOWN";

    switch (esp_reset_reason())
    {
        case ESP_RST_POWERON:  text = "POWERON";  break;
        case ESP_RST_EXT:      text = "EXT";      break;
        case ESP_RST_SW:       text = "SW";       break;
        case ESP_RST_PANIC:    text = "PANIC";    break;
        case ESP_RST_INT_WDT:  text = "INT_WDT";  break;
        case ESP_RST_TASK_WDT: text = "TASK_WDT"; break;
        case ESP_RST_WDT:      text = "WDT";      break;
        case ESP_RST_DEEPSLEEP: text = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT: text = "BROWNOUT"; break;
        case ESP_RST_SDIO:     text = "SDIO";     break;
        default:                text = "UNKNOWN"; break;
    }

    std::snprintf(buffer, bufferSize, "%s", text);
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


#ifdef CONFIG_SPIRAM
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
#else
    ESP_LOGW(
        TAG,
        "PSRAM: not supported by this build"
    );
#endif
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