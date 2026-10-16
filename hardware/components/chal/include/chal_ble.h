#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_ble(lua_State *L);

esp_err_t chal_ble_init(void);
esp_err_t chal_ble_deinit(void);

bool chal_ble_is_initialized(void);
bool chal_ble_is_advertising(void);
bool chal_ble_is_connected(void);
esp_err_t chal_ble_get_address(char *addr_str, size_t max_len);

esp_err_t chal_ble_adv_start_ibeacon(const uint8_t uuid[16], uint16_t major, uint16_t minor, int8_t measured_power, uint32_t interval_ms);
esp_err_t chal_ble_adv_start_custom(const char *name, const uint8_t *mfg_data, size_t mfg_len, uint16_t uuid16, uint32_t interval_ms, bool connectable);
esp_err_t chal_ble_adv_start_raw(const uint8_t *raw_adv, size_t raw_adv_len, const uint8_t *scan_rsp, size_t scan_rsp_len, uint32_t interval_ms, bool connectable);
esp_err_t chal_ble_adv_stop(void);

esp_err_t chal_ble_gatts_register_service(const char *svc_uuid, const char *char_uuid, uint8_t properties, const uint8_t *init_val, size_t val_len);
esp_err_t chal_ble_gatts_set_value(const char *char_uuid, const uint8_t *val, size_t len);
esp_err_t chal_ble_gatts_notify(const char *char_uuid, const uint8_t *val, size_t len);

void chal_ble_teardown(void);

#ifdef __cplusplus
}
#endif
