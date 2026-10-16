#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHAL_I2C_DEFAULT_FREQ_HZ 100000

/**
 * @brief Lua module registration entry point for require("i2c").
 */
int luaopen_i2c(lua_State *L);

/**
 * @brief Initialize I2C master bus on the specified SDA and SCL pins.
 * @param sda_pin GPIO number for SDA.
 * @param scl_pin GPIO number for SCL.
 * @param speed_hz SCL frequency in Hz (typically 100000 or 400000, 0 defaults to 100000).
 * @return ESP_OK on success.
 */
esp_err_t chal_i2c_init(int sda_pin, int scl_pin, uint32_t speed_hz);

/**
 * @brief Deinitialize I2C master bus and release claimed pins.
 */
esp_err_t chal_i2c_deinit(void);

/**
 * @brief Check if I2C bus is initialized.
 */
bool chal_i2c_is_initialized(void);

/**
 * @brief Get active SDA GPIO number.
 */
int chal_i2c_get_sda_pin(void);

/**
 * @brief Get active SCL GPIO number.
 */
int chal_i2c_get_scl_pin(void);

/**
 * @brief Get active clock frequency.
 */
uint32_t chal_i2c_get_speed_hz(void);

/**
 * @brief Scan 7-bit addresses (0x08..0x77) on the I2C bus.
 * @param out_addresses Output array of found device addresses.
 * @param max_addresses Maximum capacity of out_addresses.
 * @param out_count Actual number of devices found.
 * @return ESP_OK on success.
 */
esp_err_t chal_i2c_scan(uint8_t *out_addresses, size_t max_addresses, size_t *out_count);

/**
 * @brief Probe a specific 7-bit I2C address.
 * @param device_addr 7-bit device address.
 * @param timeout_ms Timeout in ms.
 * @return ESP_OK if device ACKed, ESP_ERR_NOT_FOUND if NACKed, or other error.
 */
esp_err_t chal_i2c_probe(uint8_t device_addr, int timeout_ms);

/**
 * @brief Write raw bytes to an I2C device.
 */
esp_err_t chal_i2c_write(uint8_t device_addr, const uint8_t *data, size_t len, int timeout_ms);

/**
 * @brief Read raw bytes from an I2C device.
 */
esp_err_t chal_i2c_read(uint8_t device_addr, uint8_t *buffer, size_t len, int timeout_ms);

/**
 * @brief Write data to a specific register on an I2C device.
 */
esp_err_t chal_i2c_write_reg(uint8_t device_addr, uint8_t reg_addr, const uint8_t *data, size_t len, int timeout_ms);

/**
 * @brief Read data from a specific register on an I2C device.
 */
esp_err_t chal_i2c_read_reg(uint8_t device_addr, uint8_t reg_addr, uint8_t *buffer, size_t len, int timeout_ms);

/**
 * @brief Emergency teardown hook for supervisor break / abort.
 */
void chal_i2c_teardown(void);

#ifdef __cplusplus
}
#endif
