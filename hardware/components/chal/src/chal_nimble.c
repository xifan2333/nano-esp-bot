#include "chal_nimble.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "store/config/ble_store_config.h"

extern void ble_store_config_init(void);

static const char *TAG = "chal_nimble";

#define MAX_SYNC_CBS 4

static SemaphoreHandle_t s_nimble_mutex = NULL;
static int s_ref_count = 0;
static bool s_inited = false;
static bool s_host_started = false;
static bool s_synced = false;

static chal_nimble_sync_cb_t s_sync_cbs[MAX_SYNC_CBS] = {0};
static size_t s_sync_cbs_count = 0;

static void nimble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task running");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_nimble_sync(void)
{
    s_synced = true;
    ESP_LOGI(TAG, "NimBLE host synchronized");

    if (s_nimble_mutex && xSemaphoreTake(s_nimble_mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < s_sync_cbs_count; i++) {
            if (s_sync_cbs[i]) {
                s_sync_cbs[i]();
            }
        }
        xSemaphoreGive(s_nimble_mutex);
    }
}

static void on_nimble_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason);
}

esp_err_t chal_nimble_ensure_inited(void)
{
    if (!s_nimble_mutex) {
        s_nimble_mutex = xSemaphoreCreateMutex();
        if (!s_nimble_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_nimble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_inited) {
        s_ref_count++;
        xSemaphoreGive(s_nimble_mutex);
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_nimble_mutex);
        return ret;
    }

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_nimble_mutex);
        return ret;
    }

    ble_hs_cfg.reset_cb = on_nimble_reset;
    ble_hs_cfg.sync_cb = on_nimble_sync;
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    s_inited = true;
    s_ref_count = 1;

    xSemaphoreGive(s_nimble_mutex);
    return ESP_OK;
}

esp_err_t chal_nimble_start_host(void)
{
    if (!s_nimble_mutex || xSemaphoreTake(s_nimble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_inited) {
        xSemaphoreGive(s_nimble_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_host_started) {
        nimble_port_freertos_init(nimble_host_task);
        s_host_started = true;
    }

    xSemaphoreGive(s_nimble_mutex);
    return ESP_OK;
}

esp_err_t chal_nimble_deinit(void)
{
    if (s_nimble_mutex && xSemaphoreTake(s_nimble_mutex, portMAX_DELAY) == pdTRUE) {
        if (!s_inited) {
            xSemaphoreGive(s_nimble_mutex);
            return ESP_OK;
        }

        s_ref_count--;
        if (s_ref_count <= 0) {
            s_ref_count = 0;
            if (s_host_started) {
                nimble_port_stop();
                s_host_started = false;
            }
            nimble_port_deinit();
            s_inited = false;
            s_synced = false;
            s_sync_cbs_count = 0;
        }

        xSemaphoreGive(s_nimble_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool chal_nimble_is_inited(void)
{
    return s_inited;
}

bool chal_nimble_is_synced(void)
{
    return s_synced;
}

esp_err_t chal_nimble_register_sync_cb(chal_nimble_sync_cb_t cb)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_nimble_mutex && xSemaphoreTake(s_nimble_mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < s_sync_cbs_count; i++) {
            if (s_sync_cbs[i] == cb) {
                xSemaphoreGive(s_nimble_mutex);
                return ESP_OK;
            }
        }
        if (s_sync_cbs_count < MAX_SYNC_CBS) {
            s_sync_cbs[s_sync_cbs_count++] = cb;
            if (s_synced) {
                cb();
            }
            xSemaphoreGive(s_nimble_mutex);
            return ESP_OK;
        }
        xSemaphoreGive(s_nimble_mutex);
        return ESP_ERR_NO_MEM;
    }
    return ESP_ERR_TIMEOUT;
}
