#include "chal_gpio.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_gpio";

/*
 * Safety guard for every pin mutation coming from a Lua script.
 *
 * gpio.mode() is the only entry point allowed to claim a pin, and it claims it
 * as LEDGER_OWNER_SCRIPT. Every other operation must therefore verify that the
 * pin is (a) a plausible GPIO number and (b) actually owned by the script.
 *
 * Without this check a script could skip the ledger entirely and drive a
 * reserved pad, e.g. gpio.write(26, 1) on an Octal-SPI Flash/PSRAM pin, which
 * is exactly the class of hardware damage the ledger exists to prevent.
 */
static void gpio_require_script_pin(lua_State *L, lua_Integer pin)
{
    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        luaL_error(L, "invalid GPIO pin %d", (int)pin);
    } else if (ledger_get_pin_owner((int)pin) != LEDGER_OWNER_SCRIPT) {
        luaL_error(L, "pin %d is not claimed by the script (call gpio.mode(pin, mode) first)", (int)pin);
    }
}

typedef struct {
    const char *name;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    ledger_pin_mode_t ledger_mode;
} gpio_mode_entry_t;

/*
 * Static table mapping user mode strings to C-HAL and ledger configuration.
 * INPUT_OUTPUT (rather than OUTPUT) keeps the input path enabled so
 * gpio_get_level() reports the real pad level on output pins.
 */
static const gpio_mode_entry_t GPIO_MODE_TABLE[] = {
    {"out",            GPIO_MODE_INPUT_OUTPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, LEDGER_PIN_MODE_GPIO_OUT},
    {"output",         GPIO_MODE_INPUT_OUTPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, LEDGER_PIN_MODE_GPIO_OUT},
    {"in",             GPIO_MODE_INPUT,        GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, LEDGER_PIN_MODE_GPIO_IN},
    {"input",          GPIO_MODE_INPUT,        GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, LEDGER_PIN_MODE_GPIO_IN},
    {"input_pullup",   GPIO_MODE_INPUT,        GPIO_PULLUP_ENABLE,  GPIO_PULLDOWN_DISABLE, LEDGER_PIN_MODE_GPIO_IN},
    {"in_pullup",      GPIO_MODE_INPUT,        GPIO_PULLUP_ENABLE,  GPIO_PULLDOWN_DISABLE, LEDGER_PIN_MODE_GPIO_IN},
    {"input_pulldown", GPIO_MODE_INPUT,        GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE,  LEDGER_PIN_MODE_GPIO_IN},
    {"in_pulldown",    GPIO_MODE_INPUT,        GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE,  LEDGER_PIN_MODE_GPIO_IN},
};

static const size_t GPIO_MODE_TABLE_SIZE = sizeof(GPIO_MODE_TABLE) / sizeof(GPIO_MODE_TABLE[0]);

static const gpio_mode_entry_t *find_gpio_mode_entry(const char *mode_str)
{
    if (mode_str == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < GPIO_MODE_TABLE_SIZE; i++) {
        if (strcmp(GPIO_MODE_TABLE[i].name, mode_str) == 0) {
            return &GPIO_MODE_TABLE[i];
        }
    }
    return NULL;
}

static int l_gpio_mode(lua_State *L)
{
    lua_Integer pin = luaL_checkinteger(L, 1);
    const char *mode_str = luaL_checkstring(L, 2);

    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        return luaL_error(L, "invalid GPIO pin %d", (int)pin);
    }

    const gpio_mode_entry_t *entry = find_gpio_mode_entry(mode_str);
    if (entry == NULL) {
        return luaL_error(L, "invalid mode '%s' (use 'out', 'in', 'input_pullup', 'input_pulldown')", mode_str);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = entry->mode,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_down_en = entry->pull_down_en,
        .pull_up_en = entry->pull_up_en,
    };

    /* Re-configuring a pin the script already owns is allowed: drop the old
     * claim first so a mode change does not fail as ESP_ERR_INVALID_STATE. */
    if (ledger_get_pin_owner((int)pin) == LEDGER_OWNER_SCRIPT) {
        ledger_release_pin((int)pin, LEDGER_OWNER_SCRIPT);
    }

    /* Safety check with Resource Ledger */
    esp_err_t err = ledger_claim_pin((int)pin, LEDGER_OWNER_SCRIPT, entry->ledger_mode, "lua_gpio");
    if (err != ESP_OK) {
        return luaL_error(L, "pin %d rejected by ledger (err %d: reserved by flash/psram/usb or already claimed)", (int)pin, (int)err);
    }

    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ledger_release_pin((int)pin, LEDGER_OWNER_SCRIPT);
        return luaL_error(L, "gpio_config failed for pin %d (err %d)", (int)pin, (int)err);
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

    gpio_require_script_pin(L, pin);

    esp_err_t err = gpio_set_level((gpio_num_t)pin, level);
    if (err != ESP_OK) {
        return luaL_error(L, "gpio_set_level failed for pin %d (err %d)", (int)pin, (int)err);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_gpio_read(lua_State *L)
{
    lua_Integer pin = luaL_checkinteger(L, 1);

    gpio_require_script_pin(L, pin);

    lua_pushinteger(L, gpio_get_level((gpio_num_t)pin));
    return 1;
}

static int l_gpio_toggle(lua_State *L)
{
    lua_Integer pin = luaL_checkinteger(L, 1);

    gpio_require_script_pin(L, pin);

    int new_level = gpio_get_level((gpio_num_t)pin) ? 0 : 1;
    esp_err_t err = gpio_set_level((gpio_num_t)pin, new_level);
    if (err != ESP_OK) {
        return luaL_error(L, "gpio_set_level failed for pin %d (err %d)", (int)pin, (int)err);
    }

    lua_pushinteger(L, new_level);
    return 1;
}

static int l_gpio_release(lua_State *L)
{
    lua_Integer pin = luaL_checkinteger(L, 1);

    gpio_require_script_pin(L, pin);

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
