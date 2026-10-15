#include "sandbox.h"
#include "esp_log.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static const char *TAG = "sandbox";

esp_err_t sandbox_init(void)
{
    ESP_LOGI(TAG, "Lua sandbox subsystem initialized (Lua %s)", LUA_VERSION);
    return ESP_OK;
}
