#ifndef CHAL_GPIO_H
#define CHAL_GPIO_H

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Lua module opener for 'gpio' (for require("gpio")).
 */
int luaopen_gpio(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* CHAL_GPIO_H */
