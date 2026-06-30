#include "ota.h"
#include "cert.h"

#include <string.h>
#include <stdlib.h>

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include "cJSON.h"
static const char *TAG = "OTA";
#define MAX_HTTP_OUTPUT_BUFFER 512

static char http_buffer[MAX_HTTP_OUTPUT_BUFFER];
static int http_len = 0;

#define VERSION_URL "https://192.168.31.237:8000/version.json"
static esp_err_t http_event_handler(esp_http_client_event_t *evt);

static esp_err_t download_version_info(ota_info_t *info)
{
    http_len = 0;
    memset(http_buffer, 0, sizeof(http_buffer));

    esp_http_client_config_t config =
        {
            .url = VERSION_URL,
            .cert_pem = cert_pem,
            .event_handler = http_event_handler,
            .timeout_ms = 10000,
        };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "HTTP Error : %s",
                 esp_err_to_name(err));

        esp_http_client_cleanup(client);

        return err;
    }

    ESP_LOGI(TAG,
             "HTTP Status = %d",
             esp_http_client_get_status_code(client));

    ESP_LOGI(TAG,
             "Received %d bytes",
             http_len);

    http_buffer[http_len] = 0;

    ESP_LOGI(TAG,
             "JSON:\n%s",
             http_buffer);

    esp_http_client_cleanup(client);

    cJSON *root = cJSON_Parse(http_buffer);

    if (root == NULL)
    {
        ESP_LOGE(TAG, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *version =
        cJSON_GetObjectItem(root, "version");

    cJSON *url =
        cJSON_GetObjectItem(root, "url");

    if (!cJSON_IsString(version) ||
        !cJSON_IsString(url))
    {
        cJSON_Delete(root);

        ESP_LOGE(TAG,
                 "version.json missing fields");

        return ESP_FAIL;
    }

    strncpy(info->version_server,
            version->valuestring,
            sizeof(info->version_server) - 1);

    strncpy(info->url,
            url->valuestring,
            sizeof(info->url) - 1);

    cJSON_Delete(root);

    return ESP_OK;
}
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:

        if (!esp_http_client_is_chunked_response(evt->client))
        {
            if ((http_len + evt->data_len) < MAX_HTTP_OUTPUT_BUFFER)
            {
                memcpy(http_buffer + http_len,
                       evt->data,
                       evt->data_len);

                http_len += evt->data_len;
            }
        }

        break;

    default:
        break;
    }

    return ESP_OK;
}
static bool compare_versions(const char *current,
                             const char *server)
{
    int c1, c2, c3;
    int s1, s2, s3;

    if (sscanf(current, "%d.%d.%d", &c1, &c2, &c3) != 3)
    {
        return false;
    }

    if (sscanf(server, "%d.%d.%d", &s1, &s2, &s3) != 3)
    {
        return false;
    }

    if (s1 > c1)
        return true;
    if (s1 < c1)
        return false;

    if (s2 > c2)
        return true;
    if (s2 < c2)
        return false;

    if (s3 > c3)
        return true;
    if (s3 < c3)
        return false;

    return false;
}

static esp_err_t perform_ota(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .cert_pem = cert_pem,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    ESP_LOGI(TAG, "Firmware URL : %s", url);

    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "OTA Successful");

        esp_restart();
    }

    ESP_LOGE(TAG,
             "OTA Failed : %s",
             esp_err_to_name(ret));

    return ret;
}
void ota_task(void *pv)
{
    ota_info_t ota_info;

    esp_ota_mark_app_valid_cancel_rollback();

    while (1)
    {
        if (download_version_info(&ota_info) == ESP_OK)
        {
            const esp_app_desc_t *app = esp_app_get_description();

            ESP_LOGI(TAG, "Current : %s", app->version);
            ESP_LOGI(TAG, "Server  : %s", ota_info.version_server);

            if (compare_versions(app->version,
                                 ota_info.version_server))
            {
                ESP_LOGI(TAG, "New firmware found");

                perform_ota(ota_info.url);

                // perform_ota() restarts ESP if successful
            }
        }

        // Wait 30 seconds before checking again
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
// #include "ota.h"
// #include "cert.h"

// #include "esp_https_ota.h"
// #include "esp_log.h"
// #include "esp_system.h"
// #include "esp_ota_ops.h"

// static const char *TAG = "OTA";

// void ota_task(void *pv)
// {
//     esp_ota_mark_app_valid_cancel_rollback();

//     esp_http_client_config_t config = {
//         .url = "https://192.168.31.237:8000/firmware.bin",
//         .cert_pem = cert_pem,
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
