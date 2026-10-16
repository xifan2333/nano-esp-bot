#include "chal_espnow.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "lauxlib.h"

static const char *TAG = "chal_espnow";

#define ESPNOW_QUEUE_SIZE 16

typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    int8_t rssi;
    uint8_t len;
    uint8_t data[ESP_NOW_MAX_DATA_LEN];
} espnow_packet_t;

static SemaphoreHandle_t s_espnow_mutex = NULL;
static QueueHandle_t s_recv_queue = NULL;
static bool s_inited = false;
static bool s_wifi_started_by_us = false;

static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (!recv_info || !recv_info->src_addr || !data || len <= 0 || !s_recv_queue) {
        return;
    }

    espnow_packet_t pkt;
    memcpy(pkt.mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    pkt.rssi = recv_info->rx_ctrl ? recv_info->rx_ctrl->rssi : 0;
    pkt.len = (uint8_t)(len > ESP_NOW_MAX_DATA_LEN ? ESP_NOW_MAX_DATA_LEN : len);
    memcpy(pkt.data, data, pkt.len);

    xQueueSend(s_recv_queue, &pkt, 0);
}

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    (void)status;
}

static esp_err_t ensure_wifi_ready(void)
{
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_OK) {
        return esp_wifi_start();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if (err == ESP_OK) {
        s_wifi_started_by_us = true;
    }
    return err;
}

esp_err_t chal_espnow_init(void)
{
    if (!s_espnow_mutex) {
        s_espnow_mutex = xSemaphoreCreateMutex();
        if (!s_espnow_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_espnow_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_inited) {
        xSemaphoreGive(s_espnow_mutex);
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_espnow_mutex);
        return ret;
    }

    ret = ensure_wifi_ready();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ensure_wifi_ready failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_espnow_mutex);
        return ret;
    }

    if (!s_recv_queue) {
        s_recv_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_packet_t));
        if (!s_recv_queue) {
            xSemaphoreGive(s_espnow_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_espnow_mutex);
        return ret;
    }

    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);

    if (!esp_now_is_peer_exist(s_broadcast_mac)) {
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    s_inited = true;
    xSemaphoreGive(s_espnow_mutex);
    return ESP_OK;
}

esp_err_t chal_espnow_deinit(void)
{
    if (s_espnow_mutex && xSemaphoreTake(s_espnow_mutex, portMAX_DELAY) == pdTRUE) {
        if (!s_inited) {
            xSemaphoreGive(s_espnow_mutex);
            return ESP_OK;
        }

        esp_now_deinit();
        if (s_recv_queue) {
            vQueueDelete(s_recv_queue);
            s_recv_queue = NULL;
        }
        if (s_wifi_started_by_us) {
            esp_wifi_stop();
            s_wifi_started_by_us = false;
        }
        s_inited = false;
        xSemaphoreGive(s_espnow_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool chal_espnow_is_initialized(void)
{
    return s_inited;
}

esp_err_t chal_espnow_add_peer(const uint8_t mac[6], uint8_t channel, bool encrypt, const uint8_t lmk[16])
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!mac) return ESP_ERR_INVALID_ARG;

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = encrypt;
    if (encrypt && lmk) {
        memcpy(peer.lmk, lmk, ESP_NOW_KEY_LEN);
    }

    if (esp_now_is_peer_exist(mac)) {
        return esp_now_mod_peer(&peer);
    }
    return esp_now_add_peer(&peer);
}

esp_err_t chal_espnow_del_peer(const uint8_t mac[6])
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!mac) return ESP_ERR_INVALID_ARG;
    return esp_now_del_peer(mac);
}

bool chal_espnow_is_peer_exist(const uint8_t mac[6])
{
    if (!s_inited || !mac) return false;
    return esp_now_is_peer_exist(mac);
}

esp_err_t chal_espnow_send(const uint8_t *mac, const uint8_t *data, size_t len)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0 || len > ESP_NOW_MAX_DATA_LEN) return ESP_ERR_INVALID_ARG;

    const uint8_t *dest = mac ? mac : s_broadcast_mac;

    if (!esp_now_is_peer_exist(dest)) {
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, dest, ESP_NOW_ETH_ALEN);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_err_t err = esp_now_add_peer(&peer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to auto-add peer: %s", esp_err_to_name(err));
            return err;
        }
    }

    return esp_now_send(dest, data, len);
}

esp_err_t chal_espnow_recv(uint8_t mac_out[6], int8_t *rssi_out, uint8_t *buf_out, size_t max_len, size_t *len_out, uint32_t timeout_ms)
{
    if (!s_inited || !s_recv_queue) return ESP_ERR_INVALID_STATE;
    if (!mac_out || !buf_out || !len_out) return ESP_ERR_INVALID_ARG;

    espnow_packet_t pkt;
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_recv_queue, &pkt, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(mac_out, pkt.mac, ESP_NOW_ETH_ALEN);
    if (rssi_out) {
        *rssi_out = pkt.rssi;
    }

    size_t cpy = pkt.len > max_len ? max_len : pkt.len;
    memcpy(buf_out, pkt.data, cpy);
    *len_out = cpy;
    return ESP_OK;
}

void chal_espnow_teardown(void)
{
    if (s_inited) {
        chal_espnow_deinit();
    }
}

static bool parse_mac_addr(const char *str, uint8_t mac[6])
{
    if (!str) return false;
    int values[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            mac[i] = (uint8_t)values[i];
        }
        return true;
    }
    return false;
}

static void format_mac_addr(const uint8_t mac[6], char str[18])
{
    snprintf(str, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int lua_espnow_init(lua_State *L)
{
    esp_err_t err = chal_espnow_init();
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_espnow_deinit(lua_State *L)
{
    esp_err_t err = chal_espnow_deinit();
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_espnow_status(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, chal_espnow_is_initialized());
    lua_setfield(L, -2, "initialized");

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        char mac_str[18];
        format_mac_addr(mac, mac_str);
        lua_pushstring(L, mac_str);
        lua_setfield(L, -2, "mac");
    }
    return 1;
}

static int lua_espnow_add_peer(lua_State *L)
{
    const char *mac_str = luaL_checkstring(L, 1);
    uint8_t mac[6];
    if (!parse_mac_addr(mac_str, mac)) {
        return luaL_error(L, "invalid MAC address format (expected xx:xx:xx:xx:xx:xx)");
    }
    uint8_t channel = (uint8_t)luaL_optinteger(L, 2, 0);
    bool encrypt = lua_toboolean(L, 3);
    const uint8_t *lmk = NULL;
    if (encrypt && lua_isstring(L, 4)) {
        size_t lmk_len = 0;
        lmk = (const uint8_t *)lua_tolstring(L, 4, &lmk_len);
        if (lmk_len != 16) {
            return luaL_error(L, "LMK encryption key must be 16 bytes");
        }
    }

    esp_err_t err = chal_espnow_add_peer(mac, channel, encrypt, lmk);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_espnow_del_peer(lua_State *L)
{
    const char *mac_str = luaL_checkstring(L, 1);
    uint8_t mac[6];
    if (!parse_mac_addr(mac_str, mac)) {
        return luaL_error(L, "invalid MAC address format");
    }
    esp_err_t err = chal_espnow_del_peer(mac);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_espnow_is_peer(lua_State *L)
{
    const char *mac_str = luaL_checkstring(L, 1);
    uint8_t mac[6];
    if (!parse_mac_addr(mac_str, mac)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, chal_espnow_is_peer_exist(mac));
    return 1;
}

static int lua_espnow_send(lua_State *L)
{
    int top = lua_gettop(L);
    uint8_t mac[6];
    const uint8_t *dest_mac = NULL;
    size_t payload_len = 0;
    const char *payload = NULL;

    if (top == 1) {
        payload = luaL_checklstring(L, 1, &payload_len);
    } else {
        if (!lua_isnil(L, 1)) {
            const char *mac_str = luaL_checkstring(L, 1);
            if (!parse_mac_addr(mac_str, mac)) {
                return luaL_error(L, "invalid MAC address format");
            }
            dest_mac = mac;
        }
        payload = luaL_checklstring(L, 2, &payload_len);
    }

    if (payload_len > ESP_NOW_MAX_DATA_LEN) {
        return luaL_error(L, "ESP-NOW payload exceeds maximum length (%d bytes)", ESP_NOW_MAX_DATA_LEN);
    }

    esp_err_t err = chal_espnow_send(dest_mac, (const uint8_t *)payload, payload_len);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_espnow_recv(lua_State *L)
{
    uint32_t timeout_ms = (uint32_t)luaL_optinteger(L, 1, 0);
    uint8_t mac[6];
    int8_t rssi = 0;
    uint8_t buf[ESP_NOW_MAX_DATA_LEN];
    size_t len = 0;

    esp_err_t err = chal_espnow_recv(mac, &rssi, buf, sizeof(buf), &len, timeout_ms);
    if (err != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }

    char mac_str[18];
    format_mac_addr(mac, mac_str);
    lua_pushstring(L, mac_str);
    lua_pushlstring(L, (const char *)buf, len);
    lua_pushinteger(L, rssi);
    return 3;
}

static const luaL_Reg s_espnow_funcs[] = {
    {"init", lua_espnow_init},
    {"deinit", lua_espnow_deinit},
    {"status", lua_espnow_status},
    {"add_peer", lua_espnow_add_peer},
    {"del_peer", lua_espnow_del_peer},
    {"is_peer", lua_espnow_is_peer},
    {"send", lua_espnow_send},
    {"recv", lua_espnow_recv},
    {NULL, NULL}
};

int luaopen_espnow(lua_State *L)
{
    luaL_newlib(L, s_espnow_funcs);
    return 1;
}
