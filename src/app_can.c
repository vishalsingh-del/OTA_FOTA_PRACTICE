#include "app_can.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/twai.h"

static const char *TAG = "APP_CAN";

void can_app_start(void)
{

    gpio_set_direction(GPIO_NUM_48, GPIO_MODE_OUTPUT);

    gpio_set_level(GPIO_NUM_48, 0);

    // twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_RX_GPIO, CAN_TX_GPIO, TWAI_MODE_NORMAL);   //old esp
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, TWAI_MODE_NORMAL); // new esp
    twai_timing_config_t t_config = CAN_BAUD_RATE();

    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
    ESP_ERROR_CHECK(twai_start());

    uint32_t alerts = TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_OFF;
    ESP_ERROR_CHECK(twai_reconfigure_alerts(alerts, NULL));
    ESP_LOGI(TAG, "CAN initialized successfully.");
}