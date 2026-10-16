#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_espnow(lua_State *L);

esp_err_t chal_espnow_init(void);
esp_err_t chal_espnow_deinit(void);

bool chal_espnow_is_initialized(void);

esp_err_t chal_espnow_add_peer(const uint8_t mac[6], uint8_t channel, bool encrypt, const uint8_t lmk[16]);
esp_err_t chal_espnow_del_peer(const uint8_t mac[6]);
bool chal_espnow_is_peer_exist(const uint8_t mac[6]);

esp_err_t chal_espnow_send(const uint8_t *mac, const uint8_t *data, size_t len);
esp_err_t chal_espnow_recv(uint8_t mac_out[6], int8_t *rssi_out, uint8_t *buf_out, size_t max_len, size_t *len_out, uint32_t timeout_ms);

void chal_espnow_teardown(void);

#ifdef __cplusplus
}
#endif
