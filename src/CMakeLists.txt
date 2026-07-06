idf_component_register(
    SRCS "fota_can.c" "main.c" "globals.c" "ota.c" "wifi_manager.c" "led_blink.c" "app_can.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp_https_ota http_parser esp_event esp_netif nvs_flash esp_partition esp_system log freertos driver mbedtls
)
