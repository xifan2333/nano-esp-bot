#include "chal_lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "chal_lcd.h"
#include "lauxlib.h"

#define NANO_LVGL_OBJ_META "nano.lvgl.obj"

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

typedef struct {
    lv_obj_t *obj;
} lua_lvgl_obj_t;

static uint32_t lvgl_tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);

    esp_err_t err = chal_lcd_draw_bitmap(area->x1, area->y1, (int)w, (int)h, (const uint16_t *)px_map);
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
        uint32_t delay_ms = s_tick_period_ms;
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint32_t next_ms = lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
            if (next_ms > 0 && next_ms < delay_ms) {
                delay_ms = next_ms;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));
    }

    ESP_LOGI(TAG, "LVGL background task terminated");
    vTaskDelete(NULL);
}

void chal_lvgl_lock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    }
}

void chal_lvgl_unlock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreGive(s_lvgl_mutex);
    }
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
    lv_tick_set_cb(lvgl_tick_get_cb);

    size_t buf_pixels = (size_t)width * buffer_lines;
    s_disp_buf_size_bytes = buf_pixels * sizeof(lv_color16_t);

    s_disp_buf1 = heap_caps_malloc(s_disp_buf_size_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_disp_buf1) {
        s_disp_buf1 = malloc(s_disp_buf_size_bytes);
    }
    if (!s_disp_buf1) {
        xSemaphoreGive(s_lvgl_mutex);
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer 1 (%lu bytes)", (unsigned long)s_disp_buf_size_bytes);
        return ESP_ERR_NO_MEM;
    }

    s_disp_buf2 = heap_caps_malloc(s_disp_buf_size_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_disp_buf2) {
        s_disp_buf2 = malloc(s_disp_buf_size_bytes);
    }

    s_lvgl_disp = lv_display_create(width, height);
    if (!s_lvgl_disp) {
        free(s_disp_buf1);
        s_disp_buf1 = NULL;
        if (s_disp_buf2) {
            free(s_disp_buf2);
            s_disp_buf2 = NULL;
        }
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
    ESP_LOGI(TAG, "LVGL initialized successfully (%dx%d, buf_lines=%d, double_buf=%s)",
             width, height, buffer_lines, s_disp_buf2 ? "true" : "false");
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
        lv_obj_clean(lv_screen_active());
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

/* -------------------------------------------------------------------------- */
/* Lua Metatable and Type Helpers                                             */
/* -------------------------------------------------------------------------- */

static lua_lvgl_obj_t *push_lvgl_obj(lua_State *L, lv_obj_t *obj)
{
    if (!obj) {
        lua_pushnil(L);
        return NULL;
    }
    lua_lvgl_obj_t *ud = (lua_lvgl_obj_t *)lua_newuserdata(L, sizeof(lua_lvgl_obj_t));
    ud->obj = obj;
    luaL_getmetatable(L, NANO_LVGL_OBJ_META);
    lua_setmetatable(L, -2);
    return ud;
}

static lv_obj_t *check_lvgl_obj(lua_State *L, int arg)
{
    lua_lvgl_obj_t *ud = (lua_lvgl_obj_t *)luaL_checkudata(L, arg, NANO_LVGL_OBJ_META);
    if (!ud || !ud->obj) {
        luaL_error(L, "null or uninitialized LVGL object");
        return NULL;
    }
    if (!s_lvgl_initialized || !lv_obj_is_valid(ud->obj)) {
        luaL_error(L, "invalid or deleted LVGL object");
        return NULL;
    }
    return ud->obj;
}

static lv_obj_t *opt_lvgl_obj(lua_State *L, int arg)
{
    if (lua_isnoneornil(L, arg)) {
        return lv_screen_active();
    }
    return check_lvgl_obj(L, arg);
}

static lv_color_t parse_lua_color(lua_State *L, int idx)
{
    if (lua_isinteger(L, idx)) {
        uint32_t val = (uint32_t)lua_tointeger(L, idx);
        return lv_color_hex(val);
    } else if (lua_istable(L, idx)) {
        lua_rawgeti(L, idx, 1);
        uint8_t r = (uint8_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_rawgeti(L, idx, 2);
        uint8_t g = (uint8_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_rawgeti(L, idx, 3);
        uint8_t b = (uint8_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        return lv_color_make(r, g, b);
    }
    return lv_color_hex(0);
}

static lv_align_t parse_align(const char *str)
{
    if (!str) return LV_ALIGN_CENTER;
    if (strcmp(str, "center") == 0) return LV_ALIGN_CENTER;
    if (strcmp(str, "top_left") == 0) return LV_ALIGN_TOP_LEFT;
    if (strcmp(str, "top_mid") == 0) return LV_ALIGN_TOP_MID;
    if (strcmp(str, "top_right") == 0) return LV_ALIGN_TOP_RIGHT;
    if (strcmp(str, "bottom_left") == 0) return LV_ALIGN_BOTTOM_LEFT;
    if (strcmp(str, "bottom_mid") == 0) return LV_ALIGN_BOTTOM_MID;
    if (strcmp(str, "bottom_right") == 0) return LV_ALIGN_BOTTOM_RIGHT;
    if (strcmp(str, "left_mid") == 0) return LV_ALIGN_LEFT_MID;
    if (strcmp(str, "right_mid") == 0) return LV_ALIGN_RIGHT_MID;
    if (strcmp(str, "out_top_left") == 0) return LV_ALIGN_OUT_TOP_LEFT;
    if (strcmp(str, "out_top_mid") == 0) return LV_ALIGN_OUT_TOP_MID;
    if (strcmp(str, "out_top_right") == 0) return LV_ALIGN_OUT_TOP_RIGHT;
    if (strcmp(str, "out_bottom_left") == 0) return LV_ALIGN_OUT_BOTTOM_LEFT;
    if (strcmp(str, "out_bottom_mid") == 0) return LV_ALIGN_OUT_BOTTOM_MID;
    if (strcmp(str, "out_bottom_right") == 0) return LV_ALIGN_OUT_BOTTOM_RIGHT;
    if (strcmp(str, "out_left_top") == 0) return LV_ALIGN_OUT_LEFT_TOP;
    if (strcmp(str, "out_left_mid") == 0) return LV_ALIGN_OUT_LEFT_MID;
    if (strcmp(str, "out_left_bottom") == 0) return LV_ALIGN_OUT_LEFT_BOTTOM;
    if (strcmp(str, "out_right_top") == 0) return LV_ALIGN_OUT_RIGHT_TOP;
    if (strcmp(str, "out_right_mid") == 0) return LV_ALIGN_OUT_RIGHT_MID;
    if (strcmp(str, "out_right_bottom") == 0) return LV_ALIGN_OUT_RIGHT_BOTTOM;
    return LV_ALIGN_DEFAULT;
}

/* -------------------------------------------------------------------------- */
/* Object Methods (Chained API)                                               */
/* -------------------------------------------------------------------------- */

static int lua_obj_set_pos(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    int32_t y = (int32_t)luaL_checkinteger(L, 3);
    chal_lvgl_lock();
    lv_obj_set_pos(obj, x, y);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_x(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    chal_lvgl_lock();
    lv_obj_set_x(obj, x);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_y(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t y = (int32_t)luaL_checkinteger(L, 2);
    chal_lvgl_lock();
    lv_obj_set_y(obj, y);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_size(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t w = (int32_t)luaL_checkinteger(L, 2);
    int32_t h = (int32_t)luaL_checkinteger(L, 3);
    chal_lvgl_lock();
    lv_obj_set_size(obj, w, h);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_width(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t w = (int32_t)luaL_checkinteger(L, 2);
    chal_lvgl_lock();
    lv_obj_set_width(obj, w);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_height(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t h = (int32_t)luaL_checkinteger(L, 2);
    chal_lvgl_lock();
    lv_obj_set_height(obj, h);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_center(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    chal_lvgl_lock();
    lv_obj_center(obj);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_align(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    const char *al_str = luaL_checkstring(L, 2);
    int32_t x_ofs = (int32_t)luaL_optinteger(L, 3, 0);
    int32_t y_ofs = (int32_t)luaL_optinteger(L, 4, 0);
    chal_lvgl_lock();
    lv_obj_align(obj, parse_align(al_str), x_ofs, y_ofs);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_align_to(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    lv_obj_t *base = check_lvgl_obj(L, 2);
    const char *al_str = luaL_checkstring(L, 3);
    int32_t x_ofs = (int32_t)luaL_optinteger(L, 4, 0);
    int32_t y_ofs = (int32_t)luaL_optinteger(L, 5, 0);
    chal_lvgl_lock();
    lv_obj_align_to(obj, base, parse_align(al_str), x_ofs, y_ofs);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_text(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    const char *text = luaL_checkstring(L, 2);
    chal_lvgl_lock();
    if (lv_obj_get_class(obj) == &lv_label_class) {
        lv_label_set_text(obj, text);
    } else {
        lv_obj_t *child = lv_obj_get_child_by_type(obj, 0, &lv_label_class);
        if (child) {
            lv_label_set_text(child, text);
        } else if (lv_obj_get_class(obj) == &lv_button_class) {
            lv_obj_t *lbl = lv_label_create(obj);
            lv_obj_center(lbl);
            lv_label_set_text(lbl, text);
        }
    }
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_get_text(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    const char *text = NULL;
    chal_lvgl_lock();
    if (lv_obj_get_class(obj) == &lv_label_class) {
        text = lv_label_get_text(obj);
    } else {
        lv_obj_t *child = lv_obj_get_child_by_type(obj, 0, &lv_label_class);
        if (child) {
            text = lv_label_get_text(child);
        }
    }
    if (text) {
        lua_pushstring(L, text);
    } else {
        lua_pushnil(L);
    }
    chal_lvgl_unlock();
    return 1;
}

static int lua_obj_set_value(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t val = (int32_t)luaL_checkinteger(L, 2);
    bool anim = lua_toboolean(L, 3);
    chal_lvgl_lock();
    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    if (cls == &lv_bar_class) {
        lv_bar_set_value(obj, val, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    } else if (cls == &lv_slider_class) {
        lv_slider_set_value(obj, val, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    } else if (cls == &lv_arc_class) {
        lv_arc_set_value(obj, val);
    }
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_get_value(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t val = 0;
    chal_lvgl_lock();
    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    if (cls == &lv_bar_class) {
        val = lv_bar_get_value(obj);
    } else if (cls == &lv_slider_class) {
        val = lv_slider_get_value(obj);
    } else if (cls == &lv_arc_class) {
        val = lv_arc_get_value(obj);
    }
    chal_lvgl_unlock();
    lua_pushinteger(L, val);
    return 1;
}

static int lua_obj_set_range(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t min = (int32_t)luaL_checkinteger(L, 2);
    int32_t max = (int32_t)luaL_checkinteger(L, 3);
    chal_lvgl_lock();
    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    if (cls == &lv_bar_class) {
        lv_bar_set_range(obj, min, max);
    } else if (cls == &lv_slider_class) {
        lv_slider_set_range(obj, min, max);
    } else if (cls == &lv_arc_class) {
        lv_arc_set_range(obj, min, max);
    }
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_bg_color(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    lv_color_t col = parse_lua_color(L, 2);
    chal_lvgl_lock();
    lv_obj_set_style_bg_color(obj, col, 0);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_text_color(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    lv_color_t col = parse_lua_color(L, 2);
    chal_lvgl_lock();
    lv_obj_set_style_text_color(obj, col, 0);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_bg_opa(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    uint8_t opa = (uint8_t)luaL_checkinteger(L, 2);
    chal_lvgl_lock();
    lv_obj_set_style_bg_opa(obj, opa, 0);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_radius(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t r = (int32_t)luaL_checkinteger(L, 2);
    chal_lvgl_lock();
    lv_obj_set_style_radius(obj, r, 0);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_border(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    int32_t width = (int32_t)luaL_checkinteger(L, 2);
    lv_color_t col = parse_lua_color(L, 3);
    chal_lvgl_lock();
    lv_obj_set_style_border_width(obj, width, 0);
    lv_obj_set_style_border_color(obj, col, 0);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_set_state(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    bool state = lua_toboolean(L, 2);
    chal_lvgl_lock();
    if (state) {
        lv_obj_add_state(obj, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(obj, LV_STATE_CHECKED);
    }
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_get_state(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    chal_lvgl_lock();
    bool state = lv_obj_has_state(obj, LV_STATE_CHECKED);
    chal_lvgl_unlock();
    lua_pushboolean(L, state);
    return 1;
}

static int lua_obj_clean(lua_State *L)
{
    lv_obj_t *obj = check_lvgl_obj(L, 1);
    chal_lvgl_lock();
    lv_obj_clean(obj);
    chal_lvgl_unlock();
    lua_settop(L, 1);
    return 1;
}

static int lua_obj_delete(lua_State *L)
{
    lua_lvgl_obj_t *ud = (lua_lvgl_obj_t *)luaL_checkudata(L, 1, NANO_LVGL_OBJ_META);
    if (ud && ud->obj && s_lvgl_initialized && lv_obj_is_valid(ud->obj)) {
        chal_lvgl_lock();
        lv_obj_delete(ud->obj);
        ud->obj = NULL;
        chal_lvgl_unlock();
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_obj_is_valid(lua_State *L)
{
    lua_lvgl_obj_t *ud = (lua_lvgl_obj_t *)luaL_checkudata(L, 1, NANO_LVGL_OBJ_META);
    bool valid = ud && ud->obj && s_lvgl_initialized && lv_obj_is_valid(ud->obj);
    lua_pushboolean(L, valid);
    return 1;
}

static int lua_obj_tostring(lua_State *L)
{
    lua_lvgl_obj_t *ud = (lua_lvgl_obj_t *)luaL_checkudata(L, 1, NANO_LVGL_OBJ_META);
    char buf[64];
    if (ud && ud->obj && s_lvgl_initialized && lv_obj_is_valid(ud->obj)) {
        snprintf(buf, sizeof(buf), "lvgl.obj<%p>", (void *)ud->obj);
    } else {
        snprintf(buf, sizeof(buf), "lvgl.obj<invalid>");
    }
    lua_pushstring(L, buf);
    return 1;
}

static void register_lvgl_obj_meta(lua_State *L)
{
    if (luaL_newmetatable(L, NANO_LVGL_OBJ_META)) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        static const luaL_Reg obj_methods[] = {
            {"set_pos", lua_obj_set_pos},
            {"set_x", lua_obj_set_x},
            {"set_y", lua_obj_set_y},
            {"set_size", lua_obj_set_size},
            {"set_width", lua_obj_set_width},
            {"set_height", lua_obj_set_height},
            {"center", lua_obj_center},
            {"align", lua_obj_align},
            {"align_to", lua_obj_align_to},
            {"set_text", lua_obj_set_text},
            {"get_text", lua_obj_get_text},
            {"set_value", lua_obj_set_value},
            {"get_value", lua_obj_get_value},
            {"set_range", lua_obj_set_range},
            {"set_bg_color", lua_obj_set_bg_color},
            {"set_text_color", lua_obj_set_text_color},
            {"set_bg_opa", lua_obj_set_bg_opa},
            {"set_radius", lua_obj_set_radius},
            {"set_border", lua_obj_set_border},
            {"set_state", lua_obj_set_state},
            {"get_state", lua_obj_get_state},
            {"clean", lua_obj_clean},
            {"delete", lua_obj_delete},
            {"is_valid", lua_obj_is_valid},
            {"__tostring", lua_obj_tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, obj_methods, 0);
    }
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------- */
/* Lua Module Functions                                                       */
/* -------------------------------------------------------------------------- */

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

static int lua_lvgl_scr_act(lua_State *L)
{
    if (!s_lvgl_initialized) {
        return luaL_error(L, "LVGL is not initialized");
    }
    chal_lvgl_lock();
    lv_obj_t *scr = lv_screen_active();
    chal_lvgl_unlock();
    push_lvgl_obj(L, scr);
    return 1;
}

static int lua_lvgl_clean(lua_State *L)
{
    if (!s_lvgl_initialized) {
        return luaL_error(L, "LVGL is not initialized");
    }
    lv_obj_t *target = opt_lvgl_obj(L, 1);
    chal_lvgl_lock();
    lv_obj_clean(target);
    chal_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_create_obj(lua_State *L)
{
    if (!s_lvgl_initialized) {
        return luaL_error(L, "LVGL is not initialized");
    }
    lv_obj_t *parent = opt_lvgl_obj(L, 1);
    chal_lvgl_lock();
    lv_obj_t *obj = lv_obj_create(parent);
    chal_lvgl_unlock();
    push_lvgl_obj(L, obj);
    return 1;
}

static int lua_lvgl_create_label(lua_State *L)
{
    if (!s_lvgl_initialized) {
        return luaL_error(L, "LVGL is not initialized");
    }
    lv_obj_t *parent = NULL;
    const char *text = NULL;

    if (lua_gettop(L) == 0) {
        parent = lv_screen_active();
    } else if (lua_gettop(L) == 1) {
        if (lua_isstring(L, 1)) {
            parent = lv_screen_active();
            text = lua_tostring(L, 1);
        } else {
            parent = check_lvgl_obj(L, 1);
        }
    } else {
        parent = opt_lvgl_obj(L, 1);
        text = luaL_optstring(L, 2, NULL);
    }

    chal_lvgl_lock();
    lv_obj_t *lbl = lv_label_create(parent);
    if (text) {
        lv_label_set_text(lbl, text);
    }
    chal_lvgl_unlock();
    push_lvgl_obj(L, lbl);
    return 1;
}

static int lua_lvgl_create_btn(lua_State *L)
{
    if (!s_lvgl_initialized) {
        return luaL_error(L, "LVGL is not initialized");
    }
    lv_obj_t *parent = NULL;
    const char *text = NULL;

    if (lua_gettop(L) == 0) {
        parent = lv_screen_active();
    } else if (lua_gettop(L) == 1) {
        if (lua_isstring(L, 1)) {
            parent = lv_screen_active();
            text = lua_tostring(L, 1);
        } else {
            parent = check_lvgl_obj(L, 1);
        }
    } else {
        parent = opt_lvgl_obj(L, 1);
        text = luaL_optstring(L, 2, NULL);
    }

    chal_lvgl_lock();
    lv_obj_t *btn = lv_button_create(parent);
    if (text) {
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_center(lbl);
        lv_label_set_text(lbl, text);
    }
    chal_lvgl_unlock();
    push_lvgl_obj(L, btn);
    return 1;
}

static int lua_lvgl_create_bar(lua_State *L)
{
    if (!s_lvgl_initialized) {
        return luaL_error(L, "LVGL is not initialized");
    }
    lv_obj_t *parent = opt_lvgl_obj(L, 1);
    chal_lvgl_lock();
    lv_obj_t *bar = lv_bar_create(parent);
    if (lua_gettop(L) >= 3) {
        int32_t min = (int32_t)luaL_checkinteger(L, 2);
        int32_t max = (int32_t)luaL_checkinteger(L, 3);
        lv_bar_set_range(bar, min, max);
    }
    if (lua_gettop(L) >= 4) {
        int32_t val = (int32_t)luaL_checkinteger(L, 4);
        lv_bar_set_value(bar, val, LV_ANIM_OFF);
    }
    chal_lvgl_unlock();
    push_lvgl_obj(L, bar);
    return 1;
}

static int lua_lvgl_create_slider(lua_State *L)
{
    if (!s_lvgl_initialized) {
        return luaL_error(L, "LVGL is not initialized");
    }
    lv_obj_t *parent = opt_lvgl_obj(L, 1);
    chal_lvgl_lock();
    lv_obj_t *slider = lv_slider_create(parent);
    if (lua_gettop(L) >= 3) {
        int32_t min = (int32_t)luaL_checkinteger(L, 2);
        int32_t max = (int32_t)luaL_checkinteger(L, 3);
        lv_slider_set_range(slider, min, max);
    }
    if (lua_gettop(L) >= 4) {
        int32_t val = (int32_t)luaL_checkinteger(L, 4);
        lv_slider_set_value(slider, val, LV_ANIM_OFF);
    }
    chal_lvgl_unlock();
    push_lvgl_obj(L, slider);
    return 1;
}

static int lua_lvgl_create_arc(lua_State *L)
{
    if (!s_lvgl_initialized) {
        return luaL_error(L, "LVGL is not initialized");
    }
    lv_obj_t *parent = opt_lvgl_obj(L, 1);
    chal_lvgl_lock();
    lv_obj_t *arc = lv_arc_create(parent);
    if (lua_gettop(L) >= 3) {
        int32_t min = (int32_t)luaL_checkinteger(L, 2);
        int32_t max = (int32_t)luaL_checkinteger(L, 3);
        lv_arc_set_range(arc, min, max);
    }
    if (lua_gettop(L) >= 4) {
        int32_t val = (int32_t)luaL_checkinteger(L, 4);
        lv_arc_set_value(arc, val);
    }
    chal_lvgl_unlock();
    push_lvgl_obj(L, arc);
    return 1;
}

static int lua_lvgl_create_switch(lua_State *L)
{
    if (!s_lvgl_initialized) {
        return luaL_error(L, "LVGL is not initialized");
    }
    lv_obj_t *parent = opt_lvgl_obj(L, 1);
    bool state = false;
    if (lua_gettop(L) >= 2) {
        state = lua_toboolean(L, 2);
    }
    chal_lvgl_lock();
    lv_obj_t *sw = lv_switch_create(parent);
    if (state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    chal_lvgl_unlock();
    push_lvgl_obj(L, sw);
    return 1;
}

static int lua_lvgl_color(lua_State *L)
{
    if (lua_gettop(L) >= 3) {
        uint8_t r = (uint8_t)luaL_checkinteger(L, 1);
        uint8_t g = (uint8_t)luaL_checkinteger(L, 2);
        uint8_t b = (uint8_t)luaL_checkinteger(L, 3);
        uint32_t hex = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        lua_pushinteger(L, hex);
        return 1;
    }
    lua_settop(L, 1);
    return 1;
}

static const luaL_Reg lvgl_funcs[] = {
    {"init", lua_lvgl_init},
    {"deinit", lua_lvgl_deinit},
    {"status", lua_lvgl_status},
    {"step", lua_lvgl_step},
    {"scr_act", lua_lvgl_scr_act},
    {"clean", lua_lvgl_clean},
    {"obj", lua_lvgl_create_obj},
    {"label", lua_lvgl_create_label},
    {"btn", lua_lvgl_create_btn},
    {"bar", lua_lvgl_create_bar},
    {"slider", lua_lvgl_create_slider},
    {"arc", lua_lvgl_create_arc},
    {"switch", lua_lvgl_create_switch},
    {"color", lua_lvgl_color},
    {NULL, NULL}
};

int luaopen_lvgl(lua_State *L)
{
    register_lvgl_obj_meta(L);
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

void chal_lvgl_lock(void)
{
}

void chal_lvgl_unlock(void)
{
}

void chal_lvgl_teardown(void)
{
}

#endif // CONFIG_NANO_ENABLE_LVGL
