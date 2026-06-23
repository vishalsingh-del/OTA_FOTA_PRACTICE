#include "led_blink.h"
#include "wifi_manager.h"
#include "ota.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// #include "esp_log.h"

// static const char *TAG = "MAIN";

void app_main()
{
    led_blink_init();
    wifi_manager_init();

    // Spawn as a proper FreeRTOS task instead of calling directly
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);

    while (1)
    {
        led_blink();
    }
}