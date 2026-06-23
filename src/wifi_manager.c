/**
 * wifi_manager.c
 *
 * Connects the ESP32-S3 to Wi-Fi and persists credentials in NVS so
 * the device reconnects automatically on every subsequent boot.
 *
 * Default credentials (used only on first boot or if NVS is erased):
 *   SSID     : AirFiber-tbegur
 *   Password : 9014396321
 */

#include "wifi_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

/* ── Constants ────────────────────────────────────────────────────────────── */

static const char *TAG = "wifi_manager";

/* NVS namespace / keys */
#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "password"

/* Default credentials (first-boot fallback) */
#define DEFAULT_SSID "AirFiber-tbegur"
#define DEFAULT_PASS "9014396321"

/* How many times to retry before giving up */
#define WIFI_MAX_RETRY 5

/* FreeRTOS event-group bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/* ── Module-level state ───────────────────────────────────────────────────── */

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;

/* ── Internal helpers ────────────────────────────────────────────────────── */

/**
 * @brief Load SSID and password from NVS.
 *        Falls back to compile-time defaults when no entry is found.
 */
static esp_err_t load_credentials_from_nvs(char *ssid, size_t ssid_len,
                                           char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        /* Namespace doesn't exist yet → use defaults */
        ESP_LOGI(TAG, "NVS namespace not found, using default credentials");
        strlcpy(ssid, DEFAULT_SSID, ssid_len);
        strlcpy(pass, DEFAULT_PASS, pass_len);
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Read SSID */
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        strlcpy(ssid, DEFAULT_SSID, ssid_len);
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Reading SSID failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    /* Read password */
    err = nvs_get_str(handle, NVS_KEY_PASS, pass, &pass_len);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        strlcpy(pass, DEFAULT_PASS, pass_len);
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Reading password failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded credentials from NVS (SSID: %s)", ssid);
    return ESP_OK;
}

/**
 * @brief Write SSID and password to NVS.
 */
static esp_err_t save_credentials_to_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open (RW) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_set_str(handle, NVS_KEY_PASS, pass);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_commit(handle);

cleanup:
    nvs_close(handle);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Credentials saved to NVS (SSID: %s)", ssid);
    }
    else
    {
        ESP_LOGE(TAG, "Saving credentials failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ── Wi-Fi event handler ─────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting …");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *d =
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Disconnected (reason %d)", d->reason);

            if (s_retry_count < WIFI_MAX_RETRY)
            {
                s_retry_count++;
                ESP_LOGI(TAG, "Retry %d/%d …", s_retry_count, WIFI_MAX_RETRY);
                esp_wifi_connect();
            }
            else
            {
                ESP_LOGE(TAG, "Max retries reached");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            break;
        }

        default:
            break;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t wifi_manager_init(void)
{
    char ssid[32 + 1] = {0};
    char pass[64 + 1] = {0};

    /* 1. NVS must be initialised before we can use it.
     *    Call nvs_flash_init(); if already done elsewhere in app_main this
     *    is a no-op (ESP_ERR_NVS_NO_FREE_PAGES / INVALID_STATE handled). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition issue — erasing and reinitialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Load credentials (NVS → defaults fallback) */
    ESP_ERROR_CHECK(load_credentials_from_nvs(ssid, sizeof(ssid),
                                              pass, sizeof(pass)));

    /* 3. Persist defaults on very first boot so future calls always read NVS */
    save_credentials_to_nvs(ssid, pass); /* silently ignore errors here */

    /* 4. Networking stack */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* 5. Wi-Fi driver */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 6. Event group for synchronisation */
    s_wifi_event_group = xEventGroupCreate();

    /* 7. Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* 8. Configure STA */
    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    /* Allow WPA2/WPA3 mixed environments */
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start()); /* triggers WIFI_EVENT_STA_START */

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    /* 9. Block until connected or failed */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, /* don't clear on exit */
                                           pdFALSE, /* wait for either bit */
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to %s ", ssid);

        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to %s", ssid);
    return ESP_FAIL;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return save_credentials_to_nvs(ssid, password);
}