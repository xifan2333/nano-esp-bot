#include "chal_lcd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_lcd";

static SemaphoreHandle_t s_lcd_mutex = NULL;
static bool s_lcd_initialized = false;
static chal_lcd_config_t s_lcd_cfg;
static spi_host_device_t s_spi_host = SPI2_HOST;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;

esp_err_t chal_lcd_init(const chal_lcd_config_t *config)
{
    (void)config;
    (void)s_lcd_mutex;
    (void)s_lcd_cfg;
    (void)s_spi_host;
    (void)s_io_handle;
    (void)s_panel_handle;
    (void)TAG;
    return ESP_OK;
}

esp_err_t chal_lcd_deinit(void)
{
    return ESP_OK;
}

bool chal_lcd_is_initialized(void)
{
    return s_lcd_initialized;
}

uint16_t chal_lcd_get_width(void)
{
    return s_lcd_cfg.width;
}

uint16_t chal_lcd_get_height(void)
{
    return s_lcd_cfg.height;
}

esp_err_t chal_lcd_clear(uint16_t color)
{
    (void)color;
    return ESP_OK;
}

esp_err_t chal_lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
    return ESP_OK;
}

esp_err_t chal_lcd_draw_bitmap(int x, int y, int w, int h, const uint16_t *data)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)data;
    return ESP_OK;
}

esp_err_t chal_lcd_set_rotation(uint16_t rotation)
{
    (void)rotation;
    return ESP_OK;
}

esp_err_t chal_lcd_invert_color(bool invert)
{
    (void)invert;
    return ESP_OK;
}

esp_err_t chal_lcd_set_backlight(bool on)
{
    (void)on;
    return ESP_OK;
}

void chal_lcd_teardown(void)
{
}

int luaopen_lcd(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
