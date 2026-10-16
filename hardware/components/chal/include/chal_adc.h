#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Lua module registration entry point for require("adc").
 */
int luaopen_adc(lua_State *L);

/**
 * @brief Initialize or reconfigure an ADC pin with specified attenuation.
 * @param gpio_num GPIO number supporting ADC.
 * @param atten Attenuation level (0=0dB, 1=2.5dB, 2=6dB, 3=12dB).
 * @return ESP_OK on success.
 */
esp_err_t chal_adc_init_pin(int gpio_num, int atten);

/**
 * @brief Deinitialize a configured ADC pin and release ledger reservation.
 * @param gpio_num GPIO number.
 * @return ESP_OK on success.
 */
esp_err_t chal_adc_deinit_pin(int gpio_num);

/**
 * @brief Read raw 12-bit ADC value (0..4095).
 * @param gpio_num GPIO number.
 * @param out_raw Output raw conversion count.
 * @return ESP_OK on success.
 */
esp_err_t chal_adc_read_raw(int gpio_num, int *out_raw);

/**
 * @brief Read calibrated voltage in millivolts (mV).
 * @param gpio_num GPIO number.
 * @param out_mv Output voltage in millivolts.
 * @return ESP_OK on success.
 */
esp_err_t chal_adc_read_mv(int gpio_num, int *out_mv);

/**
 * @brief Read both raw count and calibrated voltage in one shot.
 * @param gpio_num GPIO number.
 * @param out_raw Output raw conversion count.
 * @param out_mv Output voltage in millivolts.
 * @return ESP_OK on success.
 */
esp_err_t chal_adc_read(int gpio_num, int *out_raw, int *out_mv);

/**
 * @brief Check if a GPIO is currently configured for ADC.
 */
bool chal_adc_is_pin_configured(int gpio_num);

/**
 * @brief Emergency teardown hook for supervisor break / abort.
 */
void chal_adc_teardown(void);

#ifdef __cplusplus
}
#endif
