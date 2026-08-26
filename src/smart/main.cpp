// #if defined(DEVICE_ROLE_SMART)
// #include "SmartApplication.h"

// extern "C" void app_main()
// {
//     SmartApplication app;
//     app.runApp();
// }

// #endif // DEVICE_ROLE_SMART


#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#if defined(DEVICE_ROLE_SMART)

static const char *TAG = "TEST_BOOT";

extern "C" void app_main()
{
    while (1) {
        ESP_LOGI(TAG, "Smart Node Flash Success! Hardware alive.");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif // DEVICE_ROLE_SMART
