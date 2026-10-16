#include "chal_ble_hid.h"
#include "chal_nimble.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"
#include "lauxlib.h"

static const char *TAG = "chal_ble_hid";

#define REPORT_ID_KEYBOARD  1
#define REPORT_ID_MOUSE     2
#define REPORT_ID_CONSUMER  3

#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RGUI    0x80

typedef struct {
    uint8_t mod;
    uint8_t code;
} hid_key_t;

static const hid_key_t s_ascii_to_hid[128] = {
    ['\n'] = { 0, 0x28 },
    ['\r'] = { 0, 0x28 },
    ['\t'] = { 0, 0x2B },
    ['\b'] = { 0, 0x2A },
    ['\e'] = { 0, 0x29 },
    [' ']  = { 0, 0x2C },
    ['!']  = { MOD_LSHIFT, 0x1E },
    ['\"'] = { MOD_LSHIFT, 0x34 },
    ['#']  = { MOD_LSHIFT, 0x20 },
    ['$']  = { MOD_LSHIFT, 0x21 },
    ['%']  = { MOD_LSHIFT, 0x22 },
    ['&']  = { MOD_LSHIFT, 0x24 },
    ['\''] = { 0, 0x34 },
    ['(']  = { MOD_LSHIFT, 0x26 },
    [')']  = { MOD_LSHIFT, 0x27 },
    ['*']  = { MOD_LSHIFT, 0x25 },
    ['+']  = { MOD_LSHIFT, 0x2E },
    [',']  = { 0, 0x36 },
    ['-']  = { 0, 0x2D },
    ['.']  = { 0, 0x37 },
    ['/']  = { 0, 0x38 },
    ['0']  = { 0, 0x27 },
    ['1']  = { 0, 0x1E },
    ['2']  = { 0, 0x1F },
    ['3']  = { 0, 0x20 },
    ['4']  = { 0, 0x21 },
    ['5']  = { 0, 0x22 },
    ['6']  = { 0, 0x23 },
    ['7']  = { 0, 0x24 },
    ['8']  = { 0, 0x25 },
    ['9']  = { 0, 0x26 },
    [':']  = { MOD_LSHIFT, 0x33 },
    [';']  = { 0, 0x33 },
    ['<']  = { MOD_LSHIFT, 0x36 },
    ['=']  = { 0, 0x2E },
    ['>']  = { MOD_LSHIFT, 0x37 },
    ['?']  = { MOD_LSHIFT, 0x38 },
    ['@']  = { MOD_LSHIFT, 0x1F },
    ['A']  = { MOD_LSHIFT, 0x04 },
    ['B']  = { MOD_LSHIFT, 0x05 },
    ['C']  = { MOD_LSHIFT, 0x06 },
    ['D']  = { MOD_LSHIFT, 0x07 },
    ['E']  = { MOD_LSHIFT, 0x08 },
    ['F']  = { MOD_LSHIFT, 0x09 },
    ['G']  = { MOD_LSHIFT, 0x0A },
    ['H']  = { MOD_LSHIFT, 0x0B },
    ['I']  = { MOD_LSHIFT, 0x0C },
    ['J']  = { MOD_LSHIFT, 0x0D },
    ['K']  = { MOD_LSHIFT, 0x0E },
    ['L']  = { MOD_LSHIFT, 0x0F },
    ['M']  = { MOD_LSHIFT, 0x10 },
    ['N']  = { MOD_LSHIFT, 0x11 },
    ['O']  = { MOD_LSHIFT, 0x12 },
    ['P']  = { MOD_LSHIFT, 0x13 },
    ['Q']  = { MOD_LSHIFT, 0x14 },
    ['R']  = { MOD_LSHIFT, 0x15 },
    ['S']  = { MOD_LSHIFT, 0x16 },
    ['T']  = { MOD_LSHIFT, 0x17 },
    ['U']  = { MOD_LSHIFT, 0x18 },
    ['V']  = { MOD_LSHIFT, 0x19 },
    ['W']  = { MOD_LSHIFT, 0x1A },
    ['X']  = { MOD_LSHIFT, 0x1B },
    ['Y']  = { MOD_LSHIFT, 0x1C },
    ['Z']  = { MOD_LSHIFT, 0x1D },
    ['[']  = { 0, 0x2F },
    ['\\'] = { 0, 0x31 },
    [']']  = { 0, 0x30 },
    ['^']  = { MOD_LSHIFT, 0x23 },
    ['_']  = { MOD_LSHIFT, 0x2D },
    ['`']  = { 0, 0x35 },
    ['a']  = { 0, 0x04 },
    ['b']  = { 0, 0x05 },
    ['c']  = { 0, 0x06 },
    ['d']  = { 0, 0x07 },
    ['e']  = { 0, 0x08 },
    ['f']  = { 0, 0x09 },
    ['g']  = { 0, 0x0A },
    ['h']  = { 0, 0x0B },
    ['i']  = { 0, 0x0C },
    ['j']  = { 0, 0x0D },
    ['k']  = { 0, 0x0E },
    ['l']  = { 0, 0x0F },
    ['m']  = { 0, 0x10 },
    ['n']  = { 0, 0x11 },
    ['o']  = { 0, 0x12 },
    ['p']  = { 0, 0x13 },
    ['q']  = { 0, 0x14 },
    ['r']  = { 0, 0x15 },
    ['s']  = { 0, 0x16 },
    ['t']  = { 0, 0x17 },
    ['u']  = { 0, 0x18 },
    ['v']  = { 0, 0x19 },
    ['w']  = { 0, 0x1A },
    ['x']  = { 0, 0x1B },
    ['y']  = { 0, 0x1C },
    ['z']  = { 0, 0x1D },
    ['{']  = { MOD_LSHIFT, 0x2F },
    ['|']  = { MOD_LSHIFT, 0x31 },
    ['}']  = { MOD_LSHIFT, 0x30 },
    ['~']  = { MOD_LSHIFT, 0x35 },
};

typedef struct {
    const char *name;
    uint8_t code;
} key_name_entry_t;

static const key_name_entry_t s_named_keys[] = {
    {"ENTER", 0x28}, {"RETURN", 0x28},
    {"ESC", 0x29}, {"ESCAPE", 0x29},
    {"BACKSPACE", 0x2A}, {"BS", 0x2A},
    {"TAB", 0x2B},
    {"SPACE", 0x2C},
    {"MINUS", 0x2D},
    {"EQUAL", 0x2E},
    {"CAPSLOCK", 0x39}, {"CAPS_LOCK", 0x39},
    {"F1", 0x3A}, {"F2", 0x3B}, {"F3", 0x3C}, {"F4", 0x3D},
    {"F5", 0x3E}, {"F6", 0x3F}, {"F7", 0x40}, {"F8", 0x41},
    {"F9", 0x42}, {"F10", 0x43}, {"F11", 0x44}, {"F12", 0x45},
    {"PRINTSCREEN", 0x46}, {"PRINT_SCREEN", 0x46},
    {"SCROLLLOCK", 0x47}, {"SCROLL_LOCK", 0x47},
    {"PAUSE", 0x48},
    {"INSERT", 0x49},
    {"HOME", 0x4A},
    {"PAGEUP", 0x4B}, {"PAGE_UP", 0x4B},
    {"DELETE", 0x4C}, {"DEL", 0x4C},
    {"END", 0x4D},
    {"PAGEDOWN", 0x4E}, {"PAGE_DOWN", 0x4E},
    {"RIGHT", 0x4F}, {"RIGHT_ARROW", 0x4F},
    {"LEFT", 0x50}, {"LEFT_ARROW", 0x50},
    {"DOWN", 0x51}, {"DOWN_ARROW", 0x51},
    {"UP", 0x52}, {"UP_ARROW", 0x52},
    {"NUMLOCK", 0x53}, {"NUM_LOCK", 0x53},
};

static uint8_t lookup_key_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return 0;
    }
    if (name[1] == '\0') {
        uint8_t c = (uint8_t)name[0];
        if (c < 128 && s_ascii_to_hid[c].code != 0) {
            return s_ascii_to_hid[c].code;
        }
    }
    for (size_t i = 0; i < sizeof(s_named_keys) / sizeof(s_named_keys[0]); i++) {
        if (strcasecmp(name, s_named_keys[i].name) == 0) {
            return s_named_keys[i].code;
        }
    }
    return 0;
}

static const uint8_t s_hid_report_map[] = {
    // ----------------------------------------------------
    // Keyboard (Report ID 1)
    // ----------------------------------------------------
    0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
    0x09, 0x06,                    // USAGE (Keyboard)
    0xA1, 0x01,                    // COLLECTION (Application)
    0x85, 0x01,                    //   REPORT_ID (1)
    0x05, 0x07,                    //   USAGE_PAGE (Keyboard/Keypad)
    0x19, 0xE0,                    //   USAGE_MINIMUM (Keyboard LeftControl)
    0x29, 0xE7,                    //   USAGE_MAXIMUM (Keyboard Right GUI)
    0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
    0x25, 0x01,                    //   LOGICAL_MAXIMUM (1)
    0x75, 0x01,                    //   REPORT_SIZE (1)
    0x95, 0x08,                    //   REPORT_COUNT (8)
    0x81, 0x02,                    //   INPUT (Data,Var,Abs)
    0x95, 0x01,                    //   REPORT_COUNT (1)
    0x75, 0x08,                    //   REPORT_SIZE (8)
    0x81, 0x01,                    //   INPUT (Cnst,Ary,Abs)
    0x95, 0x05,                    //   REPORT_COUNT (5)
    0x75, 0x01,                    //   REPORT_SIZE (1)
    0x05, 0x08,                    //   USAGE_PAGE (LEDs)
    0x19, 0x01,                    //   USAGE_MINIMUM (Num Lock)
    0x29, 0x05,                    //   USAGE_MAXIMUM (Kana)
    0x91, 0x02,                    //   OUTPUT (Data,Var,Abs)
    0x95, 0x01,                    //   REPORT_COUNT (1)
    0x75, 0x03,                    //   REPORT_SIZE (3)
    0x91, 0x01,                    //   OUTPUT (Cnst,Ary,Abs)
    0x95, 0x06,                    //   REPORT_COUNT (6)
    0x75, 0x08,                    //   REPORT_SIZE (8)
    0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
    0x25, 0x65,                    //   LOGICAL_MAXIMUM (101)
    0x05, 0x07,                    //   USAGE_PAGE (Keyboard/Keypad)
    0x19, 0x00,                    //   USAGE_MINIMUM (Reserved)
    0x29, 0x65,                    //   USAGE_MAXIMUM (Keyboard Application)
    0x81, 0x00,                    //   INPUT (Data,Ary,Abs)
    0xC0,                          // END_COLLECTION

    // ----------------------------------------------------
    // Mouse (Report ID 2)
    // ----------------------------------------------------
    0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
    0x09, 0x02,                    // USAGE (Mouse)
    0xA1, 0x01,                    // COLLECTION (Application)
    0x85, 0x02,                    //   REPORT_ID (2)
    0x09, 0x01,                    //   USAGE (Pointer)
    0xA1, 0x00,                    //   COLLECTION (Physical)
    0x05, 0x09,                    //     USAGE_PAGE (Button)
    0x19, 0x01,                    //     USAGE_MINIMUM (Button 1)
    0x29, 0x03,                    //     USAGE_MAXIMUM (Button 3)
    0x15, 0x00,                    //     LOGICAL_MINIMUM (0)
    0x25, 0x01,                    //     LOGICAL_MAXIMUM (1)
    0x75, 0x01,                    //     REPORT_SIZE (1)
    0x95, 0x03,                    //     REPORT_COUNT (3)
    0x81, 0x02,                    //     INPUT (Data,Var,Abs)
    0x75, 0x05,                    //     REPORT_SIZE (5)
    0x95, 0x01,                    //     REPORT_COUNT (1)
    0x81, 0x01,                    //     INPUT (Cnst,Ary,Abs)
    0x05, 0x01,                    //     USAGE_PAGE (Generic Desktop)
    0x09, 0x30,                    //     USAGE (X)
    0x09, 0x31,                    //     USAGE (Y)
    0x09, 0x38,                    //     USAGE (Wheel)
    0x15, 0x81,                    //     LOGICAL_MINIMUM (-127)
    0x25, 0x7F,                    //     LOGICAL_MAXIMUM (127)
    0x75, 0x08,                    //     REPORT_SIZE (8)
    0x95, 0x03,                    //     REPORT_COUNT (3)
    0x81, 0x06,                    //     INPUT (Data,Var,Rel)
    0xC0,                          //   END_COLLECTION
    0xC0,                          // END_COLLECTION

    // ----------------------------------------------------
    // Consumer Control (Media) (Report ID 3)
    // ----------------------------------------------------
    0x05, 0x0C,                    // USAGE_PAGE (Consumer Devices)
    0x09, 0x01,                    // USAGE (Consumer Control)
    0xA1, 0x01,                    // COLLECTION (Application)
    0x85, 0x03,                    //   REPORT_ID (3)
    0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
    0x26, 0xFF, 0x03,              //   LOGICAL_MAXIMUM (0x03FF)
    0x19, 0x00,                    //   USAGE_MINIMUM (0)
    0x2A, 0xFF, 0x03,              //   USAGE_MAXIMUM (0x03FF)
    0x75, 0x10,                    //   REPORT_SIZE (16)
    0x95, 0x01,                    //   REPORT_COUNT (1)
    0x81, 0x00,                    //   INPUT (Data,Ary,Abs)
    0xC0                           // END_COLLECTION
};

static bool s_inited = false;
static bool s_started = false;
static bool s_connected = false;
static esp_hidd_dev_t *s_hid_dev = NULL;
static char s_device_name[32] = "nano-esp-bot";
static SemaphoreHandle_t s_ble_hid_mutex = NULL;

static struct ble_hs_adv_fields s_adv_fields;
static ble_uuid16_t s_uuid16_hid;

static esp_hid_raw_report_map_t s_raw_report_map = {
    .data = s_hid_report_map,
    .len = sizeof(s_hid_report_map),
};

static esp_err_t ble_hid_start_advertising(void);

static void ble_hid_setup_adv_fields(const char *device_name)
{
    memset(&s_adv_fields, 0, sizeof(s_adv_fields));
    s_adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    s_adv_fields.appearance = ESP_HID_APPEARANCE_KEYBOARD;
    s_adv_fields.appearance_is_present = 1;
    s_adv_fields.tx_pwr_lvl_is_present = 1;
    s_adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    s_adv_fields.name = (const uint8_t *)device_name;
    s_adv_fields.name_len = strlen(device_name);
    s_adv_fields.name_is_complete = 1;

    s_uuid16_hid = (ble_uuid16_t)BLE_UUID16_INIT(0x1812);
    s_adv_fields.uuids16 = &s_uuid16_hid;
    s_adv_fields.num_uuids16 = 1;
    s_adv_fields.uuids16_is_complete = 1;
}

static int ble_hid_gap_event(struct ble_gap_event *event, void *arg)
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
        } else if (s_started) {
            ble_hid_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason=%d)", event->disconnect.reason);
        s_connected = false;
        if (s_started) {
            ble_hid_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGD(TAG, "BLE advertise complete (reason=%d)", event->adv_complete.reason);
        if (s_started && !s_connected) {
            ble_hid_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGD(TAG, "BLE passkey action");
        struct ble_sm_io pkey = {0};
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        break;

    default:
        break;
    }
    return 0;
}

static esp_err_t ble_hid_start_advertising(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_connected) {
        return ESP_OK;
    }

    int rc = ble_gap_adv_set_fields(&s_adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting advertisement data: %d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, 180000, &adv_params, ble_hid_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Error starting advertisement: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    if (event == ESP_HIDD_START_EVENT) {
        ESP_LOGI(TAG, "ESP_HIDD_START_EVENT received");
        if (s_started) {
            ble_hid_start_advertising();
        }
    } else if (event == ESP_HIDD_CONNECT_EVENT) {
        ESP_LOGI(TAG, "ESP_HIDD_CONNECT_EVENT received");
        s_connected = true;
    } else if (event == ESP_HIDD_DISCONNECT_EVENT) {
        ESP_LOGI(TAG, "ESP_HIDD_DISCONNECT_EVENT received");
        s_connected = false;
        if (s_started) {
            ble_hid_start_advertising();
        }
    } else if (event == ESP_HIDD_STOP_EVENT) {
        ESP_LOGI(TAG, "ESP_HIDD_STOP_EVENT received");
        s_connected = false;
    }
}

esp_err_t chal_ble_hid_init(const char *device_name)
{
    if (!s_ble_hid_mutex) {
        s_ble_hid_mutex = xSemaphoreCreateMutex();
        if (!s_ble_hid_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_ble_hid_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_inited) {
        xSemaphoreGive(s_ble_hid_mutex);
        return ESP_OK;
    }

    if (device_name && strlen(device_name) > 0) {
        strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);
        s_device_name[sizeof(s_device_name) - 1] = '\0';
    }

    esp_err_t ret = chal_nimble_ensure_inited();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_ble_hid_mutex);
        return ret;
    }

    ble_hid_setup_adv_fields(s_device_name);
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    esp_hid_device_config_t hid_cfg = {
        .vendor_id = 0x02e5,
        .product_id = 0xabcd,
        .version = 0x0100,
        .device_name = s_device_name,
        .manufacturer_name = "nano-esp-bot",
        .serial_number = "140001",
        .report_maps = &s_raw_report_map,
        .report_maps_len = 1,
    };

    ret = esp_hidd_dev_init(&hid_cfg, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %s", esp_err_to_name(ret));
        chal_nimble_deinit();
        xSemaphoreGive(s_ble_hid_mutex);
        return ret;
    }

    ret = chal_nimble_start_host();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "chal_nimble_start_host failed: %s", esp_err_to_name(ret));
        esp_hidd_dev_deinit(s_hid_dev);
        s_hid_dev = NULL;
        chal_nimble_deinit();
        xSemaphoreGive(s_ble_hid_mutex);
        return ret;
    }

    s_inited = true;
    xSemaphoreGive(s_ble_hid_mutex);
    return ESP_OK;
}

esp_err_t chal_ble_hid_start(void)
{
    if (s_ble_hid_mutex && xSemaphoreTake(s_ble_hid_mutex, portMAX_DELAY) == pdTRUE) {
        if (!s_inited) {
            xSemaphoreGive(s_ble_hid_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        s_started = true;
        esp_err_t ret = ble_hid_start_advertising();
        xSemaphoreGive(s_ble_hid_mutex);
        return ret;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t chal_ble_hid_stop(void)
{
    if (s_ble_hid_mutex && xSemaphoreTake(s_ble_hid_mutex, portMAX_DELAY) == pdTRUE) {
        if (!s_inited) {
            xSemaphoreGive(s_ble_hid_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        s_started = false;
        ble_gap_adv_stop();
        xSemaphoreGive(s_ble_hid_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t chal_ble_hid_deinit(void)
{
    if (!s_inited) {
        return ESP_OK;
    }

    if (s_ble_hid_mutex && xSemaphoreTake(s_ble_hid_mutex, portMAX_DELAY) == pdTRUE) {
        chal_ble_hid_release_all();
        s_started = false;
        s_connected = false;
        ble_gap_adv_stop();
        if (s_hid_dev) {
            esp_hidd_dev_deinit(s_hid_dev);
            s_hid_dev = NULL;
        }
        chal_nimble_deinit();
        s_inited = false;
        xSemaphoreGive(s_ble_hid_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool chal_ble_hid_is_initialized(void)
{
    return s_inited;
}

bool chal_ble_hid_is_started(void)
{
    return s_started;
}

bool chal_ble_hid_is_connected(void)
{
    return s_connected;
}

esp_err_t chal_ble_hid_send_keyboard(uint8_t modifiers, const uint8_t keycodes[6])
{
    if (!s_inited || !s_connected || !s_hid_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[8] = {0};
    buf[0] = modifiers;
    buf[1] = 0;
    if (keycodes) {
        memcpy(&buf[2], keycodes, 6);
    }
    return esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_KEYBOARD, buf, sizeof(buf));
}

esp_err_t chal_ble_hid_send_text(const char *text)
{
    if (!s_inited || !s_connected || !s_hid_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    while (*text) {
        uint8_t c = (uint8_t)*text++;
        if (c >= 128) {
            continue;
        }
        hid_key_t k = s_ascii_to_hid[c];
        if (k.code == 0) {
            continue;
        }

        uint8_t keys[6] = { k.code, 0, 0, 0, 0, 0 };
        chal_ble_hid_send_keyboard(k.mod, keys);
        vTaskDelay(pdMS_TO_TICKS(12));
        chal_ble_hid_send_keyboard(0, NULL);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
    return ESP_OK;
}

esp_err_t chal_ble_hid_send_key(const char *name, bool press, bool release)
{
    if (!s_inited || !s_connected || !s_hid_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t code = lookup_key_name(name);
    if (code == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (press) {
        uint8_t keys[6] = { code, 0, 0, 0, 0, 0 };
        chal_ble_hid_send_keyboard(0, keys);
    }
    if (press && release) {
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    if (release) {
        chal_ble_hid_send_keyboard(0, NULL);
    }
    return ESP_OK;
}

esp_err_t chal_ble_hid_send_combo(uint8_t modifiers, const char *key_name)
{
    if (!s_inited || !s_connected || !s_hid_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t code = lookup_key_name(key_name);
    if (code == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t keys[6] = { code, 0, 0, 0, 0, 0 };
    chal_ble_hid_send_keyboard(modifiers, keys);
    vTaskDelay(pdMS_TO_TICKS(15));
    chal_ble_hid_send_keyboard(0, NULL);
    return ESP_OK;
}

esp_err_t chal_ble_hid_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!s_inited || !s_connected || !s_hid_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[4] = { buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
    return esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_MOUSE, buf, sizeof(buf));
}

esp_err_t chal_ble_hid_send_consumer(uint16_t usage)
{
    if (!s_inited || !s_connected || !s_hid_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { (uint8_t)(usage & 0xFF), (uint8_t)((usage >> 8) & 0xFF) };
    return esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_CONSUMER, buf, sizeof(buf));
}

esp_err_t chal_ble_hid_release_all(void)
{
    if (!s_inited || !s_connected || !s_hid_dev) {
        return ESP_OK;
    }
    uint8_t zero_kbd[8] = {0};
    uint8_t zero_mouse[4] = {0};
    uint8_t zero_consumer[2] = {0};

    esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_KEYBOARD, zero_kbd, sizeof(zero_kbd));
    esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_MOUSE, zero_mouse, sizeof(zero_mouse));
    esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_CONSUMER, zero_consumer, sizeof(zero_consumer));
    return ESP_OK;
}

void chal_ble_hid_teardown(void)
{
    if (s_inited && s_connected) {
        chal_ble_hid_release_all();
    }
}

static uint8_t parse_modifier_str(const char *name)
{
    if (!name) return 0;
    if (strcasecmp(name, "CTRL") == 0 || strcasecmp(name, "CONTROL") == 0 || strcasecmp(name, "LCTRL") == 0) return MOD_LCTRL;
    if (strcasecmp(name, "SHIFT") == 0 || strcasecmp(name, "LSHIFT") == 0) return MOD_LSHIFT;
    if (strcasecmp(name, "ALT") == 0 || strcasecmp(name, "LALT") == 0) return MOD_LALT;
    if (strcasecmp(name, "GUI") == 0 || strcasecmp(name, "WIN") == 0 || strcasecmp(name, "CMD") == 0 || strcasecmp(name, "META") == 0 || strcasecmp(name, "LGUI") == 0) return MOD_LGUI;
    if (strcasecmp(name, "RCTRL") == 0) return MOD_RCTRL;
    if (strcasecmp(name, "RSHIFT") == 0) return MOD_RSHIFT;
    if (strcasecmp(name, "RALT") == 0) return MOD_RALT;
    if (strcasecmp(name, "RGUI") == 0) return MOD_RGUI;
    return 0;
}

static uint8_t parse_mouse_button(const char *btn)
{
    if (!btn) return 0;
    if (strcasecmp(btn, "left") == 0 || strcasecmp(btn, "l") == 0) return 0x01;
    if (strcasecmp(btn, "right") == 0 || strcasecmp(btn, "r") == 0) return 0x02;
    if (strcasecmp(btn, "middle") == 0 || strcasecmp(btn, "m") == 0) return 0x04;
    return 0;
}

static uint16_t lookup_media_usage(const char *act)
{
    if (!act) return 0;
    if (strcasecmp(act, "volume_up") == 0 || strcasecmp(act, "vol_up") == 0) return 0x00E9;
    if (strcasecmp(act, "volume_down") == 0 || strcasecmp(act, "vol_down") == 0) return 0x00EA;
    if (strcasecmp(act, "mute") == 0) return 0x00E2;
    if (strcasecmp(act, "play_pause") == 0 || strcasecmp(act, "playpause") == 0) return 0x00CD;
    if (strcasecmp(act, "play") == 0) return 0x00B0;
    if (strcasecmp(act, "pause") == 0) return 0x00B1;
    if (strcasecmp(act, "stop") == 0) return 0x00B7;
    if (strcasecmp(act, "next") == 0 || strcasecmp(act, "next_track") == 0) return 0x00B5;
    if (strcasecmp(act, "prev") == 0 || strcasecmp(act, "prev_track") == 0) return 0x00B6;
    if (strcasecmp(act, "power") == 0) return 0x0030;
    return 0;
}

static int lua_ble_hid_init(lua_State *L)
{
    const char *name = luaL_optstring(L, 1, "nano-esp-bot");
    esp_err_t err = chal_ble_hid_init(name);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_start(lua_State *L)
{
    esp_err_t err = chal_ble_hid_start();
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_stop(lua_State *L)
{
    esp_err_t err = chal_ble_hid_stop();
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_deinit(lua_State *L)
{
    esp_err_t err = chal_ble_hid_deinit();
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_status(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, chal_ble_hid_is_initialized());
    lua_setfield(L, -2, "initialized");
    lua_pushboolean(L, chal_ble_hid_is_started());
    lua_setfield(L, -2, "started");
    lua_pushboolean(L, chal_ble_hid_is_connected());
    lua_setfield(L, -2, "connected");
    return 1;
}

static int lua_ble_hid_text(lua_State *L)
{
    const char *str = luaL_checkstring(L, 1);
    esp_err_t err = chal_ble_hid_send_text(str);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_key(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *gesture = luaL_optstring(L, 2, "click");
    bool press = false;
    bool release = false;

    if (strcasecmp(gesture, "click") == 0) {
        press = true;
        release = true;
    } else if (strcasecmp(gesture, "press") == 0) {
        press = true;
    } else if (strcasecmp(gesture, "release") == 0) {
        release = true;
    } else {
        return luaL_error(L, "invalid gesture: %s (expected 'click', 'press', or 'release')", gesture);
    }

    esp_err_t err = chal_ble_hid_send_key(name, press, release);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_combo(lua_State *L)
{
    uint8_t mod_mask = 0;
    const char *key_name = NULL;

    if (lua_istable(L, 1)) {
        lua_Integer len = luaL_len(L, 1);
        for (lua_Integer i = 1; i <= len; i++) {
            lua_rawgeti(L, 1, i);
            const char *m = lua_tostring(L, -1);
            mod_mask |= parse_modifier_str(m);
            lua_pop(L, 1);
        }
        key_name = luaL_checkstring(L, 2);
    } else {
        int top = lua_gettop(L);
        if (top < 1) {
            return luaL_error(L, "ble_hid.combo requires at least 1 key argument");
        }
        key_name = luaL_checkstring(L, top);
        for (int i = 1; i < top; i++) {
            const char *m = luaL_checkstring(L, i);
            mod_mask |= parse_modifier_str(m);
        }
    }

    esp_err_t err = chal_ble_hid_send_combo(mod_mask, key_name);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_mouse_move(lua_State *L)
{
    lua_Integer dx = luaL_checkinteger(L, 1);
    lua_Integer dy = luaL_checkinteger(L, 2);

    if (dx < -127) dx = -127;
    if (dx > 127) dx = 127;
    if (dy < -127) dy = -127;
    if (dy > 127) dy = 127;

    esp_err_t err = chal_ble_hid_send_mouse(0, (int8_t)dx, (int8_t)dy, 0);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_mouse_button(lua_State *L)
{
    const char *btn_name = luaL_checkstring(L, 1);
    const char *gesture = luaL_optstring(L, 2, "click");
    uint8_t btn_mask = parse_mouse_button(btn_name);

    if (btn_mask == 0) {
        return luaL_error(L, "invalid mouse button: %s (expected 'left', 'right', or 'middle')", btn_name);
    }

    esp_err_t err = ESP_OK;
    if (strcasecmp(gesture, "click") == 0) {
        err = chal_ble_hid_send_mouse(btn_mask, 0, 0, 0);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(15));
            err = chal_ble_hid_send_mouse(0, 0, 0, 0);
        }
    } else if (strcasecmp(gesture, "press") == 0) {
        err = chal_ble_hid_send_mouse(btn_mask, 0, 0, 0);
    } else if (strcasecmp(gesture, "release") == 0) {
        err = chal_ble_hid_send_mouse(0, 0, 0, 0);
    } else if (strcasecmp(gesture, "double_click") == 0) {
        err = chal_ble_hid_send_mouse(btn_mask, 0, 0, 0);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(15));
            err = chal_ble_hid_send_mouse(0, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(60));
            err = chal_ble_hid_send_mouse(btn_mask, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(15));
            err = chal_ble_hid_send_mouse(0, 0, 0, 0);
        }
    } else {
        return luaL_error(L, "invalid mouse gesture: %s (expected 'click', 'press', 'release', or 'double_click')", gesture);
    }

    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_mouse_scroll(lua_State *L)
{
    lua_Integer wheel = luaL_checkinteger(L, 1);
    if (wheel < -127) wheel = -127;
    if (wheel > 127) wheel = 127;

    esp_err_t err = chal_ble_hid_send_mouse(0, 0, 0, (int8_t)wheel);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_media(lua_State *L)
{
    const char *action = luaL_checkstring(L, 1);
    const char *gesture = luaL_optstring(L, 2, "click");
    uint16_t usage = lookup_media_usage(action);

    if (usage == 0) {
        return luaL_error(L, "invalid media action: %s", action);
    }

    esp_err_t err = ESP_OK;
    if (strcasecmp(gesture, "click") == 0) {
        err = chal_ble_hid_send_consumer(usage);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(15));
            err = chal_ble_hid_send_consumer(0);
        }
    } else if (strcasecmp(gesture, "press") == 0) {
        err = chal_ble_hid_send_consumer(usage);
    } else if (strcasecmp(gesture, "release") == 0) {
        err = chal_ble_hid_send_consumer(0);
    } else {
        return luaL_error(L, "invalid media gesture: %s (expected 'click', 'press', or 'release')", gesture);
    }

    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_release_all(lua_State *L)
{
    chal_ble_hid_release_all();
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg s_ble_hid_funcs[] = {
    {"init", lua_ble_hid_init},
    {"start", lua_ble_hid_start},
    {"stop", lua_ble_hid_stop},
    {"deinit", lua_ble_hid_deinit},
    {"status", lua_ble_hid_status},
    {"text", lua_ble_hid_text},
    {"key", lua_ble_hid_key},
    {"combo", lua_ble_hid_combo},
    {"mouse_move", lua_ble_hid_mouse_move},
    {"mouse_button", lua_ble_hid_mouse_button},
    {"mouse_scroll", lua_ble_hid_mouse_scroll},
    {"media", lua_ble_hid_media},
    {"release_all", lua_ble_hid_release_all},
    {NULL, NULL}
};

int luaopen_ble_hid(lua_State *L)
{
    luaL_newlib(L, s_ble_hid_funcs);
    return 1;
}
