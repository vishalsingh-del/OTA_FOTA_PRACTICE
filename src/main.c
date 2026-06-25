#include "led_blink.h"
#include "wifi_manager.h"
#include "ota.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// #include "esp_log.h"

// static const char *TAG = "MAIN";

void app_main()
{
    uint32_t flash_size;
    vTaskDelay(10000 / portTICK_PERIOD_MS); // Delay to allow logging to complete
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI("FLASH", "Flash Size = %lu bytes (%.1f MB)",
             flash_size,
             flash_size / (1024.0 * 1024.0));

    led_blink_init();
    wifi_manager_init();
    ESP_LOGI("TEST", "THIS IS BUILD 2");
    // Spawn as a proper FreeRTOS task instead of calling directly
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);

    while (1)
    {
        led_blink();
    }
}