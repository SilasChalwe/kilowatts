/**
 * @file RelayController.cpp
 * @brief Implements local relay GPIO actuation for the Loads wired to THIS Node.
 *
 * This file is split by ESP_PLATFORM, the macro ESP-IDF defines for every
 * source file it builds, the same way INA219Monitor.cpp and
 * RelayController.h are:
 *
 * - Relay/control-channel registration is plain, hardware-free C++.
 * - Real GPIO configuration/write is compiled only under
 *   ESP_PLATFORM, using ESP-IDF's driver/gpio.h. A host build's version of
 *   every hardware-touching method simply returns false without
 *   fabricating hardware behaviour.
 */

#include "RelayController.h"

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "esp_log.h"
#endif

namespace kilowatts {


#ifdef ESP_PLATFORM
static const char *TAG = "RELAY_CONTROLLER";
#endif


RelayController::RelayController()
    : relays_()
{
}


RelayController::~RelayController()
{
}


RelayController::RegisteredRelay* RelayController::findMutableRelay(std::uint8_t relayPin)
{
    for (RegisteredRelay& relay : relays_) {
        if (relay.configuration.relayPin == relayPin) {
            return &relay;
        }
    }

    return nullptr;
}


const RelayController::RegisteredRelay* RelayController::findRelay(std::uint8_t relayPin) const
{
    for (const RegisteredRelay& relay : relays_) {
        if (relay.configuration.relayPin == relayPin) {
            return &relay;
        }
    }

    return nullptr;
}


bool RelayController::addRelay(const RelayConfiguration& configuration)
{
    if (isRelayRegistered(configuration.relayPin)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected relay: pin %u is already registered",
                 static_cast<unsigned int>(configuration.relayPin));
#endif
        return false;
    }

    RegisteredRelay relay{};
    relay.configuration = configuration;
    relay.hardwareApplied = false;

    relays_.push_back(relay);

    const bool hardwareConfigured = configureGpio(relays_.back());
    relays_.back().hardwareApplied = hardwareConfigured;

#ifdef ESP_PLATFORM
    if (!hardwareConfigured) {
        ESP_LOGE(TAG, "Relay pin %u rejected because GPIO configuration failed",
                 static_cast<unsigned int>(configuration.relayPin));
    } else {
        ESP_LOGI(TAG, "Relay pin %u registered: activeHigh=%s initialState=%s",
                 static_cast<unsigned int>(configuration.relayPin),
                 configuration.activeHigh ? "Yes" : "No",
                 configuration.initialStateOn ? "ON" : "OFF");
    }
#endif

    if (!hardwareConfigured) {
#ifdef ESP_PLATFORM
        removeRelay(configuration.relayPin);
        return false;
#else
        // Host tests keep the logical channel without pretending GPIO exists.
        return true;
#endif
    }

    return true;
}


bool RelayController::removeRelay(std::uint8_t relayPin)
{
    for (std::size_t index = 0U; index < relays_.size(); ++index) {
        RegisteredRelay& relay = relays_[index];
        if (relay.configuration.relayPin != relayPin) {
            continue;
        }

#ifdef ESP_PLATFORM
        /*
         * A channel is only removed as part of rolling back a failed
         * provisioning operation, but still drive it to a safe OFF level
         * before releasing the pin.
         */
        if (relay.hardwareApplied) {
            if (!writeGpioLevel(relay, false)) {
                ESP_LOGE(TAG, "Could not drive relay pin %u OFF while rolling back configuration",
                         static_cast<unsigned int>(relayPin));
            }
            const esp_err_t resetResult = gpio_reset_pin(
                static_cast<gpio_num_t>(relay.configuration.relayPin));
            if (resetResult != ESP_OK) {
                ESP_LOGE(TAG, "gpio_reset_pin() failed for relay pin %u during rollback: %s",
                         static_cast<unsigned int>(relayPin), esp_err_to_name(resetResult));
            }
        }
#endif

        relays_.erase(relays_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    return false;
}


std::size_t RelayController::getNumberOfRelays() const
{
    return relays_.size();
}


const RelayController::RelayConfiguration* RelayController::getRelay(std::size_t relayIndex) const
{
    if (relayIndex >= relays_.size()) {
        return nullptr;
    }

    return &relays_[relayIndex].configuration;
}


bool RelayController::isRelayRegistered(std::uint8_t relayPin) const
{
    return findRelay(relayPin) != nullptr;
}


bool RelayController::setRelayState(std::uint8_t relayPin, bool on)
{
    RegisteredRelay* relay = findMutableRelay(relayPin);

    if (relay == nullptr) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "Cannot command relay pin %u: not registered", static_cast<unsigned int>(relayPin));
#endif
        return false;
    }

    const bool written = writeGpioLevel(*relay, on);

#ifdef ESP_PLATFORM
    if (written) {
        ESP_LOGI(TAG, "Relay pin %u commanded %s", static_cast<unsigned int>(relayPin), on ? "ON" : "OFF");
    } else {
        ESP_LOGE(TAG, "Relay pin %u GPIO write failed while commanding %s",
                 static_cast<unsigned int>(relayPin), on ? "ON" : "OFF");
    }
#endif

    return written;
}


bool RelayController::isHardwareApplied(std::uint8_t relayPin) const
{
    const RegisteredRelay* relay = findRelay(relayPin);
    return relay != nullptr && relay->hardwareApplied;
}


void RelayController::printDiagnosticReport() const
{
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "RELAY CHANNELS");
    ESP_LOGI(TAG, "--------------------------------------------------------------");
    ESP_LOGI(TAG, "%-9s %-11s %-12s", "Pin", "ActiveHigh", "GPIO");

    for (const RegisteredRelay& relay : relays_) {
        ESP_LOGI(TAG, "%-9u %-11s %-12s",
                 static_cast<unsigned int>(relay.configuration.relayPin),
                 relay.configuration.activeHigh ? "Yes" : "No",
                 relay.hardwareApplied ? "CONFIGURED" : "NOT CONFIGURED");
    }

    ESP_LOGI(TAG, "--------------------------------------------------------------");
#endif
}


bool RelayController::configureGpio(RegisteredRelay& relay)
{
#ifdef ESP_PLATFORM
    const gpio_num_t pin = static_cast<gpio_num_t>(relay.configuration.relayPin);

    /*
     * IMPORTANT:
     * Safe idle level depends on relay polarity.
     *
     * activeHigh=true:
     *   ON  = HIGH
     *   OFF = LOW
     *
     * activeHigh=false / active-low:
     *   ON  = LOW
     *   OFF = HIGH
     *
     * Therefore an active-low relay must NOT be given a pull-down as its
     * safe/default state. Pull-down means LOW, which energises the relay.
     */
    const bool initialDriveHigh =
        relay.configuration.initialStateOn
            ? relay.configuration.activeHigh
            : !relay.configuration.activeHigh;

    /*
     * Set the output latch before enabling output mode so the pin starts
     * from the correct logical state as soon as it becomes an output.
     */
    (void)gpio_set_level(pin, initialDriveHigh ? 1U : 0U);

    gpio_config_t ioConfiguration{};
    ioConfiguration.pin_bit_mask = 1ULL << static_cast<unsigned int>(pin);
    ioConfiguration.mode = GPIO_MODE_OUTPUT;

    /*
     * Bias the inactive/reset direction according to polarity.
     * For active-low boards, OFF is HIGH, so use pull-up.
     * For active-high boards, OFF is LOW, so use pull-down.
     */
    ioConfiguration.pull_up_en =
        relay.configuration.activeHigh ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
    ioConfiguration.pull_down_en =
        relay.configuration.activeHigh ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    ioConfiguration.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t configureResult = gpio_config(&ioConfiguration);
    if (configureResult != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config() failed for relay pin %u: %s",
                 static_cast<unsigned int>(relay.configuration.relayPin),
                 esp_err_to_name(configureResult));
        return false;
    }

    relay.hardwareApplied = true;

    /*
     * Write again after configuration. This is the real final state.
     */
    if (!writeGpioLevel(relay, relay.configuration.initialStateOn)) {
        ESP_LOGE(TAG, "Initial state write failed for relay pin %u",
                 static_cast<unsigned int>(relay.configuration.relayPin));
        return false;
    }

    return true;
#else
    (void)relay;
    return false;
#endif
}
bool RelayController::writeGpioLevel(const RegisteredRelay& relay, bool on) const
{
#ifdef ESP_PLATFORM
    if (!relay.hardwareApplied) {
        return false;
    }

    /*
     * Correct polarity translation:
     *
     * activeHigh=true:
     *   logical ON  -> GPIO HIGH
     *   logical OFF -> GPIO LOW
     *
     * activeHigh=false / active-low:
     *   logical ON  -> GPIO LOW
     *   logical OFF -> GPIO HIGH
     */
    const bool driveHigh =
        on ? relay.configuration.activeHigh : !relay.configuration.activeHigh;

    const esp_err_t result = gpio_set_level(
        static_cast<gpio_num_t>(relay.configuration.relayPin),
        driveHigh ? 1U : 0U);

    return result == ESP_OK;
#else
    (void)relay;
    (void)on;
    return false;
#endif
}



} // namespace kilowatts
