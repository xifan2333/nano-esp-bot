#ifndef CHAL_DELAY_H
#define CHAL_DELAY_H

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Lua module opener for 'delay' (for require("delay")).
 */
int luaopen_delay(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* CHAL_DELAY_H */
