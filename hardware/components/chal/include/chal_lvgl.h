#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t buffer_lines;
    uint32_t tick_period_ms;
    bool auto_start_task;
} chal_lvgl_config_t;

/**
 * @brief Lua module registration entry point for require("lvgl").
 */
int luaopen_lvgl(lua_State *L);

/**
 * @brief Initialize LVGL core, display buffer, and display driver hooked to chal_lcd.
 * @param config Optional configuration struct (NULL for default based on chal_lcd dimensions).
 * @return ESP_OK on success, or error code.
 */
esp_err_t chal_lvgl_init(const chal_lvgl_config_t *config);

/**
 * @brief Deinitialize LVGL, destroy timer/task, and release allocated buffers.
 * @return ESP_OK on success.
 */
esp_err_t chal_lvgl_deinit(void);

/**
 * @brief Check if LVGL subsystem is initialized and running.
 */
bool chal_lvgl_is_initialized(void);

/**
 * @brief Manually step LVGL timer handler and tick increment.
 * @param elapsed_ms Milliseconds elapsed since last step.
 * @return ESP_OK on success.
 */
esp_err_t chal_lvgl_step(uint32_t elapsed_ms);

/**
 * @brief Emergency teardown hook for supervisor break / abort.
 */
void chal_lvgl_teardown(void);

#ifdef __cplusplus
}
#endif
