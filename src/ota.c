#include "ota.h"

#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h" // <-- add this

extern const uint8_t cert_pem_start[] asm("_binary_cert_pem_start");
extern const uint8_t cert_pem_end[] asm("_binary_cert_pem_end");

static const char *TAG = "OTA";

void ota_task(void *pv)
{
    // Mark current app as valid — prevents re-flashing on every boot
    esp_ota_mark_app_valid_cancel_rollback();

    esp_http_client_config_t config = {
        .url = "https://192.168.31.237:8000/firmware.bin",
        .cert_pem = (const char *)cert_pem_start,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    ESP_LOGI(TAG, "OTA URL: %s", config.url);

    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "OTA Successful");
        esp_restart();
    }
    else
    {
        ESP_LOGE(TAG, "OTA Failed: %s", esp_err_to_name(ret));
    }

    vTaskDelete(NULL);
}

// #include "ota.h"

// #include "esp_https_ota.h"
// #include "esp_log.h"
// #include "esp_system.h"

// extern const uint8_t cert_pem_start[] asm("_binary_cert_pem_start");
// extern const uint8_t cert_pem_end[] asm("_binary_cert_pem_end");

// static const char *TAG = "OTA";

// void ota_task(void *pv)
// {
//     esp_http_client_config_t config = {

//         .url = "https://192.168.31.237:8000/firmware.bin",

//         .cert_pem = (const char *)cert_pem_start,

//         .timeout_ms = 10000,

//         .keep_alive_enable = true,

//     };

//     esp_https_ota_config_t ota_config = {

//         .http_config = &config,

//     };

//     ESP_LOGI(TAG, "OTA URL: %s", config.url);

//     esp_err_t ret = esp_https_ota(&ota_config);

//     if (ret == ESP_OK)
//     {
//         ESP_LOGI(TAG, "OTA Successful");

//         esp_restart();
//     }
//     else
//     {
//         ESP_LOGE(TAG, "OTA Failed: %s", esp_err_to_name(ret));
//     }

//     vTaskDelete(NULL);
// }
