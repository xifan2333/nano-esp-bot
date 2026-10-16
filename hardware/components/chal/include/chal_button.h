#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHAL_BUTTON_EVENT_PRESS = 0,
    CHAL_BUTTON_EVENT_RELEASE,
    CHAL_BUTTON_EVENT_CLICK,
    CHAL_BUTTON_EVENT_DOUBLE_CLICK,
    CHAL_BUTTON_EVENT_LONG_PRESS,
} chal_button_event_type_t;

typedef struct {
    int pin;
    chal_button_event_type_t event;
    uint32_t press_duration_ms;
} chal_button_event_t;

typedef struct {
    bool active_low;
    bool pull_up;
    bool pull_down;
    uint32_t long_press_ms;
    uint32_t double_click_ms;
} chal_button_config_t;

/**
 * @brief Lua module registration entry point for require("button").
 */
int luaopen_button(lua_State *L);

/**
 * @brief Initialize or reconfigure a GPIO pin as a debounced button.
 * @param gpio_num GPIO number.
 * @param config Button configuration options (or NULL for standard defaults).
 * @return ESP_OK on success.
 */
esp_err_t chal_button_init_pin(int gpio_num, const chal_button_config_t *config);

/**
 * @brief Deinitialize a button pin and release its ledger reservation.
 * @param gpio_num GPIO number.
 * @return ESP_OK on success.
 */
esp_err_t chal_button_deinit_pin(int gpio_num);

/**
 * @brief Check if a GPIO is currently registered as a button.
 */
bool chal_button_is_configured(int gpio_num);

/**
 * @brief Check whether a registered button is currently held down.
 */
bool chal_button_is_pressed(int gpio_num);

/**
 * @brief Get number of events waiting in the button event queue.
 */
uint32_t chal_button_get_queued_count(void);

/**
 * @brief Receive an event from the button queue with timeout.
 * @param out_event Destination event struct.
 * @param timeout_ms Timeout in ms (0 for non-blocking).
 * @return true if an event was popped, false on timeout/empty.
 */
bool chal_button_recv(chal_button_event_t *out_event, uint32_t timeout_ms);

/**
 * @brief Emergency teardown hook for supervisor break / abort.
 */
void chal_button_teardown(void);

#ifdef __cplusplus
}
#endif
