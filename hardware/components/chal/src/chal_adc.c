#include "chal_adc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_adc";

#define ADC_MAX_PINS 10

typedef struct {
    bool active;
    int gpio_num;
    adc_unit_t unit;
    adc_channel_t channel;
    adc_atten_t atten;
    adc_cali_handle_t cali_handle;
    bool calibrated;
} chal_adc_slot_t;

static SemaphoreHandle_t s_adc_mutex = NULL;
static adc_oneshot_unit_handle_t s_unit1_handle = NULL;
static adc_oneshot_unit_handle_t s_unit2_handle = NULL;
static int s_unit1_refcount = 0;
static int s_unit2_refcount = 0;
static chal_adc_slot_t s_slots[ADC_MAX_PINS];

static int find_slot_by_gpio(int gpio_num)
{
    for (int i = 0; i < ADC_MAX_PINS; i++) {
        if (s_slots[i].active && s_slots[i].gpio_num == gpio_num) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void)
{
    for (int i = 0; i < ADC_MAX_PINS; i++) {
        if (!s_slots[i].active) {
            return i;
        }
    }
    return -1;
}

static adc_oneshot_unit_handle_t get_or_create_unit_locked(adc_unit_t unit)
{
    if (unit == ADC_UNIT_1) {
        if (!s_unit1_handle) {
            adc_oneshot_unit_init_cfg_t init_cfg = {
                .unit_id = ADC_UNIT_1,
                .ulp_mode = ADC_ULP_MODE_DISABLE,
            };
            esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_unit1_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create ADC1 unit: %s", esp_err_to_name(err));
                return NULL;
            }
        }
        s_unit1_refcount++;
        return s_unit1_handle;
    } else {
        if (!s_unit2_handle) {
            adc_oneshot_unit_init_cfg_t init_cfg = {
                .unit_id = ADC_UNIT_2,
                .ulp_mode = ADC_ULP_MODE_DISABLE,
            };
            esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_unit2_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create ADC2 unit: %s", esp_err_to_name(err));
                return NULL;
            }
        }
        s_unit2_refcount++;
        return s_unit2_handle;
    }
}

static void release_unit_locked(adc_unit_t unit)
{
    if (unit == ADC_UNIT_1) {
        s_unit1_refcount--;
        if (s_unit1_refcount <= 0) {
            if (s_unit1_handle) {
                adc_oneshot_del_unit(s_unit1_handle);
                s_unit1_handle = NULL;
            }
            s_unit1_refcount = 0;
        }
    } else {
        s_unit2_refcount--;
        if (s_unit2_refcount <= 0) {
            if (s_unit2_handle) {
                adc_oneshot_del_unit(s_unit2_handle);
                s_unit2_handle = NULL;
            }
            s_unit2_refcount = 0;
        }
    }
}

static void delete_cali_handle(adc_cali_handle_t handle)
{
    if (!handle) {
        return;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(handle);
#endif
}

static bool create_cali_handle(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &handle) == ESP_OK) {
        calibrated = true;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    (void)channel;
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &handle) == ESP_OK) {
        calibrated = true;
    }
#else
    (void)unit;
    (void)channel;
    (void)atten;
#endif

    *out_handle = handle;
    return calibrated;
}

static uint32_t atten_max_mv(adc_atten_t atten)
{
    switch (atten) {
    case ADC_ATTEN_DB_0:   return 950;
    case ADC_ATTEN_DB_2_5: return 1250;
    case ADC_ATTEN_DB_6:   return 1750;
    case ADC_ATTEN_DB_12:  return 3100;
    default:               return 3100;
    }
}

esp_err_t chal_adc_init_pin(int gpio_num, int atten)
{
    if (!ledger_is_pin_safe(gpio_num)) {
        ESP_LOGE(TAG, "GPIO %d is not safe for ADC", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(gpio_num, &unit, &channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d does not support ADC: %s", gpio_num, esp_err_to_name(err));
        return err;
    }

    adc_atten_t adc_atten = (adc_atten_t)atten;

    if (!s_adc_mutex) {
        s_adc_mutex = xSemaphoreCreateMutex();
        if (!s_adc_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);
    int slot_idx = find_slot_by_gpio(gpio_num);
    if (slot_idx >= 0) {
        // Reconfigure attenuation if changed
        if (s_slots[slot_idx].atten != adc_atten) {
            adc_oneshot_unit_handle_t u_handle = (s_slots[slot_idx].unit == ADC_UNIT_1) ? s_unit1_handle : s_unit2_handle;
            adc_oneshot_chan_cfg_t chan_cfg = {
                .atten = adc_atten,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            err = adc_oneshot_config_channel(u_handle, channel, &chan_cfg);
            if (err != ESP_OK) {
                xSemaphoreGive(s_adc_mutex);
                return err;
            }
            if (s_slots[slot_idx].cali_handle) {
                delete_cali_handle(s_slots[slot_idx].cali_handle);
                s_slots[slot_idx].cali_handle = NULL;
                s_slots[slot_idx].calibrated = false;
            }
            s_slots[slot_idx].atten = adc_atten;
            s_slots[slot_idx].calibrated = create_cali_handle(unit, channel, adc_atten, &s_slots[slot_idx].cali_handle);
        }
        xSemaphoreGive(s_adc_mutex);
        return ESP_OK;
    }

    slot_idx = find_free_slot();
    if (slot_idx < 0) {
        xSemaphoreGive(s_adc_mutex);
        ESP_LOGE(TAG, "No free ADC slots available (max %d)", ADC_MAX_PINS);
        return ESP_ERR_NO_MEM;
    }

    err = ledger_claim_pin(gpio_num, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_ADC, "adc");
    if (err != ESP_OK) {
        xSemaphoreGive(s_adc_mutex);
        ESP_LOGE(TAG, "Ledger failed to claim GPIO %d for ADC: %s", gpio_num, esp_err_to_name(err));
        return err;
    }

    adc_oneshot_unit_handle_t u_handle = get_or_create_unit_locked(unit);
    if (!u_handle) {
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_adc_mutex);
        return ESP_ERR_NO_MEM;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = adc_atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(u_handle, channel, &chan_cfg);
    if (err != ESP_OK) {
        release_unit_locked(unit);
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_adc_mutex);
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_cali_handle_t cali_handle = NULL;
    bool cal_ok = create_cali_handle(unit, channel, adc_atten, &cali_handle);

    s_slots[slot_idx].active = true;
    s_slots[slot_idx].gpio_num = gpio_num;
    s_slots[slot_idx].unit = unit;
    s_slots[slot_idx].channel = channel;
    s_slots[slot_idx].atten = adc_atten;
    s_slots[slot_idx].cali_handle = cali_handle;
    s_slots[slot_idx].calibrated = cal_ok;

    xSemaphoreGive(s_adc_mutex);
    ESP_LOGI(TAG, "ADC configured on GPIO %d (unit %d, chan %d, atten %d, calibrated: %s)",
             gpio_num, (int)unit + 1, (int)channel, (int)adc_atten, cal_ok ? "yes" : "no");
    return ESP_OK;
}

esp_err_t chal_adc_deinit_pin(int gpio_num)
{
    if (!s_adc_mutex) {
        return ESP_OK;
    }

    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);
    int slot_idx = find_slot_by_gpio(gpio_num);
    if (slot_idx < 0) {
        xSemaphoreGive(s_adc_mutex);
        return ESP_OK;
    }

    if (s_slots[slot_idx].cali_handle) {
        delete_cali_handle(s_slots[slot_idx].cali_handle);
        s_slots[slot_idx].cali_handle = NULL;
    }

    release_unit_locked(s_slots[slot_idx].unit);
    ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
    memset(&s_slots[slot_idx], 0, sizeof(chal_adc_slot_t));

    xSemaphoreGive(s_adc_mutex);
    ESP_LOGI(TAG, "ADC deinitialized for GPIO %d", gpio_num);
    return ESP_OK;
}

bool chal_adc_is_pin_configured(int gpio_num)
{
    return find_slot_by_gpio(gpio_num) >= 0;
}

esp_err_t chal_adc_read(int gpio_num, int *out_raw, int *out_mv)
{
    if (!out_raw && !out_mv) {
        return ESP_ERR_INVALID_ARG;
    }

    // Auto-init with default 12dB attenuation if not configured
    if (!chal_adc_is_pin_configured(gpio_num)) {
        esp_err_t err = chal_adc_init_pin(gpio_num, ADC_ATTEN_DB_12);
        if (err != ESP_OK) {
            return err;
        }
    }

    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);
    int slot_idx = find_slot_by_gpio(gpio_num);
    if (slot_idx < 0) {
        xSemaphoreGive(s_adc_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    adc_oneshot_unit_handle_t u_handle = (s_slots[slot_idx].unit == ADC_UNIT_1) ? s_unit1_handle : s_unit2_handle;
    if (!u_handle) {
        xSemaphoreGive(s_adc_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    int raw_val = 0;
    esp_err_t err = adc_oneshot_read(u_handle, s_slots[slot_idx].channel, &raw_val);
    if (err != ESP_OK) {
        xSemaphoreGive(s_adc_mutex);
        ESP_LOGE(TAG, "adc_oneshot_read failed for GPIO %d: %s", gpio_num, esp_err_to_name(err));
        return err;
    }

    if (out_raw) {
        *out_raw = raw_val;
    }

    if (out_mv) {
        if (s_slots[slot_idx].calibrated && s_slots[slot_idx].cali_handle) {
            err = adc_cali_raw_to_voltage(s_slots[slot_idx].cali_handle, raw_val, out_mv);
            if (err != ESP_OK) {
                // Fallback to proportional calculation
                *out_mv = (int)((raw_val * atten_max_mv(s_slots[slot_idx].atten)) / 4095);
            }
        } else {
            *out_mv = (int)((raw_val * atten_max_mv(s_slots[slot_idx].atten)) / 4095);
        }
    }

    xSemaphoreGive(s_adc_mutex);
    return ESP_OK;
}

esp_err_t chal_adc_read_raw(int gpio_num, int *out_raw)
{
    return chal_adc_read(gpio_num, out_raw, NULL);
}

esp_err_t chal_adc_read_mv(int gpio_num, int *out_mv)
{
    return chal_adc_read(gpio_num, NULL, out_mv);
}

void chal_adc_teardown(void)
{
    if (!s_adc_mutex) {
        return;
    }

    if (xSemaphoreTake(s_adc_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < ADC_MAX_PINS; i++) {
            if (s_slots[i].active) {
                if (s_slots[i].cali_handle) {
                    delete_cali_handle(s_slots[i].cali_handle);
                    s_slots[i].cali_handle = NULL;
                }
                release_unit_locked(s_slots[i].unit);
                ledger_release_pin(s_slots[i].gpio_num, LEDGER_OWNER_SCRIPT);
                memset(&s_slots[i], 0, sizeof(chal_adc_slot_t));
            }
        }
        xSemaphoreGive(s_adc_mutex);
    }
}

static adc_atten_t parse_atten(lua_State *L, int arg_idx)
{
    if (lua_isnoneornil(L, arg_idx)) {
        return ADC_ATTEN_DB_12;
    }
    if (lua_isinteger(L, arg_idx)) {
        int val = (int)lua_tointeger(L, arg_idx);
        if (val == 0) return ADC_ATTEN_DB_0;
        if (val == 2 || val == 3) return ADC_ATTEN_DB_2_5;
        if (val == 6) return ADC_ATTEN_DB_6;
        if (val == 11 || val == 12) return ADC_ATTEN_DB_12;
    } else if (lua_isstring(L, arg_idx)) {
        const char *s = lua_tostring(L, arg_idx);
        if (strcmp(s, "0db") == 0 || strcmp(s, "0") == 0) return ADC_ATTEN_DB_0;
        if (strcmp(s, "2.5db") == 0 || strcmp(s, "2.5") == 0) return ADC_ATTEN_DB_2_5;
        if (strcmp(s, "6db") == 0 || strcmp(s, "6") == 0) return ADC_ATTEN_DB_6;
        if (strcmp(s, "12db") == 0 || strcmp(s, "11db") == 0 || strcmp(s, "12") == 0) return ADC_ATTEN_DB_12;
    }
    return ADC_ATTEN_DB_12;
}

static const char *atten_to_string(adc_atten_t atten)
{
    switch (atten) {
    case ADC_ATTEN_DB_0:   return "0dB";
    case ADC_ATTEN_DB_2_5: return "2.5dB";
    case ADC_ATTEN_DB_6:   return "6dB";
    case ADC_ATTEN_DB_12:  return "12dB";
    default:               return "unknown";
    }
}

static int lua_adc_init(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    adc_atten_t atten = parse_atten(L, 2);

    esp_err_t err = chal_adc_init_pin(pin, atten);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_adc_deinit(lua_State *L)
{
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        int pin = (int)luaL_checkinteger(L, 1);
        chal_adc_deinit_pin(pin);
    } else {
        chal_adc_teardown();
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_adc_read(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        adc_atten_t atten = parse_atten(L, 2);
        chal_adc_init_pin(pin, atten);
    }

    int raw = 0, mv = 0;
    esp_err_t err = chal_adc_read(pin, &raw, &mv);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    lua_pushinteger(L, raw);
    lua_pushinteger(L, mv);
    return 2;
}

static int lua_adc_read_raw(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        adc_atten_t atten = parse_atten(L, 2);
        chal_adc_init_pin(pin, atten);
    }

    int raw = 0;
    esp_err_t err = chal_adc_read_raw(pin, &raw);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    lua_pushinteger(L, raw);
    return 1;
}

static int lua_adc_read_mv(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        adc_atten_t atten = parse_atten(L, 2);
        chal_adc_init_pin(pin, atten);
    }

    int mv = 0;
    esp_err_t err = chal_adc_read_mv(pin, &mv);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    lua_pushinteger(L, mv);
    return 1;
}

static int lua_adc_status(lua_State *L)
{
    lua_newtable(L);

    if (s_adc_mutex && xSemaphoreTake(s_adc_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < ADC_MAX_PINS; i++) {
            if (s_slots[i].active) {
                lua_pushinteger(L, s_slots[i].gpio_num);
                lua_newtable(L);
                lua_pushinteger(L, (int)s_slots[i].unit + 1);
                lua_setfield(L, -2, "unit");
                lua_pushinteger(L, (int)s_slots[i].channel);
                lua_setfield(L, -2, "channel");
                lua_pushstring(L, atten_to_string(s_slots[i].atten));
                lua_setfield(L, -2, "atten");
                lua_pushboolean(L, s_slots[i].calibrated);
                lua_setfield(L, -2, "calibrated");
                lua_settable(L, -3);
            }
        }
        xSemaphoreGive(s_adc_mutex);
    }

    return 1;
}

static const luaL_Reg adc_funcs[] = {
    {"init", lua_adc_init},
    {"deinit", lua_adc_deinit},
    {"read", lua_adc_read},
    {"read_raw", lua_adc_read_raw},
    {"read_mv", lua_adc_read_mv},
    {"status", lua_adc_status},
    {NULL, NULL}
};

int luaopen_adc(lua_State *L)
{
    luaL_newlib(L, adc_funcs);
    return 1;
}
