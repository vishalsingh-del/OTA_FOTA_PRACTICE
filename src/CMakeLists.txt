idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "ota.c"
        "led_blink.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        driver
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        log
        esp_https_ota
)
