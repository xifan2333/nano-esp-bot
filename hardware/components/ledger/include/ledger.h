#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LEDGER_OWNER_NONE = 0,
    LEDGER_OWNER_KERNEL,
    LEDGER_OWNER_SCRIPT,
} ledger_owner_t;

typedef enum {
    LEDGER_PIN_MODE_FREE = 0,
    LEDGER_PIN_MODE_GPIO_IN,
    LEDGER_PIN_MODE_GPIO_OUT,
    LEDGER_PIN_MODE_PWM,
    LEDGER_PIN_MODE_I2C,
    LEDGER_PIN_MODE_SPI,
    LEDGER_PIN_MODE_RMT,
    LEDGER_PIN_MODE_ADC,
    LEDGER_PIN_MODE_UART,
} ledger_pin_mode_t;

/**
 * @brief Initialize the hardware resource ledger.
 * @return ESP_OK on success.
 */
esp_err_t ledger_init(void);

/**
 * @brief Claim a GPIO pin for a specific owner and mode.
 * @param gpio_num GPIO number (0 to 48).
 * @param owner Owner identity (KERNEL or SCRIPT).
 * @param mode Intended operational mode.
 * @param tag Optional tag (e.g. "led", "button").
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_ARG if invalid or reserved pin,
 *         ESP_ERR_INVALID_STATE if already claimed.
 */
esp_err_t ledger_claim_pin(int gpio_num, ledger_owner_t owner, ledger_pin_mode_t mode, const char *tag);

/**
 * @brief Release a previously claimed GPIO pin.
 * @param gpio_num GPIO number.
 * @param owner Current owner (must match).
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not claimed, ESP_ERR_INVALID_STATE if owner mismatch.
 */
esp_err_t ledger_release_pin(int gpio_num, ledger_owner_t owner);

/**
 * @brief Atomically tear down all resources claimed by an owner.
 *
 * Resets all claimed pins to floating input state,
 * releases hardware channels, and restores hardware safety.
 *
 * @param owner Owner whose resources are to be reclaimed.
 * @return Number of resources freed.
 */
uint32_t ledger_teardown(ledger_owner_t owner);

/**
 * @brief Get total number of active claimed pins for an owner.
 * @param owner Specific owner, or LEDGER_OWNER_NONE to count all claimed pins.
 */
uint32_t ledger_get_claimed_count(ledger_owner_t owner);

/**
 * @brief Check if a GPIO is currently claimed.
 */
bool ledger_is_pin_claimed(int gpio_num);

/**
 * @brief Check if a GPIO is safe to claim (not reserved for Flash/PSRAM/USB).
 */
bool ledger_is_pin_safe(int gpio_num);

#ifdef __cplusplus
}
#endif
