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
#include "host/ble_gatt.h"
#include "host/ble_hs_adv.h"
#include "host/ble_ibeacon.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
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
        chal_ble_gatts_reset();
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

static bool parse_uuid128(const char *str, uint8_t out[16])
{
    if (!str) return false;
    size_t len = strlen(str);
    if (len == 16) {
        memcpy(out, str, 16);
        return true;
    }
    size_t count = 0;
    uint8_t current_byte = 0;
    bool high_nibble = true;

    for (size_t i = 0; i < len && count < 16; i++) {
        char c = str[i];
        if (c == '-') continue;
        int val = -1;
        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
        else return false;

        if (high_nibble) {
            current_byte = (uint8_t)(val << 4);
            high_nibble = false;
        } else {
            current_byte |= (uint8_t)val;
            out[count++] = current_byte;
            high_nibble = true;
        }
    }
    return count == 16;
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
        chal_ble_gatts_reset();
    }
}

#define MAX_DYNAMIC_SERVICES 4
#define MAX_DYNAMIC_CHARS    16
#define MAX_CHAR_VAL_LEN     512

typedef struct {
    bool used;
    ble_uuid_any_t uuid;
    uint16_t val_handle;
    uint8_t flags;
    uint8_t val[MAX_CHAR_VAL_LEN];
    size_t val_len;
} chal_ble_char_t;

typedef struct {
    bool used;
    ble_uuid_any_t uuid;
    struct ble_gatt_svc_def svc_def[2];
    struct ble_gatt_chr_def chr_defs[MAX_DYNAMIC_CHARS + 1];
    size_t chr_count;
} chal_ble_svc_t;

static chal_ble_svc_t s_dynamic_svcs[MAX_DYNAMIC_SERVICES];
static chal_ble_char_t s_dynamic_chrs[MAX_DYNAMIC_CHARS];

static int dynamic_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    chal_ble_char_t *chr = (chal_ble_char_t *)arg;
    if (!chr || !chr->used) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        int rc = os_mbuf_append(ctxt->om, chr->val, (int)chr->val_len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, chr->val, sizeof(chr->val), &out_len);
        if (rc != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        chr->val_len = out_len;
        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static chal_ble_char_t *find_dynamic_char(const char *uuid_str)
{
    ble_uuid_any_t target_uuid;
    if (ble_uuid_from_str(&target_uuid, uuid_str) != 0) {
        return NULL;
    }
    for (size_t i = 0; i < MAX_DYNAMIC_CHARS; i++) {
        if (s_dynamic_chrs[i].used &&
            ble_uuid_cmp(&s_dynamic_chrs[i].uuid.u, &target_uuid.u) == 0) {
            return &s_dynamic_chrs[i];
        }
    }
    return NULL;
}

void chal_ble_gatts_reset(void)
{
    for (size_t i = 0; i < MAX_DYNAMIC_SERVICES; i++) {
        if (s_dynamic_svcs[i].used) {
            ble_gatts_delete_svc(&s_dynamic_svcs[i].uuid.u);
            s_dynamic_svcs[i].used = false;
        }
    }
    for (size_t i = 0; i < MAX_DYNAMIC_CHARS; i++) {
        s_dynamic_chrs[i].used = false;
        s_dynamic_chrs[i].val_len = 0;
    }
}

esp_err_t chal_ble_gatts_register_service(const char *svc_uuid_str, const char *char_uuid_str,
                                          uint8_t properties, const uint8_t *init_val, size_t val_len)
{
    if (!svc_uuid_str || !char_uuid_str) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = chal_ble_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int svc_idx = -1;
    for (size_t i = 0; i < MAX_DYNAMIC_SERVICES; i++) {
        if (!s_dynamic_svcs[i].used) {
            svc_idx = (int)i;
            break;
        }
    }
    if (svc_idx < 0) {
        xSemaphoreGive(s_ble_mutex);
        return ESP_ERR_NO_MEM;
    }

    int chr_idx = -1;
    for (size_t i = 0; i < MAX_DYNAMIC_CHARS; i++) {
        if (!s_dynamic_chrs[i].used) {
            chr_idx = (int)i;
            break;
        }
    }
    if (chr_idx < 0) {
        xSemaphoreGive(s_ble_mutex);
        return ESP_ERR_NO_MEM;
    }

    if (ble_uuid_from_str(&s_dynamic_svcs[svc_idx].uuid, svc_uuid_str) != 0) {
        xSemaphoreGive(s_ble_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    if (ble_uuid_from_str(&s_dynamic_chrs[chr_idx].uuid, char_uuid_str) != 0) {
        xSemaphoreGive(s_ble_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    s_dynamic_chrs[chr_idx].used = true;
    s_dynamic_chrs[chr_idx].flags = properties;
    if (init_val && val_len > 0) {
        size_t cpy = val_len > sizeof(s_dynamic_chrs[chr_idx].val) ? sizeof(s_dynamic_chrs[chr_idx].val) : val_len;
        memcpy(s_dynamic_chrs[chr_idx].val, init_val, cpy);
        s_dynamic_chrs[chr_idx].val_len = cpy;
    } else {
        s_dynamic_chrs[chr_idx].val_len = 0;
    }

    chal_ble_svc_t *svc = &s_dynamic_svcs[svc_idx];
    svc->used = true;
    svc->chr_count = 1;

    memset(svc->chr_defs, 0, sizeof(svc->chr_defs));
    svc->chr_defs[0].uuid = &s_dynamic_chrs[chr_idx].uuid.u;
    svc->chr_defs[0].access_cb = dynamic_chr_access_cb;
    svc->chr_defs[0].arg = &s_dynamic_chrs[chr_idx];
    svc->chr_defs[0].flags = properties;
    svc->chr_defs[0].val_handle = &s_dynamic_chrs[chr_idx].val_handle;

    memset(svc->svc_def, 0, sizeof(svc->svc_def));
    svc->svc_def[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    svc->svc_def[0].uuid = &svc->uuid.u;
    svc->svc_def[0].characteristics = svc->chr_defs;
    svc->svc_def[1].type = BLE_GATT_SVC_TYPE_END;

    int rc = ble_gatts_add_dynamic_svcs(svc->svc_def);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_dynamic_svcs failed: %d", rc);
        s_dynamic_chrs[chr_idx].used = false;
        svc->used = false;
        xSemaphoreGive(s_ble_mutex);
        return ESP_FAIL;
    }

    xSemaphoreGive(s_ble_mutex);
    return ESP_OK;
}

esp_err_t chal_ble_gatts_set_value(const char *char_uuid, const uint8_t *val, size_t len)
{
    if (!char_uuid) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    chal_ble_char_t *chr = find_dynamic_char(char_uuid);
    if (!chr) {
        xSemaphoreGive(s_ble_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    size_t copy_len = len > sizeof(chr->val) ? sizeof(chr->val) : len;
    if (val && copy_len > 0) {
        memcpy(chr->val, val, copy_len);
        chr->val_len = copy_len;
    } else {
        chr->val_len = 0;
    }
    xSemaphoreGive(s_ble_mutex);
    return ESP_OK;
}

esp_err_t chal_ble_gatts_get_value(const char *char_uuid, uint8_t *out_val, size_t max_len, size_t *out_len)
{
    if (!char_uuid || !out_val || !out_len) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    chal_ble_char_t *chr = find_dynamic_char(char_uuid);
    if (!chr) {
        xSemaphoreGive(s_ble_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    size_t cpy = chr->val_len > max_len ? max_len : chr->val_len;
    memcpy(out_val, chr->val, cpy);
    *out_len = cpy;
    xSemaphoreGive(s_ble_mutex);
    return ESP_OK;
}

esp_err_t chal_ble_gatts_notify(const char *char_uuid, const uint8_t *val, size_t len)
{
    if (!char_uuid) return ESP_ERR_INVALID_ARG;
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    chal_ble_char_t *chr = find_dynamic_char(char_uuid);
    if (!chr) {
        xSemaphoreGive(s_ble_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (val && len > 0) {
        size_t cpy = len > sizeof(chr->val) ? sizeof(chr->val) : len;
        memcpy(chr->val, val, cpy);
        chr->val_len = cpy;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(chr->val, (uint16_t)chr->val_len);
    if (!om) {
        xSemaphoreGive(s_ble_mutex);
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, chr->val_handle, om);
    xSemaphoreGive(s_ble_mutex);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static int lua_ble_init(lua_State *L)
{
    esp_err_t err = chal_ble_init();
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_deinit(lua_State *L)
{
    esp_err_t err = chal_ble_deinit();
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_status(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, chal_ble_is_initialized());
    lua_setfield(L, -2, "initialized");
    lua_pushboolean(L, chal_ble_is_advertising());
    lua_setfield(L, -2, "advertising");
    lua_pushboolean(L, chal_ble_is_connected());
    lua_setfield(L, -2, "connected");

    char addr_str[18] = {0};
    if (chal_ble_get_address(addr_str, sizeof(addr_str)) == ESP_OK) {
        lua_pushstring(L, addr_str);
        lua_setfield(L, -2, "addr");
    }
    return 1;
}

static int lua_ble_adv_start(lua_State *L)
{
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "ble.adv_start requires an options table");
    }

    uint32_t interval_ms = 50;
    lua_getfield(L, 1, "interval");
    if (lua_isinteger(L, -1)) {
        interval_ms = (uint32_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    bool connectable = false;
    lua_getfield(L, 1, "connectable");
    if (!lua_isnil(L, -1)) {
        connectable = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "ibeacon");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "uuid");
        const char *uuid_str = luaL_optstring(L, -1, "fda50693-a4e2-4fb1-afcf-c6eb07647825");
        uint8_t uuid[16] = {0};
        parse_uuid128(uuid_str, uuid);
        lua_pop(L, 1);

        lua_getfield(L, -1, "major");
        uint16_t major = (uint16_t)luaL_optinteger(L, -1, 10001);
        lua_pop(L, 1);

        lua_getfield(L, -1, "minor");
        uint16_t minor = (uint16_t)luaL_optinteger(L, -1, 19641);
        lua_pop(L, 1);

        lua_getfield(L, -1, "power");
        int8_t power = (int8_t)luaL_optinteger(L, -1, -59);
        lua_pop(L, 1);

        lua_pop(L, 1);

        esp_err_t err = chal_ble_adv_start_ibeacon(uuid, major, minor, power, interval_ms);
        if (err != ESP_OK) {
            lua_pushnil(L);
            lua_pushstring(L, esp_err_to_name(err));
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "raw");
    if (lua_isstring(L, -1)) {
        size_t raw_len = 0;
        const char *raw_data = lua_tolstring(L, -1, &raw_len);

        size_t rsp_len = 0;
        const char *rsp_data = NULL;
        lua_getfield(L, 1, "raw_scan_rsp");
        if (lua_isstring(L, -1)) {
            rsp_data = lua_tolstring(L, -1, &rsp_len);
        }
        lua_pop(L, 1);

        esp_err_t err = chal_ble_adv_start_raw((const uint8_t *)raw_data, raw_len,
                                               (const uint8_t *)rsp_data, rsp_len,
                                               interval_ms, connectable);
        lua_pop(L, 1);
        if (err != ESP_OK) {
            lua_pushnil(L);
            lua_pushstring(L, esp_err_to_name(err));
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pop(L, 1);

    const char *name = NULL;
    lua_getfield(L, 1, "name");
    if (lua_isstring(L, -1)) {
        name = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    size_t mfg_len = 0;
    const char *mfg_data = NULL;
    lua_getfield(L, 1, "mfg_data");
    if (lua_isstring(L, -1)) {
        mfg_data = lua_tolstring(L, -1, &mfg_len);
    }
    lua_pop(L, 1);

    uint16_t uuid16 = 0;
    lua_getfield(L, 1, "uuid");
    if (lua_isinteger(L, -1)) {
        uuid16 = (uint16_t)lua_tointeger(L, -1);
    } else if (lua_isstring(L, -1)) {
        const char *u_str = lua_tostring(L, -1);
        ble_uuid_any_t u;
        if (ble_uuid_from_str(&u, u_str) == 0 && u.u.type == BLE_UUID_TYPE_16) {
            uuid16 = u.u16.value;
        }
    }
    lua_pop(L, 1);

    esp_err_t err = chal_ble_adv_start_custom(name, (const uint8_t *)mfg_data, mfg_len,
                                              uuid16, interval_ms, connectable);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_adv_stop(lua_State *L)
{
    esp_err_t err = chal_ble_adv_stop();
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_gatts_define(lua_State *L)
{
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "ble.gatts_define requires a service definition table");
    }

    lua_getfield(L, 1, "uuid");
    const char *svc_uuid = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "characteristics");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "characteristics array required in gatts_define");
    }

    lua_Integer nchrs = luaL_len(L, -1);
    for (lua_Integer i = 1; i <= nchrs; i++) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "uuid");
        const char *chr_uuid = luaL_checkstring(L, -1);
        lua_pop(L, 1);

        uint8_t flags = 0;
        lua_getfield(L, -1, "flags");
        if (lua_istable(L, -1)) {
            lua_Integer nflags = luaL_len(L, -1);
            for (lua_Integer f = 1; f <= nflags; f++) {
                lua_rawgeti(L, -1, f);
                const char *flag_name = lua_tostring(L, -1);
                if (flag_name) {
                    if (strcasecmp(flag_name, "read") == 0) flags |= BLE_GATT_CHR_F_READ;
                    else if (strcasecmp(flag_name, "write") == 0) flags |= BLE_GATT_CHR_F_WRITE;
                    else if (strcasecmp(flag_name, "notify") == 0) flags |= BLE_GATT_CHR_F_NOTIFY;
                    else if (strcasecmp(flag_name, "indicate") == 0) flags |= BLE_GATT_CHR_F_INDICATE;
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        size_t val_len = 0;
        const char *val_data = NULL;
        lua_getfield(L, -1, "value");
        if (lua_isstring(L, -1)) {
            val_data = lua_tolstring(L, -1, &val_len);
        }
        lua_pop(L, 1);

        esp_err_t err = chal_ble_gatts_register_service(svc_uuid, chr_uuid, flags,
                                                        (const uint8_t *)val_data, val_len);
        lua_pop(L, 1);
        if (err != ESP_OK) {
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_pushstring(L, esp_err_to_name(err));
            return 2;
        }
    }
    lua_pop(L, 1);

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_gatts_set_value(lua_State *L)
{
    const char *char_uuid = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *val = luaL_checklstring(L, 2, &len);

    esp_err_t err = chal_ble_gatts_set_value(char_uuid, (const uint8_t *)val, len);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_gatts_get_value(lua_State *L)
{
    const char *char_uuid = luaL_checkstring(L, 1);
    uint8_t buf[MAX_CHAR_VAL_LEN];
    size_t out_len = 0;

    esp_err_t err = chal_ble_gatts_get_value(char_uuid, buf, sizeof(buf), &out_len);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushlstring(L, (const char *)buf, out_len);
    return 1;
}

static int lua_ble_notify(lua_State *L)
{
    const char *char_uuid = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *val = NULL;
    if (lua_isstring(L, 2)) {
        val = lua_tolstring(L, 2, &len);
    }

    esp_err_t err = chal_ble_gatts_notify(char_uuid, (const uint8_t *)val, len);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg s_ble_funcs[] = {
    {"init", lua_ble_init},
    {"deinit", lua_ble_deinit},
    {"status", lua_ble_status},
    {"adv_start", lua_ble_adv_start},
    {"adv_stop", lua_ble_adv_stop},
    {"gatts_define", lua_ble_gatts_define},
    {"gatts_set_value", lua_ble_gatts_set_value},
    {"gatts_get_value", lua_ble_gatts_get_value},
    {"notify", lua_ble_notify},
    {NULL, NULL}
};

int luaopen_ble(lua_State *L)
{
    luaL_newlib(L, s_ble_funcs);
    return 1;
}
