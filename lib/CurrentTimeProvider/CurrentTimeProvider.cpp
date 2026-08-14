/**
 * @file CurrentTimeProvider.cpp
 * @brief Implements dual-mode (Automatic NTP / Manual) real wall-clock
 *        time for the Kilowatts firmware.
 *
 * Uses ESP-IDF's esp_netif SNTP integration (driver/esp_netif_sntp.h) for
 * Automatic mode, and the standard POSIX/ESP-IDF settimeofday()/mktime()
 * mechanism for Manual mode — both ultimately set the same ESP32 system
 * clock. Time Mode is persisted using the real ESP-IDF NVS API
 * (nvs_flash_init()/nvs_open()/nvs_get_u8()/nvs_set_u8()), scoped to one
 * small namespace local to this responsibility.
 *
 * This file is split by ESP_PLATFORM, the macro ESP-IDF defines for every
 * source file it builds, the same way INA219Monitor.cpp is:
 *
 * - Manual date/time validation (isManualDateTimeValid() and the
 *   in-memory Time Mode/Time Source bookkeeping in setTimeMode()) is
 *   plain, hardware-free C++ and is always compiled, so it can be
 *   exercised by a host-native test (see test/CurrentTimeProvider/) with
 *   no ESP32 involved.
 * - Real SNTP, NVS and system-clock changes (settimeofday()) are compiled
 *   only under ESP_PLATFORM. A host build's version of every
 *   hardware-touching operation simply reports failure/no data rather
 *   than fabricating a synchronized time, a persisted value, or an
 *   applied clock change.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "CurrentTimeProvider.h"

#include <cstdlib>
#include <sys/time.h>

#ifdef ESP_PLATFORM
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace kilowatts {


#ifdef ESP_PLATFORM
static const char *TAG = "CURRENT_TIME_PROVIDER";
#endif


namespace {

/*
 * POSIX TZ string for Zambia (Central Africa Time, UTC+2, no daylight
 * saving time), matching this project's stated institution (The
 * Copperbelt University). POSIX TZ sign convention is inverted from the
 * UTC offset: east of Greenwich is negative, hence "CAT-2" for UTC+2.
 * ESP-IDF's newlib does not carry the IANA zoneinfo database, so a POSIX
 * TZ string is used rather than a zone name such as "Africa/Lusaka".
 *
 * Used by setenv("TZ", ...) unconditionally (both platforms), so this
 * stays outside the ESP_PLATFORM guard below.
 */
constexpr const char* LOCAL_TIMEZONE_POSIX_STRING = "CAT-2";

/*
 * Sanity bounds for a manually entered year: this project cannot
 * legitimately need a manual date before the year 2000, and 2099 is
 * generous headroom without accepting nonsensical values. Used by
 * isManualDateTimeValid(), which is always compiled.
 */
constexpr std::uint16_t MINIMUM_MANUAL_YEAR = 2000U;
constexpr std::uint16_t MAXIMUM_MANUAL_YEAR = 2099U;


bool isLeapYear(std::uint16_t year)
{
    return (year % 4U == 0U && year % 100U != 0U) || (year % 400U == 0U);
}


std::uint8_t daysInMonth(std::uint16_t year, std::uint8_t month)
{
    static constexpr std::uint8_t DAYS_IN_MONTH[12] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
    };

    if (month < 1U || month > 12U) {
        return 0U;
    }

    if (month == 2U && isLeapYear(year)) {
        return 29U;
    }

    return DAYS_IN_MONTH[month - 1U];
}


#ifdef ESP_PLATFORM

/*
 * pool.ntp.org is the standard public NTP pool used throughout ESP-IDF's
 * own SNTP documentation and examples.
 */
constexpr const char* NTP_SERVER_HOSTNAME = "pool.ntp.org";

/*
 * NVS is persistent non-volatile configuration storage, not a database.
 * This namespace and its two keys are local to CurrentTimeProvider's own
 * responsibility only.
 */
constexpr const char* NVS_NAMESPACE = "kw_time";
constexpr const char* NVS_KEY_TIME_MODE = "mode";
constexpr const char* NVS_KEY_MANUAL_TIMESTAMP = "manual_ts";

#endif // ESP_PLATFORM

} // namespace


CurrentTimeProvider* CurrentTimeProvider::instance_ = nullptr;


CurrentTimeProvider::CurrentTimeProvider()
    : initialized_(false),
      sntpActive_(false),
      timeMode_(TimeMode::AUTOMATIC),
      timeSource_(TimeSource::NONE)
{
    instance_ = this;
}


CurrentTimeProvider::~CurrentTimeProvider()
{
    if (instance_ == this) {
        stopAutomaticSynchronization();
        instance_ = nullptr;
    }
}


bool CurrentTimeProvider::isManualDateTimeValid(const ManualDateTime& manualDateTime)
{
    if (manualDateTime.year < MINIMUM_MANUAL_YEAR || manualDateTime.year > MAXIMUM_MANUAL_YEAR) {
        return false;
    }

    if (manualDateTime.month < 1U || manualDateTime.month > 12U) {
        return false;
    }

    const std::uint8_t maximumDay = daysInMonth(manualDateTime.year, manualDateTime.month);
    if (manualDateTime.day < 1U || manualDateTime.day > maximumDay) {
        return false;
    }

    if (manualDateTime.hour > 23U) {
        return false;
    }

    if (manualDateTime.minute > 59U) {
        return false;
    }

    if (manualDateTime.second > 59U) {
        return false;
    }

    return true;
}


bool CurrentTimeProvider::initializeNonVolatileStorage() const
{
#ifdef ESP_PLATFORM
    esp_err_t result = nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires erase before initialisation");

        result = nvs_flash_erase();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Could not erase NVS: %s", esp_err_to_name(result));
            return false;
        }

        result = nvs_flash_init();
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialisation failed: %s", esp_err_to_name(result));
        return false;
    }

    return true;
#else
    return false;
#endif
}


TimeMode CurrentTimeProvider::loadPersistedTimeMode() const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (result != ESP_OK) {
        ESP_LOGI(TAG, "No persisted time configuration found yet; defaulting Time Mode to AUTOMATIC");
        return TimeMode::AUTOMATIC;
    }

    std::uint8_t storedMode = 0U;
    result = nvs_get_u8(handle, NVS_KEY_TIME_MODE, &storedMode);
    nvs_close(handle);

    if (result != ESP_OK) {
        return TimeMode::AUTOMATIC;
    }

    if (storedMode == static_cast<std::uint8_t>(TimeMode::MANUAL)) {
        return TimeMode::MANUAL;
    }

    return TimeMode::AUTOMATIC;
#else
    return TimeMode::AUTOMATIC;
#endif
}


bool CurrentTimeProvider::persistTimeMode(TimeMode timeMode) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS to persist Time Mode: %s", esp_err_to_name(result));
        return false;
    }

    result = nvs_set_u8(handle, NVS_KEY_TIME_MODE, static_cast<std::uint8_t>(timeMode));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist Time Mode: %s", esp_err_to_name(result));
        return false;
    }

    return true;
#else
    (void)timeMode;
    return false;
#endif
}


bool CurrentTimeProvider::persistLastManualEpochSeconds(std::int64_t epochSeconds) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS to persist the last manual date/time: %s", esp_err_to_name(result));
        return false;
    }

    result = nvs_set_i64(handle, NVS_KEY_MANUAL_TIMESTAMP, epochSeconds);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist the last manual date/time: %s", esp_err_to_name(result));
        return false;
    }

    return true;
#else
    (void)epochSeconds;
    return false;
#endif
}


bool CurrentTimeProvider::startAutomaticSynchronization()
{
#ifdef ESP_PLATFORM
    if (sntpActive_) {
        return true;
    }

    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(result));
        return false;
    }

    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Default event loop initialisation failed: %s", esp_err_to_name(result));
        return false;
    }

    esp_sntp_config_t sntpConfiguration = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER_HOSTNAME);
    sntpConfiguration.sync_cb = &CurrentTimeProvider::handleTimeSynchronizationEvent;

    result = esp_netif_sntp_init(&sntpConfiguration);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "SNTP initialisation failed: %s", esp_err_to_name(result));
        return false;
    }

    sntpActive_ = true;

    ESP_LOGI(TAG, "SNTP time synchronization started against %s", NTP_SERVER_HOSTNAME);

    return true;
#else
    return false;
#endif
}


void CurrentTimeProvider::stopAutomaticSynchronization()
{
#ifdef ESP_PLATFORM
    if (!sntpActive_) {
        return;
    }

    esp_netif_sntp_deinit();
    sntpActive_ = false;

    ESP_LOGI(TAG, "SNTP time synchronization stopped");
#endif
}


bool CurrentTimeProvider::initializeTimeSynchronization()
{
    if (initialized_) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "Time provider already initialised; ignoring repeated call");
#endif
        return true;
    }

    setenv("TZ", LOCAL_TIMEZONE_POSIX_STRING, 1);
    tzset();

    if (!initializeNonVolatileStorage()) {
        return false;
    }

    timeMode_.store(loadPersistedTimeMode());
    timeSource_.store(TimeSource::NONE);

    bool result = true;

    if (timeMode_.load() == TimeMode::AUTOMATIC) {
        result = startAutomaticSynchronization();
    }
#ifdef ESP_PLATFORM
    else {
        ESP_LOGI(TAG, "Persisted Time Mode is MANUAL; waiting for setManualCurrentDateTime()");
    }
#endif

    initialized_ = true;

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "CurrentTimeProvider initialised: mode=%s timezone=%s",
             timeMode_.load() == TimeMode::AUTOMATIC ? "AUTOMATIC" : "MANUAL",
             LOCAL_TIMEZONE_POSIX_STRING);
#endif

    return result;
}


TimeMode CurrentTimeProvider::getTimeMode() const
{
    return timeMode_.load();
}


bool CurrentTimeProvider::setTimeMode(TimeMode timeMode)
{
    if (timeMode_.load() == timeMode) {
        return true;
    }

    const bool persisted = persistTimeMode(timeMode);
#ifdef ESP_PLATFORM
    if (!persisted) {
        ESP_LOGE(TAG, "Time Mode change was not persisted to NVS");
    }
#endif

    /*
     * A mode change always invalidates the previous Source: a stale
     * Automatic/NTP or Manual value is never carried over into the newly
     * selected mode. The underlying system clock value itself is left
     * alone — only this object's own trust bookkeeping is reset. This
     * bookkeeping happens unconditionally on both platforms; only the
     * real NVS/SNTP side effects below are ESP32-only.
     */
    timeMode_.store(timeMode);
    timeSource_.store(TimeSource::NONE);

    bool transitionSucceeded = true;

    if (timeMode == TimeMode::MANUAL) {
        stopAutomaticSynchronization();
    } else {
        transitionSucceeded = startAutomaticSynchronization();
    }

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Time Mode changed to %s", timeMode == TimeMode::AUTOMATIC ? "AUTOMATIC" : "MANUAL");
#endif

    return persisted && transitionSucceeded;
}


bool CurrentTimeProvider::setManualCurrentDateTime(const ManualDateTime& manualDateTime)
{
    if (!isManualDateTimeValid(manualDateTime)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected manual date/time %04u-%02u-%02u %02u:%02u:%02u: not a real date/time",
                 static_cast<unsigned int>(manualDateTime.year),
                 static_cast<unsigned int>(manualDateTime.month),
                 static_cast<unsigned int>(manualDateTime.day),
                 static_cast<unsigned int>(manualDateTime.hour),
                 static_cast<unsigned int>(manualDateTime.minute),
                 static_cast<unsigned int>(manualDateTime.second));
#endif
        return false;
    }

#ifdef ESP_PLATFORM
    std::tm localDateTime{};
    localDateTime.tm_year = static_cast<int>(manualDateTime.year) - 1900;
    localDateTime.tm_mon = static_cast<int>(manualDateTime.month) - 1;
    localDateTime.tm_mday = static_cast<int>(manualDateTime.day);
    localDateTime.tm_hour = static_cast<int>(manualDateTime.hour);
    localDateTime.tm_min = static_cast<int>(manualDateTime.minute);
    localDateTime.tm_sec = static_cast<int>(manualDateTime.second);
    localDateTime.tm_isdst = 0;

    /*
     * mktime() interprets localDateTime according to the timezone already
     * configured by initializeTimeSynchronization() (TZ=CAT-2), so the
     * user's entry is treated as local time, matching AutoSchedule's own
     * local-time semantics.
     */
    const std::time_t epochSeconds = mktime(&localDateTime);
    if (epochSeconds == static_cast<std::time_t>(-1)) {
        ESP_LOGE(TAG, "mktime() could not convert the supplied manual date/time");
        return false;
    }

    struct timeval newSystemTime{};
    newSystemTime.tv_sec = epochSeconds;
    newSystemTime.tv_usec = 0;

    if (settimeofday(&newSystemTime, nullptr) != 0) {
        ESP_LOGE(TAG, "settimeofday() failed while applying the manual date/time");
        return false;
    }

    timeSource_.store(TimeSource::MANUAL);

    if (!persistLastManualEpochSeconds(static_cast<std::int64_t>(epochSeconds))) {
        ESP_LOGW(TAG, "Manual date/time applied, but could not be persisted as a recovery value");
    }

    ESP_LOGI(TAG, "Manual date/time applied: %04u-%02u-%02u %02u:%02u:%02u",
             static_cast<unsigned int>(manualDateTime.year),
             static_cast<unsigned int>(manualDateTime.month),
             static_cast<unsigned int>(manualDateTime.day),
             static_cast<unsigned int>(manualDateTime.hour),
             static_cast<unsigned int>(manualDateTime.minute),
             static_cast<unsigned int>(manualDateTime.second));

    return true;
#else
    /*
     * A host build has no real system clock of this Node to set. Never
     * fabricate success: the validated input is simply not applied.
     */
    return false;
#endif
}


TimeSource CurrentTimeProvider::getCurrentTimeSource() const
{
    return timeSource_.load();
}


bool CurrentTimeProvider::isCurrentTimeValid() const
{
    return timeSource_.load() != TimeSource::NONE;
}


bool CurrentTimeProvider::getCurrentLocalDateTime(std::tm& localDateTime) const
{
    if (!isCurrentTimeValid()) {
        return false;
    }

    const std::time_t now = std::time(nullptr);
    return localtime_r(&now, &localDateTime) != nullptr;
}


bool CurrentTimeProvider::getCurrentLocalHour(std::uint8_t& hour) const
{
    std::tm localDateTime{};
    if (!getCurrentLocalDateTime(localDateTime)) {
        return false;
    }

    hour = static_cast<std::uint8_t>(localDateTime.tm_hour);
    return true;
}


bool CurrentTimeProvider::getCurrentLocalMinute(std::uint8_t& minute) const
{
    std::tm localDateTime{};
    if (!getCurrentLocalDateTime(localDateTime)) {
        return false;
    }

    minute = static_cast<std::uint8_t>(localDateTime.tm_min);
    return true;
}


bool CurrentTimeProvider::getCurrentLocalSecond(std::uint8_t& second) const
{
    std::tm localDateTime{};
    if (!getCurrentLocalDateTime(localDateTime)) {
        return false;
    }

    second = static_cast<std::uint8_t>(localDateTime.tm_sec);
    return true;
}


bool CurrentTimeProvider::getLastConfiguredManualDateTime(std::tm& lastManualDateTime) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (result != ESP_OK) {
        return false;
    }

    std::int64_t storedEpochSeconds = 0;
    result = nvs_get_i64(handle, NVS_KEY_MANUAL_TIMESTAMP, &storedEpochSeconds);
    nvs_close(handle);

    if (result != ESP_OK) {
        return false;
    }

    const std::time_t storedTime = static_cast<std::time_t>(storedEpochSeconds);
    return localtime_r(&storedTime, &lastManualDateTime) != nullptr;
#else
    (void)lastManualDateTime;
    return false;
#endif
}


void CurrentTimeProvider::printDiagnosticReport() const
{
#ifdef ESP_PLATFORM
    const TimeMode mode = timeMode_.load();
    const TimeSource source = timeSource_.load();

    const char *sourceText = "NONE";
    if (source == TimeSource::NTP) {
        sourceText = "NTP";
    } else if (source == TimeSource::MANUAL) {
        sourceText = "MANUAL";
    }

    ESP_LOGI(TAG, "================ CURRENT TIME PROVIDER DIAGNOSTIC ================");
    ESP_LOGI(TAG, "Time Mode          : %s", mode == TimeMode::AUTOMATIC ? "AUTOMATIC" : "MANUAL");
    ESP_LOGI(TAG, "Time Source        : %s", sourceText);
    ESP_LOGI(TAG, "Time Valid         : %s", isCurrentTimeValid() ? "Yes" : "No");

    if (mode == TimeMode::AUTOMATIC) {
        ESP_LOGI(TAG, "SNTP running       : %s", sntpActive_ ? "Yes" : "No");
    }

    if (!isCurrentTimeValid()) {
        ESP_LOGI(TAG, "Status             : %s",
                 mode == TimeMode::AUTOMATIC ? "WAITING FOR INTERNET TIME" : "WAITING FOR MANUAL TIME ENTRY");
        ESP_LOGI(TAG, "====================================================================");
        return;
    }

    const std::time_t nowUtc = std::time(nullptr);
    char utcText[32] = {};
    std::tm utcDateTime{};
    if (gmtime_r(&nowUtc, &utcDateTime) != nullptr) {
        std::strftime(utcText, sizeof(utcText), "%Y-%m-%d %H:%M:%S", &utcDateTime);
    }

    std::tm localDateTime{};
    char localText[32] = {};
    if (getCurrentLocalDateTime(localDateTime)) {
        std::strftime(localText, sizeof(localText), "%Y-%m-%d %H:%M:%S", &localDateTime);
    }

    ESP_LOGI(TAG, "Current UTC time   : %s", utcText);
    ESP_LOGI(TAG, "Current local time : %s", localText);
    ESP_LOGI(TAG, "Current local hour : %u", static_cast<unsigned int>(localDateTime.tm_hour));
    ESP_LOGI(TAG, "Current local min. : %u", static_cast<unsigned int>(localDateTime.tm_min));
    ESP_LOGI(TAG, "====================================================================");
#endif
}


void CurrentTimeProvider::handleTimeSynchronizationEvent(struct timeval* synchronizedTime)
{
    (void)synchronizedTime;

#ifdef ESP_PLATFORM
    if (instance_ == nullptr) {
        return;
    }

    if (instance_->timeMode_.load() != TimeMode::AUTOMATIC) {
        ESP_LOGW(TAG, "Ignoring an SNTP synchronization event received while Time Mode is not AUTOMATIC");
        return;
    }

    instance_->timeSource_.store(TimeSource::NTP);

    ESP_LOGI(TAG, "System time synchronized via SNTP");
#endif
}


} // namespace kilowatts
