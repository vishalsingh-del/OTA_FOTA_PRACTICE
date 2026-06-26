#include "led_blink.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

// Choose a valid GPIO pin for ESP32-S3 devkitc-1
#define blink_led GPIO_NUM_47

void led_blink_init(void)
{
    gpio_config_t io_conf = {
        // .pin_bit_mask = (1ULL << led_1) | (1ULL << led_2) | (1ULL << blink_led),
        .pin_bit_mask = (1ULL << blink_led),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);
    // gpio_set_level(led_1, 0);
    // gpio_set_level(led_2, 0);
    gpio_set_level(blink_led, 0);
}

void led_blink(void)
{
    // gpio_set_level(led_1, 1);
    // gpio_set_level(led_2, 0);
    gpio_set_level(blink_led, 1);
    printf("LED blinked\n");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    // gpio_set_level(led_1, 0);
    // gpio_set_level(led_2, 1);
    gpio_set_level(blink_led, 0);
    printf("LED off\n");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
}