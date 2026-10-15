#include "chal_gpio.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_gpio";

static int l_gpio_mode(lua_State *L)
{
    lua_Integer pin = luaL_checkinteger(L, 1);
    const char *mode_str = luaL_checkstring(L, 2);

    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        return luaL_error(L, "invalid GPIO pin %d", (int)pin);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .intr_type = GPIO_INTR_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };

    ledger_pin_mode_t lmode;
    if (strcmp(mode_str, "out") == 0 || strcmp(mode_str, "output") == 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        lmode = LEDGER_PIN_MODE_GPIO_OUT;
    } else if (strcmp(mode_str, "in") == 0 || strcmp(mode_str, "input") == 0) {
        io_conf.mode = GPIO_MODE_INPUT;
        lmode = LEDGER_PIN_MODE_GPIO_IN;
    } else if (strcmp(mode_str, "input_pullup") == 0 || strcmp(mode_str, "in_pullup") == 0) {
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        lmode = LEDGER_PIN_MODE_GPIO_IN;
    } else if (strcmp(mode_str, "input_pulldown") == 0 || strcmp(mode_str, "in_pulldown") == 0) {
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        lmode = LEDGER_PIN_MODE_GPIO_IN;
    } else {
        return luaL_error(L, "invalid mode '%s' (use 'out', 'in', 'input_pullup', 'input_pulldown')", mode_str);
    }

    /* Safety check with Resource Ledger */
    esp_err_t err = ledger_claim_pin((int)pin, LEDGER_OWNER_SCRIPT, lmode, "lua_gpio");
    if (err != ESP_OK) {
        return luaL_error(L, "pin %d rejected by ledger (err 0x%x - pin may be reserved by flash/psram/usb or already claimed)", (int)pin, err);
    }

    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ledger_release_pin((int)pin, LEDGER_OWNER_SCRIPT);
        return luaL_error(L, "gpio_config failed for pin %d (0x%x)", (int)pin, err);
    }

    ESP_LOGD(TAG, "Configured GPIO %d as %s", (int)pin, mode_str);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_gpio_write(lua_State *L)
{
    lua_Integer pin = luaL_checkinteger(L, 1);
    int level = 0;
    if (lua_isboolean(L, 2)) {
        level = lua_toboolean(L, 2);
    } else {
        level = (int)luaL_checkinteger(L, 2) ? 1 : 0;
    }

    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        return luaL_error(L, "invalid GPIO pin %d", (int)pin);
    }

    esp_err_t err = gpio_set_level((gpio_num_t)pin, level);
    if (err != ESP_OK) {
        return luaL_error(L, "gpio_set_level failed for pin %d (0x%x)", (int)pin, err);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_gpio_read(lua_State *L)
{
    lua_Integer pin = luaL_checkinteger(L, 1);
    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        return luaL_error(L, "invalid GPIO pin %d", (int)pin);
    }

    int level = gpio_get_level((gpio_num_t)pin);
    lua_pushinteger(L, level);
    return 1;
}

static int l_gpio_toggle(lua_State *L)
{
    lua_Integer pin = luaL_checkinteger(L, 1);
    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        return luaL_error(L, "invalid GPIO pin %d", (int)pin);
    }

    int current = gpio_get_level((gpio_num_t)pin);
    int new_level = current ? 0 : 1;
    esp_err_t err = gpio_set_level((gpio_num_t)pin, new_level);
    if (err != ESP_OK) {
        return luaL_error(L, "gpio_set_level failed for pin %d (0x%x)", (int)pin, err);
    }

    lua_pushinteger(L, new_level);
    return 1;
}

static int l_gpio_release(lua_State *L)
{
    lua_Integer pin = luaL_checkinteger(L, 1);
    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        return luaL_error(L, "invalid GPIO pin %d", (int)pin);
    }

    esp_err_t err = ledger_release_pin((int)pin, LEDGER_OWNER_SCRIPT);
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

static const luaL_Reg gpio_funcs[] = {
    {"mode", l_gpio_mode},
    {"set_direction", l_gpio_mode},
    {"write", l_gpio_write},
    {"set_level", l_gpio_write},
    {"read", l_gpio_read},
    {"get_level", l_gpio_read},
    {"toggle", l_gpio_toggle},
    {"release", l_gpio_release},
    {NULL, NULL}
};

int luaopen_gpio(lua_State *L)
{
    luaL_newlib(L, gpio_funcs);
    return 1;
}
