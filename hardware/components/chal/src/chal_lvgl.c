#include "chal_lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "chal_lcd.h"
#include "lauxlib.h"

#if CONFIG_NANO_ENABLE_LVGL
#include "lvgl.h"

static const char *TAG = "chal_lvgl";

static SemaphoreHandle_t s_lvgl_mutex = NULL;
static bool s_lvgl_initialized = false;
static TaskHandle_t s_lvgl_task_handle = NULL;
static lv_display_t *s_lvgl_disp = NULL;
static void *s_disp_buf1 = NULL;
static void *s_disp_buf2 = NULL;
static uint32_t s_disp_buf_size_bytes = 0;
static uint32_t s_tick_period_ms = 10;

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int x = area->x1;
    int y = area->y1;
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    esp_err_t err = chal_lcd_draw_bitmap(x, y, w, h, (const uint16_t *)px_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chal_lcd_draw_bitmap failed during flush: %s", esp_err_to_name(err));
    }

    lv_display_flush_ready(disp);
}

static void lvgl_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "LVGL background task started (period: %lu ms)", (unsigned long)s_tick_period_ms);

    while (s_lvgl_initialized) {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(s_tick_period_ms));
    }

    ESP_LOGI(TAG, "LVGL background task terminated");
    vTaskDelete(NULL);
}

esp_err_t chal_lvgl_init(const chal_lvgl_config_t *config)
{
    if (!chal_lcd_is_initialized()) {
        ESP_LOGE(TAG, "Underlying chal_lcd display is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_lvgl_mutex) {
        s_lvgl_mutex = xSemaphoreCreateMutex();
        if (!s_lvgl_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    if (s_lvgl_initialized) {
        xSemaphoreGive(s_lvgl_mutex);
        ESP_LOGW(TAG, "LVGL already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t width = config && config->width > 0 ? config->width : chal_lcd_get_width();
    uint16_t height = config && config->height > 0 ? config->height : chal_lcd_get_height();
    uint16_t buffer_lines = config && config->buffer_lines > 0 ? config->buffer_lines : CONFIG_NANO_LVGL_BUFFER_LINES;
    s_tick_period_ms = config && config->tick_period_ms > 0 ? config->tick_period_ms : CONFIG_NANO_LVGL_TICK_PERIOD_MS;
    bool auto_start_task = config ? config->auto_start_task : true;

    lv_init();

    // Allocate draw buffer in DMA-capable internal SRAM
    size_t buf_pixels = (size_t)width * buffer_lines;
    s_disp_buf_size_bytes = buf_pixels * sizeof(lv_color16_t);

    s_disp_buf1 = heap_caps_malloc(s_disp_buf_size_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_disp_buf1) {
        s_disp_buf1 = malloc(s_disp_buf_size_bytes);
    }
    if (!s_disp_buf1) {
        xSemaphoreGive(s_lvgl_mutex);
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer (%lu bytes)", (unsigned long)s_disp_buf_size_bytes);
        return ESP_ERR_NO_MEM;
    }

    s_lvgl_disp = lv_display_create(width, height);
    if (!s_lvgl_disp) {
        free(s_disp_buf1);
        s_disp_buf1 = NULL;
        xSemaphoreGive(s_lvgl_mutex);
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_FAIL;
    }

    lv_display_set_color_format(s_lvgl_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_lvgl_disp, s_disp_buf1, s_disp_buf2, s_disp_buf_size_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_lvgl_disp, lvgl_flush_cb);

    s_lvgl_initialized = true;

    if (auto_start_task) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            lvgl_task,
            "lvgl_task",
            CONFIG_NANO_LVGL_TASK_STACK_SIZE,
            NULL,
            CONFIG_NANO_LVGL_TASK_PRIORITY,
            &s_lvgl_task_handle,
            1
        );
        if (ret != pdPASS) {
            ESP_LOGW(TAG, "Failed to create background LVGL task; manual stepping required");
            s_lvgl_task_handle = NULL;
        }
    }

    xSemaphoreGive(s_lvgl_mutex);
    ESP_LOGI(TAG, "LVGL initialized successfully (%dx%d, buf_lines=%d)", width, height, buffer_lines);
    return ESP_OK;
}

esp_err_t chal_lvgl_deinit(void)
{
    if (!s_lvgl_initialized) {
        return ESP_OK;
    }

    if (s_lvgl_mutex) {
        xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    }

    s_lvgl_initialized = false;

    if (s_lvgl_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(s_tick_period_ms * 2 + 10));
        s_lvgl_task_handle = NULL;
    }

    if (s_lvgl_disp) {
        lv_display_delete(s_lvgl_disp);
        s_lvgl_disp = NULL;
    }

    if (s_disp_buf1) {
        free(s_disp_buf1);
        s_disp_buf1 = NULL;
    }
    if (s_disp_buf2) {
        free(s_disp_buf2);
        s_disp_buf2 = NULL;
    }

    lv_deinit();

    if (s_lvgl_mutex) {
        xSemaphoreGive(s_lvgl_mutex);
    }

    ESP_LOGI(TAG, "LVGL deinitialized");
    return ESP_OK;
}

bool chal_lvgl_is_initialized(void)
{
    return s_lvgl_initialized;
}

esp_err_t chal_lvgl_step(uint32_t elapsed_ms)
{
    if (!s_lvgl_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_lvgl_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lv_tick_inc(elapsed_ms);
        lv_timer_handler();
        xSemaphoreGive(s_lvgl_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

void chal_lvgl_teardown(void)
{
    if (s_lvgl_initialized) {
        ESP_LOGI(TAG, "Emergency teardown triggered");
        chal_lvgl_deinit();
    }
}

static int lua_lvgl_init(lua_State *L)
{
    chal_lvgl_config_t cfg = {0};
    bool has_cfg = false;

    if (lua_istable(L, 1)) {
        has_cfg = true;
        lua_getfield(L, 1, "width");
        if (lua_isinteger(L, -1)) cfg.width = (uint16_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "height");
        if (lua_isinteger(L, -1)) cfg.height = (uint16_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "buffer_lines");
        if (lua_isinteger(L, -1)) cfg.buffer_lines = (uint16_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "tick_period_ms");
        if (lua_isinteger(L, -1)) cfg.tick_period_ms = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "auto_start_task");
        if (lua_isboolean(L, -1)) cfg.auto_start_task = lua_toboolean(L, -1);
        else cfg.auto_start_task = true;
        lua_pop(L, 1);
    } else {
        cfg.auto_start_task = true;
    }

    esp_err_t err = chal_lvgl_init(has_cfg ? &cfg : NULL);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_deinit(lua_State *L)
{
    esp_err_t err = chal_lvgl_deinit();
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_status(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, chal_lvgl_is_initialized());
    lua_setfield(L, -2, "initialized");

    if (chal_lvgl_is_initialized()) {
        lua_pushinteger(L, chal_lcd_get_width());
        lua_setfield(L, -2, "width");
        lua_pushinteger(L, chal_lcd_get_height());
        lua_setfield(L, -2, "height");
        lua_pushinteger(L, s_tick_period_ms);
        lua_setfield(L, -2, "tick_period_ms");
        lua_pushboolean(L, s_lvgl_task_handle != NULL);
        lua_setfield(L, -2, "task_running");
    }

    return 1;
}

static int lua_lvgl_step(lua_State *L)
{
    uint32_t elapsed = (uint32_t)luaL_optinteger(L, 1, 10);
    esp_err_t err = chal_lvgl_step(elapsed);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg lvgl_funcs[] = {
    {"init", lua_lvgl_init},
    {"deinit", lua_lvgl_deinit},
    {"status", lua_lvgl_status},
    {"step", lua_lvgl_step},
    {NULL, NULL}
};

int luaopen_lvgl(lua_State *L)
{
    luaL_newlib(L, lvgl_funcs);
    return 1;
}

#else // !CONFIG_NANO_ENABLE_LVGL

int luaopen_lvgl(lua_State *L)
{
    return luaL_error(L, "LVGL subsystem is disabled in configuration (CONFIG_NANO_ENABLE_LVGL=n)");
}

esp_err_t chal_lvgl_init(const chal_lvgl_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t chal_lvgl_deinit(void)
{
    return ESP_OK;
}

bool chal_lvgl_is_initialized(void)
{
    return false;
}

esp_err_t chal_lvgl_step(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

void chal_lvgl_teardown(void)
{
}

#endif // CONFIG_NANO_ENABLE_LVGL
