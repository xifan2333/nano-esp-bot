#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_led_strip(lua_State *L);

esp_err_t chal_led_strip_init(int gpio_num, uint32_t num_leds);
esp_err_t chal_led_strip_deinit(void);

bool chal_led_strip_is_initialized(void);
int chal_led_strip_get_pin(void);
uint32_t chal_led_strip_get_length(void);

esp_err_t chal_led_strip_set_pixel(uint32_t index, uint8_t red, uint8_t green, uint8_t blue);
esp_err_t chal_led_strip_fill(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t chal_led_strip_refresh(void);
esp_err_t chal_led_strip_clear(void);

void chal_led_strip_teardown(void);

#ifdef __cplusplus
}
#endif
