#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_ble_hid(lua_State *L);

esp_err_t chal_ble_hid_init(const char *device_name);
esp_err_t chal_ble_hid_start(void);
esp_err_t chal_ble_hid_stop(void);
esp_err_t chal_ble_hid_deinit(void);

bool chal_ble_hid_is_initialized(void);
bool chal_ble_hid_is_started(void);
bool chal_ble_hid_is_connected(void);

esp_err_t chal_ble_hid_send_keyboard(uint8_t modifiers, const uint8_t keycodes[6]);
esp_err_t chal_ble_hid_send_text(const char *text);
esp_err_t chal_ble_hid_send_key(const char *name, bool press, bool release);
esp_err_t chal_ble_hid_send_combo(uint8_t modifiers, const char *key_name);

esp_err_t chal_ble_hid_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
esp_err_t chal_ble_hid_send_consumer(uint16_t usage);
esp_err_t chal_ble_hid_release_all(void);

void chal_ble_hid_teardown(void);

#ifdef __cplusplus
}
#endif
