/**
 * @file BatteryStateOfCharge.cpp
 * @brief Implements the coulomb-counting battery State of Charge estimator
 *        (Section 4.6.2.2, Equation 4.8).
 *
 * The coulomb-counting update itself (update()) is plain, hardware-free
 * math and is always compiled, so it is directly host-testable (see
 * test/BatteryStateOfCharge/). NVS persistence is compiled only under
 * ESP_PLATFORM, matching the split already used by CurrentTimeProvider and
 * INA219Monitor's calibration storage — a host build's persist() always
 * reports failure rather than fabricating durable storage.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "BatteryStateOfCharge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace kilowatts {


#ifdef ESP_PLATFORM
static const char *TAG = "BATTERY_SOC";

namespace {
/*
 * NVS is persistent non-volatile configuration storage, not a database.
 * This namespace and key are private to BatteryStateOfCharge's own
 * responsibility only.
 */
constexpr const char* NVS_NAMESPACE = "kw_battery";
constexpr const char* NVS_KEY_STATE_OF_CHARGE = "soc";
} // namespace
#endif


const char* toText(StateOfChargeSource source)
{
    switch (source) {
        case StateOfChargeSource::UNKNOWN: return "UNKNOWN";
        case StateOfChargeSource::PERSISTED: return "PERSISTED";
        case StateOfChargeSource::INITIAL_COMMISSIONING: return "INITIAL_COMMISSIONING";
        case StateOfChargeSource::COULOMB_COUNTING: return "COULOMB_COUNTING";
    }

    return "UNKNOWN";
}


BatteryStateOfCharge::BatteryStateOfCharge()
    : initialized_(false),
      batteryCapacityAmpHours_(0.0F),
      stateOfChargePercent_(0.0F),
      source_(StateOfChargeSource::UNKNOWN)
{
}


float BatteryStateOfCharge::clampPercent(float value)
{
    return std::max(0.0F, std::min(value, 100.0F));
}


bool BatteryStateOfCharge::initialize(float batteryCapacityAmpHours, float defaultStateOfChargePercent)
{
    if (!std::isfinite(batteryCapacityAmpHours) || batteryCapacityAmpHours <= 0.0F ||
        !std::isfinite(defaultStateOfChargePercent) ||
        defaultStateOfChargePercent < 0.0F || defaultStateOfChargePercent > 100.0F) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "initialize() rejected: capacity=%.3fAh defaultSoC=%.1f%%",
                 static_cast<double>(batteryCapacityAmpHours), static_cast<double>(defaultStateOfChargePercent));
#endif
        return false;
    }

    batteryCapacityAmpHours_ = batteryCapacityAmpHours;

    float persistedStateOfCharge = 0.0F;
    if (loadPersistedStateOfChargePercent(persistedStateOfCharge)) {
        stateOfChargePercent_ = persistedStateOfCharge;
        source_ = StateOfChargeSource::PERSISTED;
#ifdef ESP_PLATFORM
        ESP_LOGI(TAG, "BATTERY_SOC: valid persisted record found; SoC=%.2f%% source=PERSISTED capacity=%.3fAh",
                 static_cast<double>(stateOfChargePercent_), static_cast<double>(batteryCapacityAmpHours_));
#endif
    } else {
        stateOfChargePercent_ = defaultStateOfChargePercent;
        source_ = StateOfChargeSource::INITIAL_COMMISSIONING;
#ifdef ESP_PLATFORM
        ESP_LOGI(TAG, "BATTERY_SOC: No valid persisted SoC estimate; SoC=%.2f%% source=INITIAL_COMMISSIONING capacity=%.3fAh",
                 static_cast<double>(stateOfChargePercent_), static_cast<double>(batteryCapacityAmpHours_));
#endif
    }

    initialized_ = true;
    return true;
}


bool BatteryStateOfCharge::isInitialized() const
{
    return initialized_;
}


bool BatteryStateOfCharge::isValid() const
{
    return source_ != StateOfChargeSource::UNKNOWN;
}


StateOfChargeSource BatteryStateOfCharge::getSource() const
{
    return source_;
}


bool BatteryStateOfCharge::update(float batteryCurrentAmps, float deltaTimeSeconds)
{
    if (!initialized_ ||
        !std::isfinite(batteryCurrentAmps) ||
        !std::isfinite(deltaTimeSeconds) || deltaTimeSeconds <= 0.0F) {
        return false;
    }

    /*
     * SoC(t) = clamp[ SoC(t-Dt) - 100 I_B(t) Dt / (3600 C_B), 0, 100 ]
     * (Equation 4.8). Discharge current is positive, so a positive I_B
     * reduces SoC and a negative (charging) I_B increases it.
     */
    const float dischargedPercent =
        (100.0F * batteryCurrentAmps * deltaTimeSeconds) / (3600.0F * batteryCapacityAmpHours_);

    stateOfChargePercent_ = clampPercent(stateOfChargePercent_ - dischargedPercent);
    source_ = StateOfChargeSource::COULOMB_COUNTING;

    return true;
}


float BatteryStateOfCharge::getStateOfChargePercent() const
{
    return stateOfChargePercent_;
}


bool BatteryStateOfCharge::persist() const
{
    if (!initialized_) {
        return false;
    }

    return persistStateOfChargePercent(stateOfChargePercent_);
}


bool BatteryStateOfCharge::loadPersistedStateOfChargePercent(float& stateOfChargePercent) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        return false;
    }

    std::uint32_t rawBits = 0U;
    result = nvs_get_u32(handle, NVS_KEY_STATE_OF_CHARGE, &rawBits);
    nvs_close(handle);

    if (result != ESP_OK) {
        return false;
    }

    float restored = 0.0F;
    static_assert(sizeof(rawBits) == sizeof(restored), "float must be 32 bits to round-trip through NVS u32 storage");
    std::memcpy(&restored, &rawBits, sizeof(restored));

    if (!std::isfinite(restored) || restored < 0.0F || restored > 100.0F) {
        ESP_LOGW(TAG, "Discarding corrupt persisted SoC value");
        return false;
    }

    stateOfChargePercent = restored;
    return true;
#else
    (void)stateOfChargePercent;
    return false;
#endif
}


bool BatteryStateOfCharge::persistStateOfChargePercent(float stateOfChargePercent) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS to persist SoC: %s", esp_err_to_name(result));
        return false;
    }

    std::uint32_t rawBits = 0U;
    static_assert(sizeof(rawBits) == sizeof(stateOfChargePercent), "float must be 32 bits to round-trip through NVS u32 storage");
    std::memcpy(&rawBits, &stateOfChargePercent, sizeof(stateOfChargePercent));

    result = nvs_set_u32(handle, NVS_KEY_STATE_OF_CHARGE, rawBits);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);
    return result == ESP_OK;
#else
    (void)stateOfChargePercent;
    return false;
#endif
}


} // namespace kilowatts
