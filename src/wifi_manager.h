#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize Wi-Fi in STA mode and connect using credentials
     *        stored in NVS (or the compile-time defaults on first boot).
     *
     * Blocks until the IP address is obtained or the connection attempt
     * times out.  On success the credentials are persisted to NVS so
     * every subsequent boot reconnects automatically.
     *
     * @return ESP_OK on successful connection
     */
    esp_err_t wifi_manager_init(void);

    /**
     * @brief Save new Wi-Fi credentials to NVS and reconnect.
     *
     * @param ssid      NULL-terminated SSID  (max 32 chars)
     * @param password  NULL-terminated password (max 64 chars)
     * @return ESP_OK on success
     */
    esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif