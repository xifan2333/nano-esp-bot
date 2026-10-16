#include "sandbox.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ledger.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static const char *TAG = "sandbox";

#define HOOK_STEP 1000
#define SANDBOX_MAX_MODULES 16

typedef struct {
    char name[32];
    lua_CFunction open_fn;
} sandbox_module_t;

typedef struct {
    size_t mem_used;
    size_t mem_peak;
    size_t mem_limit;
} sandbox_mem_mgr_t;

static SemaphoreHandle_t s_sandbox_mutex = NULL;
static lua_State *s_L = NULL;
static sandbox_mem_mgr_t s_mem_mgr = {0, 0, SANDBOX_DEFAULT_MEM_LIMIT};
static volatile bool s_abort_requested = false;
static uint32_t s_insn_count = 0;
static uint32_t s_insn_quota = SANDBOX_DEFAULT_INSN_QUOTA;

static sandbox_module_t s_modules[SANDBOX_MAX_MODULES];
static size_t s_module_count = 0;

static void sandbox_apply_modules(lua_State *L)
{
    for (size_t i = 0; i < s_module_count; i++) {
        luaL_requiref(L, s_modules[i].name, s_modules[i].open_fn, 1);
        lua_pop(L, 1);
    }
}

static void *sandbox_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    sandbox_mem_mgr_t *mgr = (sandbox_mem_mgr_t *)ud;
    if (nsize == 0) {
        if (ptr != NULL) {
            free(ptr);
            if (mgr->mem_used >= osize) {
                mgr->mem_used -= osize;
            } else {
                mgr->mem_used = 0;
            }
        }
        return NULL;
    }

    size_t old_size = (ptr != NULL) ? osize : 0;
    size_t new_used = mgr->mem_used + nsize - old_size;
    if (new_used > mgr->mem_limit) {
        ESP_LOGW(TAG, "Sandbox memory limit reached: %zu > %zu bytes", new_used, mgr->mem_limit);
        return NULL;
    }

    void *new_ptr = realloc(ptr, nsize);
    if (new_ptr == NULL) {
        ESP_LOGE(TAG, "System realloc failed for %zu bytes", nsize);
        return NULL;
    }

    mgr->mem_used = new_used;
    if (mgr->mem_used > mgr->mem_peak) {
        mgr->mem_peak = mgr->mem_used;
    }
    return new_ptr;
}

static void sandbox_quota_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (s_abort_requested) {
        s_abort_requested = false;
        luaL_error(L, "interrupted: execution terminated by break signal");
    }

    s_insn_count += HOOK_STEP;
    if (s_insn_quota > 0 && s_insn_count >= s_insn_quota) {
        /* NOTE: luaL_error() uses Lua's own formatter, which supports only
         * %s %c %d %I %f %p %U %%. C's %lu / %x are NOT supported and would
         * surface as "invalid option ... to 'lua_pushfstring'". */
        luaL_error(L, "quota exceeded: instruction limit of %d reached", (int)s_insn_quota);
    }

    vTaskDelay(0);
}

static esp_err_t sandbox_reset_internal(void)
{
    if (s_L != NULL) {
        lua_close(s_L);
        s_L = NULL;
    }

    s_mem_mgr.mem_used = 0;
    s_mem_mgr.mem_peak = 0;
    s_mem_mgr.mem_limit = SANDBOX_DEFAULT_MEM_LIMIT;
    s_abort_requested = false;
    s_insn_count = 0;

    s_L = lua_newstate(sandbox_lua_alloc, &s_mem_mgr);
    if (s_L == NULL) {
        ESP_LOGE(TAG, "Failed to create sandboxed Lua state");
        return ESP_ERR_NO_MEM;
    }

    /* Open full standard libraries (base, package, coroutine, table, io, os, string, math, utf8, debug) */
    luaL_openlibs(s_L);

    /* Preload and register external C-HAL modules */
    sandbox_apply_modules(s_L);

    /* Configure instruction quota hook */
    lua_sethook(s_L, sandbox_quota_hook, LUA_MASKCOUNT, HOOK_STEP);

    ESP_LOGI(TAG, "Sandboxed Lua state initialized (limit: %zu bytes)", s_mem_mgr.mem_limit);
    return ESP_OK;
}

esp_err_t sandbox_init(void)
{
    if (s_sandbox_mutex == NULL) {
        s_sandbox_mutex = xSemaphoreCreateMutex();
        if (s_sandbox_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_sandbox_mutex, portMAX_DELAY);
    esp_err_t err = sandbox_reset_internal();
    xSemaphoreGive(s_sandbox_mutex);

    return err;
}

esp_err_t sandbox_reset(void)
{
    if (s_sandbox_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_sandbox_mutex, portMAX_DELAY);
    esp_err_t err = sandbox_reset_internal();
    xSemaphoreGive(s_sandbox_mutex);

    return err;
}

esp_err_t sandbox_break(void)
{
    s_abort_requested = true;
    if (s_L != NULL) {
        lua_sethook(s_L, sandbox_quota_hook, LUA_MASKCOUNT | LUA_MASKLINE | LUA_MASKCALL | LUA_MASKRET, 1);
    }
    ledger_teardown(LEDGER_OWNER_SCRIPT);
    ESP_LOGI(TAG, "Break requested: abort flag set and script ledger torn down");
    return ESP_OK;
}

bool sandbox_is_abort_requested(void)
{
    return s_abort_requested;
}

esp_err_t sandbox_register_module(const char *name, lua_CFunction open_fn)
{
    if (name == NULL || open_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sandbox_mutex != NULL) {
        xSemaphoreTake(s_sandbox_mutex, portMAX_DELAY);
    }

    /* Check if already registered */
    for (size_t i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].name, name) == 0) {
            s_modules[i].open_fn = open_fn;
            if (s_L != NULL) {
                luaL_requiref(s_L, name, open_fn, 1);
                lua_pop(s_L, 1);
            }
            if (s_sandbox_mutex != NULL) {
                xSemaphoreGive(s_sandbox_mutex);
            }
            return ESP_OK;
        }
    }

    if (s_module_count >= SANDBOX_MAX_MODULES) {
        if (s_sandbox_mutex != NULL) {
            xSemaphoreGive(s_sandbox_mutex);
        }
        ESP_LOGE(TAG, "Module registry full (max %d)", SANDBOX_MAX_MODULES);
        return ESP_ERR_NO_MEM;
    }

    strncpy(s_modules[s_module_count].name, name, sizeof(s_modules[s_module_count].name) - 1);
    s_modules[s_module_count].name[sizeof(s_modules[s_module_count].name) - 1] = '\0';
    s_modules[s_module_count].open_fn = open_fn;
    s_module_count++;

    if (s_L != NULL) {
        luaL_requiref(s_L, name, open_fn, 1);
        lua_pop(s_L, 1);
    }

    if (s_sandbox_mutex != NULL) {
        xSemaphoreGive(s_sandbox_mutex);
    }

    ESP_LOGI(TAG, "Registered Lua module: '%s'", name);
    return ESP_OK;
}

void sandbox_get_mem_stats(size_t *out_used, size_t *out_peak, size_t *out_limit)
{
    if (out_used != NULL) {
        *out_used = s_mem_mgr.mem_used;
    }
    if (out_peak != NULL) {
        *out_peak = s_mem_mgr.mem_peak;
    }
    if (out_limit != NULL) {
        *out_limit = s_mem_mgr.mem_limit;
    }
}

esp_err_t sandbox_eval(const char *code, sandbox_result_t *out_result)
{
    if (code == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_result, 0, sizeof(*out_result));

    if (s_sandbox_mutex == NULL || s_L == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_sandbox_mutex, portMAX_DELAY);

    /* Reset per-eval instruction counter and abort flag */
    s_insn_count = 0;
    s_abort_requested = false;
    lua_sethook(s_L, sandbox_quota_hook, LUA_MASKCOUNT, HOOK_STEP);

    /* Clean stack */
    lua_settop(s_L, 0);

    size_t code_len = strlen(code);
    int load_status = LUA_OK;

    /* Strategy 1: Try compiling as expression "return <code>;" */
    char expr_buf[1024];
    if (code_len < sizeof(expr_buf) - 16) {
        int expr_len = snprintf(expr_buf, sizeof(expr_buf), "return %s;", code);
        load_status = luaL_loadbuffer(s_L, expr_buf, (size_t)expr_len, "=eval");
    } else {
        load_status = LUA_ERRSYNTAX;
    }

    /* Strategy 2: If expression fails with syntax error, compile as statement */
    if (load_status != LUA_OK) {
        lua_pop(s_L, 1); /* Remove error string from stack */
        load_status = luaL_loadbuffer(s_L, code, code_len, "=eval");
    }

    if (load_status != LUA_OK) {
        const char *compile_err = lua_tostring(s_L, -1);
        snprintf(out_result->output, sizeof(out_result->output), "%s",
                 compile_err ? compile_err : "syntax error");
        out_result->success = false;
        lua_pop(s_L, 1);
        out_result->mem_used = s_mem_mgr.mem_used;
        out_result->mem_peak = s_mem_mgr.mem_peak;
        out_result->insn_count = s_insn_count;
        xSemaphoreGive(s_sandbox_mutex);
        return ESP_OK;
    }

    /* Execute compiled chunk */
    int pcall_status = lua_pcall(s_L, 0, LUA_MULTRET, 0);

    if (pcall_status == LUA_OK) {
        out_result->success = true;
        int nresults = lua_gettop(s_L);
        if (nresults == 0) {
            snprintf(out_result->output, sizeof(out_result->output), "nil");
        } else {
            size_t offset = 0;
            for (int i = 1; i <= nresults; i++) {
                const char *str = NULL;
                char temp_buf[32];
                int type = lua_type(s_L, i);
                switch (type) {
                    case LUA_TNIL:
                        str = "nil";
                        break;
                    case LUA_TBOOLEAN:
                        str = lua_toboolean(s_L, i) ? "true" : "false";
                        break;
                    case LUA_TNUMBER:
                        if (lua_isinteger(s_L, i)) {
                            snprintf(temp_buf, sizeof(temp_buf), "%lld", (long long)lua_tointeger(s_L, i));
                        } else {
                            snprintf(temp_buf, sizeof(temp_buf), "%f", lua_tonumber(s_L, i));
                        }
                        str = temp_buf;
                        break;
                    case LUA_TSTRING:
                        str = lua_tostring(s_L, i);
                        break;
                    default:
                        snprintf(temp_buf, sizeof(temp_buf), "<%s>", lua_typename(s_L, type));
                        str = temp_buf;
                        break;
                }
                if (i > 1 && offset < sizeof(out_result->output) - 1) {
                    out_result->output[offset++] = '\t';
                }
                size_t len = strlen(str);
                if (offset + len >= sizeof(out_result->output)) {
                    len = sizeof(out_result->output) - 1 - offset;
                }
                memcpy(out_result->output + offset, str, len);
                offset += len;
                out_result->output[offset] = '\0';
            }
        }
    } else {
        out_result->success = false;
        const char *runtime_err = lua_tostring(s_L, -1);
        snprintf(out_result->output, sizeof(out_result->output), "%s",
                 runtime_err ? runtime_err : "runtime error");
    }

    out_result->mem_used = s_mem_mgr.mem_used;
    out_result->mem_peak = s_mem_mgr.mem_peak;
    out_result->insn_count = s_insn_count;

    /* Clean stack */
    lua_settop(s_L, 0);

    xSemaphoreGive(s_sandbox_mutex);
    return ESP_OK;
}

esp_err_t sandbox_eval_file(const char *filepath, sandbox_result_t *out_result)
{
    if (filepath == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_result, 0, sizeof(*out_result));

    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long fsize = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    if (fsize < 0 || fsize > 64 * 1024) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *code = malloc((size_t)fsize + 1);
    if (code == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(code, 1, (size_t)fsize, f);
    code[read_bytes] = '\0';
    fclose(f);

    esp_err_t err = sandbox_eval(code, out_result);
    free(code);
    return err;
}
