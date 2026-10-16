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

int luaopen_espnow(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
