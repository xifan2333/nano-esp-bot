#include "chal_wifi.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "lauxlib.h"

static const char *TAG = "chal_wifi";

#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1
#define WIFI_MAX_SCAN_APS     32

static bool s_nvs_inited = false;
static bool s_wifi_inited = false;
static bool s_wifi_started = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static SemaphoreHandle_t s_wifi_mutex = NULL;

static bool s_sta_connected = false;
static bool s_ap_active = false;
static bool s_napt_enabled = false;
static char s_sta_ip[16] = {0};
static char s_sta_ssid[33] = {0};
static char s_ap_ssid[33] = {0};

static const char *wifi_authmode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "wep";
        case WIFI_AUTH_WPA_PSK: return "wpa_psk";
        case WIFI_AUTH_WPA2_PSK: return "wpa2_psk";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa_wpa2_psk";
        case WIFI_AUTH_WPA3_PSK: return "wpa3_psk";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2_wpa3_psk";
        case WIFI_AUTH_WAPI_PSK: return "wapi_psk";
        case WIFI_AUTH_OWE: return "owe";
        default: return "unknown";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGD(TAG, "STA start event");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "STA connected to AP");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "STA disconnected");
                s_sta_connected = false;
                s_sta_ip[0] = '\0';
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                break;
            case WIFI_EVENT_AP_START:
                s_ap_active = true;
                ESP_LOGI(TAG, "SoftAP started: SSID %s", s_ap_ssid);
                break;
            case WIFI_EVENT_AP_STOP:
                s_ap_active = false;
                s_napt_enabled = false;
                ESP_LOGI(TAG, "SoftAP stopped");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
            s_sta_connected = true;
            ESP_LOGI(TAG, "STA got IP: %s", s_sta_ip);
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        }
    }
}

static esp_err_t ensure_wifi_init(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }

    if (!s_nvs_inited) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            esp_err_t erase_err = nvs_flash_erase();
            if (erase_err != ESP_OK) return erase_err;
            ret = nvs_flash_init();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS flash init failed (%s)", esp_err_to_name(ret));
            return ret;
        }
        s_nvs_inited = true;
    }

    if (!s_wifi_mutex) {
        s_wifi_mutex = xSemaphoreCreateMutex();
        if (!s_wifi_mutex) return ESP_ERR_NO_MEM;
    }

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed (%s)", esp_err_to_name(err));
        return err;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    s_wifi_inited = true;
    return ESP_OK;
}

static int l_wifi_scan(lua_State *L)
{
    esp_err_t err = ensure_wifi_init();
    if (err != ESP_OK) {
        return luaL_error(L, "wifi init failed (%s)", esp_err_to_name(err));
    }

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);

    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);
    if (!s_wifi_started) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        s_wifi_started = true;
    } else if (current_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_wifi_mutex);
        return luaL_error(L, "wifi scan failed (%s)", esp_err_to_name(err));
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > WIFI_MAX_SCAN_APS) {
        ap_count = WIFI_MAX_SCAN_APS;
    }

    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_count > 0 && !ap_records) {
        xSemaphoreGive(s_wifi_mutex);
        return luaL_error(L, "out of memory for scan records");
    }

    if (ap_count > 0) {
        esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    }

    lua_createtable(L, ap_count, 0);
    for (uint16_t i = 0; i < ap_count; i++) {
        lua_createtable(L, 0, 5);
        lua_pushstring(L, (const char *)ap_records[i].ssid);
        lua_setfield(L, -2, "ssid");

        lua_pushinteger(L, ap_records[i].rssi);
        lua_setfield(L, -2, "rssi");

        lua_pushstring(L, wifi_authmode_str(ap_records[i].authmode));
        lua_setfield(L, -2, "authmode");

        lua_pushinteger(L, ap_records[i].primary);
        lua_setfield(L, -2, "channel");

        char bssid_str[18];
        snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap_records[i].bssid[0], ap_records[i].bssid[1], ap_records[i].bssid[2],
                 ap_records[i].bssid[3], ap_records[i].bssid[4], ap_records[i].bssid[5]);
        lua_pushstring(L, bssid_str);
        lua_setfield(L, -2, "bssid");

        lua_rawseti(L, -2, i + 1);
    }

    if (ap_records) {
        free(ap_records);
    }

    xSemaphoreGive(s_wifi_mutex);
    return 1;
}

static int l_wifi_connect(lua_State *L)
{
    const char *ssid = luaL_checkstring(L, 1);
    const char *password = luaL_optstring(L, 2, NULL);

    esp_err_t err = ensure_wifi_init();
    if (err != ESP_OK) {
        return luaL_error(L, "wifi init failed (%s)", esp_err_to_name(err));
    }

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);

    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);
    if (s_ap_active) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password && strlen(password) > 0) {
        strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    }

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    if (!s_wifi_started) {
        esp_wifi_start();
        s_wifi_started = true;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        xSemaphoreGive(s_wifi_mutex);
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    xSemaphoreGive(s_wifi_mutex);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        lua_pushboolean(L, 1);
        lua_pushstring(L, s_sta_ip);
        return 2;
    }

    lua_pushnil(L);
    lua_pushstring(L, (bits & WIFI_FAIL_BIT) ? "connection_failed" : "timeout");
    return 2;
}

static int l_wifi_disconnect(lua_State *L)
{
    if (!s_wifi_started) {
        lua_pushboolean(L, 1);
        return 1;
    }

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    esp_err_t err = esp_wifi_disconnect();
    s_sta_connected = false;
    s_sta_ip[0] = '\0';
    xSemaphoreGive(s_wifi_mutex);

    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

static int l_wifi_status(lua_State *L)
{
    lua_createtable(L, 0, 7);

    lua_pushboolean(L, s_sta_connected);
    lua_setfield(L, -2, "sta_connected");

    if (s_sta_connected && s_sta_ip[0]) {
        lua_pushstring(L, s_sta_ip);
        lua_setfield(L, -2, "sta_ip");
        lua_pushstring(L, s_sta_ssid);
        lua_setfield(L, -2, "sta_ssid");
    }

    lua_pushboolean(L, s_ap_active);
    lua_setfield(L, -2, "ap_active");

    if (s_ap_active) {
        lua_pushstring(L, "192.168.4.1");
        lua_setfield(L, -2, "ap_ip");
        lua_pushstring(L, s_ap_ssid);
        lua_setfield(L, -2, "ap_ssid");
    }

    lua_pushboolean(L, s_napt_enabled);
    lua_setfield(L, -2, "router_enabled");

    if (!s_wifi_started) {
        lua_pushstring(L, "off");
    } else if (s_sta_connected && s_ap_active) {
        lua_pushstring(L, "apsta");
    } else if (s_ap_active) {
        lua_pushstring(L, "ap");
    } else {
        lua_pushstring(L, "sta");
    }
    lua_setfield(L, -2, "mode");

    return 1;
}

static int l_wifi_ap(lua_State *L)
{
    const char *ssid = luaL_checkstring(L, 1);
    const char *password = luaL_optstring(L, 2, NULL);
    int channel = (int)luaL_optinteger(L, 3, 1);

    esp_err_t err = ensure_wifi_init();
    if (err != ESP_OK) {
        return luaL_error(L, "wifi init failed (%s)", esp_err_to_name(err));
    }

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);

    if (s_sta_connected) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ssid);
    ap_cfg.ap.channel = (uint8_t)(channel > 0 && channel <= 13 ? channel : 1);
    ap_cfg.ap.max_connection = 4;

    if (password && strlen(password) >= 8) {
        strncpy((char *)ap_cfg.ap.password, password, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    if (!s_wifi_started) {
        esp_wifi_start();
        s_wifi_started = true;
    }

    strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
    s_ap_active = true;

    xSemaphoreGive(s_wifi_mutex);

    lua_pushboolean(L, 1);
    lua_pushstring(L, "192.168.4.1");
    return 2;
}

static int l_wifi_router(lua_State *L)
{
    bool enable = lua_toboolean(L, 1);

    if (!s_wifi_inited || !s_wifi_started) {
        lua_pushnil(L);
        lua_pushstring(L, "wifi not started");
        return 2;
    }

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);

    if (enable) {
        if (!s_ap_active) {
            xSemaphoreGive(s_wifi_mutex);
            lua_pushnil(L);
            lua_pushstring(L, "SoftAP is not active (call wifi.ap first)");
            return 2;
        }

        if (!s_sta_connected) {
            xSemaphoreGive(s_wifi_mutex);
            lua_pushnil(L);
            lua_pushstring(L, "Station is not connected to upstream network (call wifi.connect first)");
            return 2;
        }

        esp_err_t err = esp_netif_napt_enable(s_ap_netif);
        if (err != ESP_OK) {
            xSemaphoreGive(s_wifi_mutex);
            lua_pushnil(L);
            lua_pushstring(L, esp_err_to_name(err));
            return 2;
        }

        s_napt_enabled = true;
        ESP_LOGI(TAG, "NAPT router forwarding enabled across AP and STA netifs");
        xSemaphoreGive(s_wifi_mutex);

        lua_pushboolean(L, 1);
        return 1;
    } else {
        if (s_napt_enabled && s_ap_netif) {
            esp_netif_napt_disable(s_ap_netif);
            s_napt_enabled = false;
            ESP_LOGI(TAG, "NAPT router forwarding disabled");
        }
        xSemaphoreGive(s_wifi_mutex);
        lua_pushboolean(L, 1);
        return 1;
    }
}

static int l_wifi_stop(lua_State *L)
{
    chal_wifi_teardown();
    lua_pushboolean(L, 1);
    return 1;
}

void chal_wifi_teardown(void)
{
    if (!s_wifi_started) {
        return;
    }

    if (s_wifi_mutex) {
        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    }

    if (s_napt_enabled && s_ap_netif) {
        esp_netif_napt_disable(s_ap_netif);
        s_napt_enabled = false;
    }

    esp_wifi_disconnect();
    esp_wifi_stop();

    s_wifi_started = false;
    s_sta_connected = false;
    s_ap_active = false;
    s_sta_ip[0] = '\0';

    if (s_wifi_mutex) {
        xSemaphoreGive(s_wifi_mutex);
    }

    ESP_LOGI(TAG, "Wi-Fi radio safely torn down");
}

static const luaL_Reg wifi_funcs[] = {
    {"scan", l_wifi_scan},
    {"connect", l_wifi_connect},
    {"disconnect", l_wifi_disconnect},
    {"status", l_wifi_status},
    {"ap", l_wifi_ap},
    {"router", l_wifi_router},
    {"stop", l_wifi_stop},
    {NULL, NULL}
};

int luaopen_wifi(lua_State *L)
{
    luaL_newlib(L, wifi_funcs);
    return 1;
}
