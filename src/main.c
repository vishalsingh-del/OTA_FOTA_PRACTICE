#include "led_blink.h"
#include "wifi_manager.h"
// #include "ota.h"

#include "esp_log.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fota_can.h"
#include "app_can.h"
#include "led_blink.h"

// #include "esp_log.h"

// static const char *TAG = "MAIN";

void app_main(void)
{
    led_blink_init();
    wifi_manager_init();
    can_app_start(); // Initialize CAN interface

    ESP_LOGI("TEST", "Starting CAN FOTA");

    fota_can_run(); // Starts the complete CAN FOTA process

    while (1)
    {
        led_blink();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}