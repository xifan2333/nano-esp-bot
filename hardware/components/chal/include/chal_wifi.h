#pragma once

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_wifi(lua_State *L);

void chal_wifi_teardown(void);

#ifdef __cplusplus
}
#endif
