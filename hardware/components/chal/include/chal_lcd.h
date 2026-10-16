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
    CHAL_LCD_DRIVER_ST7789 = 0,
    CHAL_LCD_DRIVER_GC9A01,
    CHAL_LCD_DRIVER_ILI9341,
    CHAL_LCD_DRIVER_ST7735,
    CHAL_LCD_DRIVER_CUSTOM,
} chal_lcd_driver_t;

typedef struct {
    chal_lcd_driver_t driver;
    int sclk_pin;
    int mosi_pin;
    int cs_pin;
    int dc_pin;
    int rst_pin;
    int backlight_pin;
    uint16_t width;
    uint16_t height;
    uint16_t x_offset;
    uint16_t y_offset;
    uint32_t speed_hz;
    uint16_t rotation;
    bool invert_color;
    bool bgr_order;
} chal_lcd_config_t;

/**
 * @brief Lua module registration entry point for require("lcd").
 */
int luaopen_lcd(lua_State *L);

/**
 * @brief Initialize LCD hardware bus and panel driver.
 * @param config Panel configuration struct.
 * @return ESP_OK on success.
 */
esp_err_t chal_lcd_init(const chal_lcd_config_t *config);

/**
 * @brief Deinitialize LCD panel, bus, and release claimed pins.
 * @return ESP_OK on success.
 */
esp_err_t chal_lcd_deinit(void);

/**
 * @brief Check if LCD panel is initialized.
 */
bool chal_lcd_is_initialized(void);

/**
 * @brief Get active display width in pixels.
 */
uint16_t chal_lcd_get_width(void);

/**
 * @brief Get active display height in pixels.
 */
uint16_t chal_lcd_get_height(void);

/**
 * @brief Clear entire screen to a uniform RGB565 color.
 * @param color 16-bit RGB565 color.
 * @return ESP_OK on success.
 */
esp_err_t chal_lcd_clear(uint16_t color);

/**
 * @brief Fill a rectangular region with an RGB565 color.
 * @param x Top-left X coordinate.
 * @param y Top-left Y coordinate.
 * @param w Width of rectangle.
 * @param h Height of rectangle.
 * @param color 16-bit RGB565 color.
 * @return ESP_OK on success.
 */
esp_err_t chal_lcd_fill(int x, int y, int w, int h, uint16_t color);

/**
 * @brief Push a raw RGB565 pixel buffer to a display region.
 * @param x Top-left X coordinate.
 * @param y Top-left Y coordinate.
 * @param w Width of rectangle.
 * @param h Height of rectangle.
 * @param data Array of 16-bit RGB565 pixel values.
 * @return ESP_OK on success.
 */
esp_err_t chal_lcd_draw_bitmap(int x, int y, int w, int h, const uint16_t *data);

/**
 * @brief Set display rotation (0, 90, 180, 270 degrees).
 */
esp_err_t chal_lcd_set_rotation(uint16_t rotation);

/**
 * @brief Invert display color polarity.
 */
esp_err_t chal_lcd_invert_color(bool invert);

/**
 * @brief Enable or disable LCD backlight.
 */
esp_err_t chal_lcd_set_backlight(bool on);

/**
 * @brief Emergency teardown hook for supervisor break / abort.
 */
void chal_lcd_teardown(void);

#ifdef __cplusplus
}
#endif
