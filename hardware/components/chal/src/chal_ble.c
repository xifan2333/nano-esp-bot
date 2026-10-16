#include "chal_ble.h"
#include "chal_nimble.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "host/ble_ibeacon.h"
#include "host/ble_store.h"
#include "lauxlib.h"

static const char *TAG = "chal_ble";

static SemaphoreHandle_t s_ble_mutex = NULL;
static bool s_inited = false;
static bool s_advertising = false;
static bool s_connected = false;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

typedef enum {
    ADV_MODE_NONE = 0,
    ADV_MODE_IBEACON,
    ADV_MODE_CUSTOM,
    ADV_MODE_RAW
} ble_adv_mode_t;

static ble_adv_mode_t s_adv_mode = ADV_MODE_NONE;
static uint32_t s_adv_interval_ms = 50;
static bool s_adv_connectable = false;

// Saved configurations for advertising restarts
static uint8_t s_ibeacon_uuid[16] = {0};
static uint16_t s_ibeacon_major = 0;
static uint16_t s_ibeacon_minor = 0;
static int8_t s_ibeacon_power = -59;

static char s_custom_name[32] = {0};
static uint8_t s_custom_mfg_data[31] = {0};
static size_t s_custom_mfg_len = 0;
static uint16_t s_custom_uuid16 = 0;

static uint8_t s_raw_adv_data[31] = {0};
static size_t s_raw_adv_len = 0;
static uint8_t s_raw_rsp_data[31] = {0};
static size_t s_raw_rsp_len = 0;

static int chal_ble_gap_event(struct ble_gap_event *event, void *arg);

static esp_err_t do_adv_start(void)
{
    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type: %d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = s_adv_connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(s_adv_interval_ms);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(s_adv_interval_ms + 20);

    rc = ble_gap_adv_start(own_addr_type, NULL, 180000, &adv_params, chal_ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        s_advertising = false;
        return ESP_FAIL;
    }

    s_advertising = true;
    return ESP_OK;
}

static esp_err_t chal_ble_restart_adv(void)
{
    if (!s_inited || s_adv_mode == ADV_MODE_NONE) {
        return ESP_OK;
    }

    if (s_adv_mode == ADV_MODE_IBEACON) {
        ble_ibeacon_set_adv_data(s_ibeacon_uuid, s_ibeacon_major, s_ibeacon_minor, s_ibeacon_power);
    } else if (s_adv_mode == ADV_MODE_CUSTOM) {
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.tx_pwr_lvl_is_present = 1;
        fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

        if (strlen(s_custom_name) > 0) {
            fields.name = (const uint8_t *)s_custom_name;
            fields.name_len = strlen(s_custom_name);
            fields.name_is_complete = 1;
        }

        ble_uuid16_t uuid16_obj;
        if (s_custom_uuid16 != 0) {
            uuid16_obj = (ble_uuid16_t)BLE_UUID16_INIT(s_custom_uuid16);
            fields.uuids16 = &uuid16_obj;
            fields.num_uuids16 = 1;
            fields.uuids16_is_complete = 1;
        }

        if (s_custom_mfg_len > 0) {
            fields.mfg_data = s_custom_mfg_data;
            fields.mfg_data_len = s_custom_mfg_len;
        }

        ble_gap_adv_set_fields(&fields);
    } else if (s_adv_mode == ADV_MODE_RAW) {
        if (s_raw_adv_len > 0) {
            ble_gap_adv_set_data(s_raw_adv_data, (int)s_raw_adv_len);
        }
        if (s_raw_rsp_len > 0) {
            ble_gap_adv_rsp_set_data(s_raw_rsp_data, (int)s_raw_rsp_len);
        }
    }

    return do_adv_start();
}

static int chal_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE connection %s (status=%d, conn_handle=%d)",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status, event->connect.conn_handle);
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            s_advertising = false;
        } else if (s_adv_mode != ADV_MODE_NONE) {
            chal_ble_restart_adv();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason=%d)", event->disconnect.reason);
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_adv_mode != ADV_MODE_NONE) {
            chal_ble_restart_adv();
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGD(TAG, "BLE advertise complete (reason=%d)", event->adv_complete.reason);
        if (s_adv_mode != ADV_MODE_NONE && !s_connected) {
            chal_ble_restart_adv();
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        break;
    }
    return 0;
}

static void on_nimble_sync_cb(void)
{
    if (s_inited && s_adv_mode != ADV_MODE_NONE && !s_connected) {
        chal_ble_restart_adv();
    }
}

esp_err_t chal_ble_init(void)
{
    if (!s_ble_mutex) {
        s_ble_mutex = xSemaphoreCreateMutex();
        if (!s_ble_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_inited) {
        xSemaphoreGive(s_ble_mutex);
        return ESP_OK;
    }

    esp_err_t ret = chal_nimble_ensure_inited();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_ble_mutex);
        return ret;
    }

    ret = chal_nimble_start_host();
    if (ret != ESP_OK) {
        chal_nimble_deinit();
        xSemaphoreGive(s_ble_mutex);
        return ret;
    }

    chal_nimble_register_sync_cb(on_nimble_sync_cb);

    s_inited = true;
    xSemaphoreGive(s_ble_mutex);
    return ESP_OK;
}

esp_err_t chal_ble_deinit(void)
{
    if (!s_inited) {
        return ESP_OK;
    }

    if (s_ble_mutex && xSemaphoreTake(s_ble_mutex, portMAX_DELAY) == pdTRUE) {
        chal_ble_adv_stop();
        chal_nimble_deinit();
        s_inited = false;
        s_advertising = false;
        s_connected = false;
        s_adv_mode = ADV_MODE_NONE;
        xSemaphoreGive(s_ble_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool chal_ble_is_initialized(void)
{
    return s_inited;
}

bool chal_ble_is_advertising(void)
{
    return s_advertising;
}

bool chal_ble_is_connected(void)
{
    return s_connected;
}

esp_err_t chal_ble_get_address(char *addr_str, size_t max_len)
{
    if (!addr_str || max_len < 18) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        return ESP_FAIL;
    }

    uint8_t addr[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    if (rc != 0) {
        return ESP_FAIL;
    }

    snprintf(addr_str, max_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return ESP_OK;
}

esp_err_t chal_ble_adv_start_ibeacon(const uint8_t uuid[16], uint16_t major, uint16_t minor,
                                    int8_t measured_power, uint32_t interval_ms)
{
    if (!uuid) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = chal_ble_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_adv_mode = ADV_MODE_IBEACON;
    memcpy(s_ibeacon_uuid, uuid, 16);
    s_ibeacon_major = major;
    s_ibeacon_minor = minor;
    s_ibeacon_power = measured_power;
    s_adv_interval_ms = (interval_ms < 20) ? 20 : interval_ms;
    s_adv_connectable = false;

    ret = chal_ble_restart_adv();
    xSemaphoreGive(s_ble_mutex);
    return ret;
}

esp_err_t chal_ble_adv_start_custom(const char *name, const uint8_t *mfg_data, size_t mfg_len,
                                   uint16_t uuid16, uint32_t interval_ms, bool connectable)
{
    esp_err_t ret = chal_ble_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_adv_mode = ADV_MODE_CUSTOM;
    if (name) {
        strncpy(s_custom_name, name, sizeof(s_custom_name) - 1);
        s_custom_name[sizeof(s_custom_name) - 1] = '\0';
    } else {
        s_custom_name[0] = '\0';
    }

    if (mfg_data && mfg_len > 0) {
        size_t copy_len = (mfg_len > sizeof(s_custom_mfg_data)) ? sizeof(s_custom_mfg_data) : mfg_len;
        memcpy(s_custom_mfg_data, mfg_data, copy_len);
        s_custom_mfg_len = copy_len;
    } else {
        s_custom_mfg_len = 0;
    }

    s_custom_uuid16 = uuid16;
    s_adv_interval_ms = (interval_ms < 20) ? 20 : interval_ms;
    s_adv_connectable = connectable;

    ret = chal_ble_restart_adv();
    xSemaphoreGive(s_ble_mutex);
    return ret;
}

esp_err_t chal_ble_adv_start_raw(const uint8_t *raw_adv, size_t raw_adv_len,
                                const uint8_t *scan_rsp, size_t scan_rsp_len,
                                uint32_t interval_ms, bool connectable)
{
    esp_err_t ret = chal_ble_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_adv_mode = ADV_MODE_RAW;
    if (raw_adv && raw_adv_len > 0) {
        size_t copy_len = (raw_adv_len > sizeof(s_raw_adv_data)) ? sizeof(s_raw_adv_data) : raw_adv_len;
        memcpy(s_raw_adv_data, raw_adv, copy_len);
        s_raw_adv_len = copy_len;
    } else {
        s_raw_adv_len = 0;
    }

    if (scan_rsp && scan_rsp_len > 0) {
        size_t copy_len = (scan_rsp_len > sizeof(s_raw_rsp_data)) ? sizeof(s_raw_rsp_data) : scan_rsp_len;
        memcpy(s_raw_rsp_data, scan_rsp, copy_len);
        s_raw_rsp_len = copy_len;
    } else {
        s_raw_rsp_len = 0;
    }

    s_adv_interval_ms = (interval_ms < 20) ? 20 : interval_ms;
    s_adv_connectable = connectable;

    ret = chal_ble_restart_adv();
    xSemaphoreGive(s_ble_mutex);
    return ret;
}

esp_err_t chal_ble_adv_stop(void)
{
    if (s_ble_mutex && xSemaphoreTake(s_ble_mutex, portMAX_DELAY) == pdTRUE) {
        s_adv_mode = ADV_MODE_NONE;
        s_advertising = false;
        ble_gap_adv_stop();
        xSemaphoreGive(s_ble_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

void chal_ble_teardown(void)
{
    if (s_inited) {
        chal_ble_adv_stop();
    }
}

// Stubs for Task 3 dynamic GATT
esp_err_t chal_ble_gatts_register_service(const char *svc_uuid, const char *char_uuid,
                                          uint8_t properties, const uint8_t *init_val, size_t val_len)
{
    (void)svc_uuid;
    (void)char_uuid;
    (void)properties;
    (void)init_val;
    (void)val_len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t chal_ble_gatts_set_value(const char *char_uuid, const uint8_t *val, size_t len)
{
    (void)char_uuid;
    (void)val;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t chal_ble_gatts_notify(const char *char_uuid, const uint8_t *val, size_t len)
{
    (void)char_uuid;
    (void)val;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

int luaopen_ble(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
