#include "chal_lcd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_lcd";

#define LCD_DEFAULT_WIDTH      240
#define LCD_DEFAULT_HEIGHT     240
#define LCD_DEFAULT_SPI_FREQ   40000000 // 40MHz
#define LCD_DMA_CHUNK_LINES    16

static SemaphoreHandle_t s_lcd_mutex = NULL;
static bool s_lcd_initialized = false;
static chal_lcd_config_t s_lcd_cfg;
static spi_host_device_t s_spi_host = SPI2_HOST;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static uint16_t *s_dma_chunk_buf = NULL;
static size_t s_dma_chunk_pixels = 0;

static void gc9a01_init_sequence(esp_lcd_panel_io_handle_t io)
{
    esp_lcd_panel_io_tx_param(io, 0xFE, NULL, 0);
    esp_lcd_panel_io_tx_param(io, 0xEF, NULL, 0);

    const uint8_t c84[] = {0x40}; esp_lcd_panel_io_tx_param(io, 0x84, c84, 1);
    const uint8_t c85[] = {0xFF}; esp_lcd_panel_io_tx_param(io, 0x85, c85, 1);
    const uint8_t c86[] = {0xFF}; esp_lcd_panel_io_tx_param(io, 0x86, c86, 1);
    const uint8_t c87[] = {0xFF}; esp_lcd_panel_io_tx_param(io, 0x87, c87, 1);
    const uint8_t c88[] = {0x0A}; esp_lcd_panel_io_tx_param(io, 0x88, c88, 1);
    const uint8_t c89[] = {0x21}; esp_lcd_panel_io_tx_param(io, 0x89, c89, 1);
    const uint8_t c8A[] = {0x00}; esp_lcd_panel_io_tx_param(io, 0x8A, c8A, 1);
    const uint8_t c8B[] = {0x80}; esp_lcd_panel_io_tx_param(io, 0x8B, c8B, 1);
    const uint8_t c8C[] = {0x01}; esp_lcd_panel_io_tx_param(io, 0x8C, c8C, 1);
    const uint8_t c8D[] = {0x01}; esp_lcd_panel_io_tx_param(io, 0x8D, c8D, 1);
    const uint8_t c8E[] = {0xFF}; esp_lcd_panel_io_tx_param(io, 0x8E, c8E, 1);
    const uint8_t c8F[] = {0xFF}; esp_lcd_panel_io_tx_param(io, 0x8F, c8F, 1);
    const uint8_t cB6[] = {0x00, 0x00}; esp_lcd_panel_io_tx_param(io, 0xB6, cB6, 2);
    const uint8_t c3A[] = {0x05}; esp_lcd_panel_io_tx_param(io, 0x3A, c3A, 1);
    const uint8_t cBA[] = {0x01}; esp_lcd_panel_io_tx_param(io, 0xBA, cBA, 1);

    esp_lcd_panel_io_tx_param(io, 0x21, NULL, 0); // Invert ON
    esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0); // Sleep OUT
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0); // Display ON
}

esp_err_t chal_lcd_init(const chal_lcd_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ledger_is_pin_safe(config->sclk_pin) ||
        !ledger_is_pin_safe(config->mosi_pin) ||
        !ledger_is_pin_safe(config->cs_pin) ||
        !ledger_is_pin_safe(config->dc_pin)) {
        ESP_LOGE(TAG, "One or more SPI LCD pins are not safe");
        return ESP_ERR_INVALID_ARG;
    }

    if (config->rst_pin >= 0 && !ledger_is_pin_safe(config->rst_pin)) {
        ESP_LOGE(TAG, "RST pin %d is not safe", config->rst_pin);
        return ESP_ERR_INVALID_ARG;
    }

    if (config->backlight_pin >= 0 && !ledger_is_pin_safe(config->backlight_pin)) {
        ESP_LOGE(TAG, "Backlight pin %d is not safe", config->backlight_pin);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_lcd_mutex) {
        s_lcd_mutex = xSemaphoreCreateMutex();
        if (!s_lcd_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    if (s_lcd_initialized) {
        xSemaphoreGive(s_lcd_mutex);
        ESP_LOGW(TAG, "LCD is already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Claim pins in Ledger
    esp_err_t err = ledger_claim_pin(config->sclk_pin, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_SPI, "lcd_sclk");
    if (err != ESP_OK) {
        xSemaphoreGive(s_lcd_mutex);
        return err;
    }
    err = ledger_claim_pin(config->mosi_pin, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_SPI, "lcd_mosi");
    if (err != ESP_OK) {
        ledger_release_pin(config->sclk_pin, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_lcd_mutex);
        return err;
    }
    err = ledger_claim_pin(config->cs_pin, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_SPI, "lcd_cs");
    if (err != ESP_OK) {
        ledger_release_pin(config->mosi_pin, LEDGER_OWNER_SCRIPT);
        ledger_release_pin(config->sclk_pin, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_lcd_mutex);
        return err;
    }
    err = ledger_claim_pin(config->dc_pin, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_GPIO_OUT, "lcd_dc");
    if (err != ESP_OK) {
        ledger_release_pin(config->cs_pin, LEDGER_OWNER_SCRIPT);
        ledger_release_pin(config->mosi_pin, LEDGER_OWNER_SCRIPT);
        ledger_release_pin(config->sclk_pin, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_lcd_mutex);
        return err;
    }
    if (config->rst_pin >= 0) {
        err = ledger_claim_pin(config->rst_pin, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_GPIO_OUT, "lcd_rst");
        if (err != ESP_OK) {
            ledger_release_pin(config->dc_pin, LEDGER_OWNER_SCRIPT);
            ledger_release_pin(config->cs_pin, LEDGER_OWNER_SCRIPT);
            ledger_release_pin(config->mosi_pin, LEDGER_OWNER_SCRIPT);
            ledger_release_pin(config->sclk_pin, LEDGER_OWNER_SCRIPT);
            xSemaphoreGive(s_lcd_mutex);
            return err;
        }
    }
    if (config->backlight_pin >= 0) {
        err = ledger_claim_pin(config->backlight_pin, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_GPIO_OUT, "lcd_bk");
        if (err != ESP_OK) {
            if (config->rst_pin >= 0) ledger_release_pin(config->rst_pin, LEDGER_OWNER_SCRIPT);
            ledger_release_pin(config->dc_pin, LEDGER_OWNER_SCRIPT);
            ledger_release_pin(config->cs_pin, LEDGER_OWNER_SCRIPT);
            ledger_release_pin(config->mosi_pin, LEDGER_OWNER_SCRIPT);
            ledger_release_pin(config->sclk_pin, LEDGER_OWNER_SCRIPT);
            xSemaphoreGive(s_lcd_mutex);
            return err;
        }
    }

    uint16_t width = config->width ? config->width : LCD_DEFAULT_WIDTH;
    uint16_t height = config->height ? config->height : LCD_DEFAULT_HEIGHT;
    uint32_t speed = config->speed_hz ? config->speed_hz : LCD_DEFAULT_SPI_FREQ;

    spi_bus_config_t buscfg = {
        .sclk_io_num = config->sclk_pin,
        .mosi_io_num = config->mosi_pin,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)(width * LCD_DMA_CHUNK_LINES * sizeof(uint16_t)),
    };

    err = spi_bus_initialize(s_spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        goto err_cleanup_pins;
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = config->dc_pin,
        .cs_gpio_num = config->cs_pin,
        .pclk_hz = speed,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)s_spi_host, &io_config, &s_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        spi_bus_free(s_spi_host);
        goto err_cleanup_pins;
    }

    esp_lcd_panel_dev_config_t panel_dev_config = {
        .reset_gpio_num = config->rst_pin,
        .rgb_ele_order = config->bgr_order ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    };

    err = esp_lcd_new_panel_st7789(s_io_handle, &panel_dev_config, &s_panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(err));
        esp_lcd_panel_io_del(s_io_handle);
        s_io_handle = NULL;
        spi_bus_free(s_spi_host);
        goto err_cleanup_pins;
    }

    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);

    if (config->driver == CHAL_LCD_DRIVER_GC9A01) {
        gc9a01_init_sequence(s_io_handle);
    }

    if (config->invert_color) {
        esp_lcd_panel_invert_color(s_panel_handle, true);
    }

    if (config->x_offset > 0 || config->y_offset > 0) {
        esp_lcd_panel_set_gap(s_panel_handle, config->x_offset, config->y_offset);
    }

    chal_lcd_set_rotation(config->rotation);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    if (config->backlight_pin >= 0) {
        gpio_config_t bk_conf = {
            .pin_bit_mask = 1ULL << config->backlight_pin,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&bk_conf);
        gpio_set_level((gpio_num_t)config->backlight_pin, 1);
    }

    s_dma_chunk_pixels = (size_t)width * LCD_DMA_CHUNK_LINES;
    s_dma_chunk_buf = (uint16_t *)heap_caps_malloc(s_dma_chunk_pixels * sizeof(uint16_t),
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    s_lcd_cfg = *config;
    s_lcd_cfg.width = width;
    s_lcd_cfg.height = height;
    s_lcd_cfg.speed_hz = speed;
    s_lcd_initialized = true;

    xSemaphoreGive(s_lcd_mutex);
    chal_lcd_clear(0x0000); // Clear screen to black on init
    ESP_LOGI(TAG, "LCD initialized (%dx%d, speed=%lu Hz, driver=%d)",
             width, height, (unsigned long)speed, (int)config->driver);
    return ESP_OK;

err_cleanup_pins:
    if (config->backlight_pin >= 0) ledger_release_pin(config->backlight_pin, LEDGER_OWNER_SCRIPT);
    if (config->rst_pin >= 0) ledger_release_pin(config->rst_pin, LEDGER_OWNER_SCRIPT);
    ledger_release_pin(config->dc_pin, LEDGER_OWNER_SCRIPT);
    ledger_release_pin(config->cs_pin, LEDGER_OWNER_SCRIPT);
    ledger_release_pin(config->mosi_pin, LEDGER_OWNER_SCRIPT);
    ledger_release_pin(config->sclk_pin, LEDGER_OWNER_SCRIPT);
    xSemaphoreGive(s_lcd_mutex);
    return err;
}

esp_err_t chal_lcd_deinit(void)
{
    if (!s_lcd_mutex) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    if (!s_lcd_initialized) {
        xSemaphoreGive(s_lcd_mutex);
        return ESP_OK;
    }

    if (s_lcd_cfg.backlight_pin >= 0) {
        gpio_set_level((gpio_num_t)s_lcd_cfg.backlight_pin, 0);
        ledger_release_pin(s_lcd_cfg.backlight_pin, LEDGER_OWNER_SCRIPT);
    }

    if (s_panel_handle) {
        esp_lcd_panel_disp_on_off(s_panel_handle, false);
        esp_lcd_panel_del(s_panel_handle);
        s_panel_handle = NULL;
    }

    if (s_io_handle) {
        esp_lcd_panel_io_del(s_io_handle);
        s_io_handle = NULL;
    }

    spi_bus_free(s_spi_host);

    if (s_lcd_cfg.rst_pin >= 0) ledger_release_pin(s_lcd_cfg.rst_pin, LEDGER_OWNER_SCRIPT);
    ledger_release_pin(s_lcd_cfg.dc_pin, LEDGER_OWNER_SCRIPT);
    ledger_release_pin(s_lcd_cfg.cs_pin, LEDGER_OWNER_SCRIPT);
    ledger_release_pin(s_lcd_cfg.mosi_pin, LEDGER_OWNER_SCRIPT);
    ledger_release_pin(s_lcd_cfg.sclk_pin, LEDGER_OWNER_SCRIPT);

    if (s_dma_chunk_buf) {
        free(s_dma_chunk_buf);
        s_dma_chunk_buf = NULL;
        s_dma_chunk_pixels = 0;
    }

    s_lcd_initialized = false;
    memset(&s_lcd_cfg, 0, sizeof(chal_lcd_config_t));
    xSemaphoreGive(s_lcd_mutex);
    ESP_LOGI(TAG, "LCD deinitialized");
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

esp_err_t chal_lcd_draw_bitmap(int x, int y, int w, int h, const uint16_t *data)
{
    if (!s_lcd_initialized || !s_panel_handle || !data) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + w, y + h, data);
    xSemaphoreGive(s_lcd_mutex);
    return err;
}

esp_err_t chal_lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    if (!s_lcd_initialized || !s_panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);

    // Swap color byte order for SPI big-endian transmit
    uint16_t swapped_color = (uint16_t)((color << 8) | (color >> 8));

    if (!s_dma_chunk_buf) {
        s_dma_chunk_pixels = (size_t)s_lcd_cfg.width * LCD_DMA_CHUNK_LINES;
        s_dma_chunk_buf = (uint16_t *)heap_caps_malloc(s_dma_chunk_pixels * sizeof(uint16_t),
                                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_dma_chunk_buf) {
            xSemaphoreGive(s_lcd_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    for (size_t i = 0; i < s_dma_chunk_pixels; i++) {
        s_dma_chunk_buf[i] = swapped_color;
    }

    int cur_y = y;
    int rem_h = h;
    esp_err_t err = ESP_OK;

    while (rem_h > 0) {
        int slice_h = (rem_h > LCD_DMA_CHUNK_LINES) ? LCD_DMA_CHUNK_LINES : rem_h;
        err = esp_lcd_panel_draw_bitmap(s_panel_handle, x, cur_y, x + w, cur_y + slice_h, s_dma_chunk_buf);
        if (err != ESP_OK) {
            break;
        }
        cur_y += slice_h;
        rem_h -= slice_h;
    }

    xSemaphoreGive(s_lcd_mutex);
    return err;
}

esp_err_t chal_lcd_clear(uint16_t color)
{
    return chal_lcd_fill(0, 0, s_lcd_cfg.width, s_lcd_cfg.height, color);
}

esp_err_t chal_lcd_set_rotation(uint16_t rotation)
{
    if (!s_panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (rotation) {
    case 0:
        esp_lcd_panel_swap_xy(s_panel_handle, false);
        esp_lcd_panel_mirror(s_panel_handle, false, false);
        break;
    case 90:
        esp_lcd_panel_swap_xy(s_panel_handle, true);
        esp_lcd_panel_mirror(s_panel_handle, false, true);
        break;
    case 180:
        esp_lcd_panel_swap_xy(s_panel_handle, false);
        esp_lcd_panel_mirror(s_panel_handle, true, true);
        break;
    case 270:
        esp_lcd_panel_swap_xy(s_panel_handle, true);
        esp_lcd_panel_mirror(s_panel_handle, true, false);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    s_lcd_cfg.rotation = rotation;
    return ESP_OK;
}

esp_err_t chal_lcd_invert_color(bool invert)
{
    if (!s_panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lcd_cfg.invert_color = invert;
    return esp_lcd_panel_invert_color(s_panel_handle, invert);
}

esp_err_t chal_lcd_set_backlight(bool on)
{
    if (s_lcd_cfg.backlight_pin >= 0) {
        gpio_set_level((gpio_num_t)s_lcd_cfg.backlight_pin, on ? 1 : 0);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void chal_lcd_teardown(void)
{
    chal_lcd_deinit();
}

int luaopen_lcd(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
