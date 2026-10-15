#include "chal_delay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "sandbox.h"
#include "lauxlib.h"

static int l_delay_ms(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms <= 0) {
        vTaskDelay(0);
        return 0;
    }

    int64_t remaining = ms;
    while (remaining > 0) {
        int64_t step = remaining > 50 ? 50 : remaining;
        TickType_t ticks = pdMS_TO_TICKS(step);
        vTaskDelay(ticks > 0 ? ticks : 1);
        remaining -= step;

        if (sandbox_is_abort_requested()) {
            return luaL_error(L, "interrupted: delay aborted by break signal");
        }
    }

    return 0;
}

static int l_delay_us(lua_State *L)
{
    lua_Integer us = luaL_checkinteger(L, 1);
    if (us > 0) {
        esp_rom_delay_us((uint32_t)us);
    }
    return 0;
}

static const luaL_Reg delay_funcs[] = {
    {"ms", l_delay_ms},
    {"us", l_delay_us},
    {NULL, NULL}
};

/*
 * `delay` is exposed as a module table so that both styles work:
 *   delay.ms(500)   -- explicit field access
 *   delay(500)      -- callable shortcut
 *
 * The __call metamethod receives the module table itself as argument #1, so
 * drop it before delegating, otherwise the millisecond value would land in
 * slot 2 and luaL_checkinteger(1) would see a table.
 */
static int l_delay_call(lua_State *L)
{
    lua_remove(L, 1);
    return l_delay_ms(L);
}

int luaopen_delay(lua_State *L)
{
    luaL_newlib(L, delay_funcs);

    /* Attach __call so `delay(ms)` behaves exactly like `delay.ms(ms)` */
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_delay_call);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);

    return 1;
}
