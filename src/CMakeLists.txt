idf_component_register(
    SRCS "main.c" "globals.c" "ota.c" "fota_can.c" "wifi_manager.c" "led_blink.c" "app_can.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp_https_ota http_parser esp_event esp_netif nvs_flash esp_partition esp_system log freertos driver mbedtls
)
