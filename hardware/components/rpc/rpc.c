#include "rpc.h"
#include "ledger.h"
#include "sandbox.h"
#include "storage.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "rpc";

#define RPC_LINE_BUF_SIZE       2048
#define STORAGE_READ_MAX_BYTES  (16 * 1024)

/*
 * The control plane task must be sized for the deepest handler it can run.
 * RPC method handlers execute *inline* on this task, and `exec` runs the
 * full Lua pipeline here.
 *
 * Stack high-water telemetry is exposed through info -> rpc_stack_headroom so
 * the margin can be verified on real hardware instead of guessed.
 */
#define RPC_TASK_STACK_SIZE  16384
#define RPC_TASK_PRIORITY    10

static rpc_break_callback_t s_break_cb = NULL;
static bool s_initialized = false;
static TaskHandle_t s_rpc_task_handle = NULL;

static void rpc_send_raw(const char *str)
{
    if (!str) {
        return;
    }
    size_t len = strlen(str);
    usb_serial_jtag_write_bytes(str, len, pdMS_TO_TICKS(100));
    usb_serial_jtag_write_bytes("\n", 1, pdMS_TO_TICKS(100));
}

static void rpc_send_error_raw(const cJSON *id, int code, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return;
    }
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }

    cJSON *err_obj = cJSON_CreateObject();
    if (err_obj) {
        cJSON_AddNumberToObject(err_obj, "code", code);
        cJSON_AddStringToObject(err_obj, "message", message ? message : "Error");
        cJSON_AddItemToObject(resp, "error", err_obj);
    }

    char *rendered = cJSON_PrintUnformatted(resp);
    if (rendered) {
        rpc_send_raw(rendered);
        free(rendered);
    }
    cJSON_Delete(resp);
}

/**
 * @brief Remaining stack headroom of the control-plane task, in bytes.
 *
 * Only meaningful once the task has started running; returns 0 before that.
 * Exposed through info -> rpc_stack_headroom so the real margin can be checked on hardware.
 */
static uint32_t rpc_get_task_stack_headroom(void)
{
    if (s_rpc_task_handle == NULL) {
        return 0;
    }
    return (uint32_t)uxTaskGetStackHighWaterMark(s_rpc_task_handle);
}

/**
 * @brief Generic parameter extraction helper.
 *
 * Supports:
 * - Bare string: "str" (when index == 0)
 * - Array element at index: ["str", ...]
 * - Object field with key: {"key": "str", ...}
 *
 * @param params cJSON params pointer (may be NULL).
 * @param index 0-based index if params is an array.
 * @param key Object key name if params is an object.
 * @return const char* string value, or NULL if absent or incorrect type.
 */
static const char *rpc_param_get_string(const cJSON *params, int index, const char *key)
{
    if (params == NULL) {
        return NULL;
    }
    if (cJSON_IsString(params)) {
        return (index == 0) ? params->valuestring : NULL;
    }
    if (cJSON_IsArray(params)) {
        const cJSON *item = cJSON_GetArrayItem(params, index);
        if (item != NULL && cJSON_IsString(item)) {
            return item->valuestring;
        }
    }
    if (cJSON_IsObject(params) && key != NULL) {
        const cJSON *item = cJSON_GetObjectItem(params, key);
        if (item != NULL && cJSON_IsString(item)) {
            return item->valuestring;
        }
    }
    return NULL;
}

static cJSON *handle_primitive_ping(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    (void)params;
    (void)error_code;
    (void)error_msg;
    (void)error_msg_len;

    cJSON *res = cJSON_CreateObject();
    if (!res) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Out of memory");
        return NULL;
    }
    cJSON_AddBoolToObject(res, "pong", true);
    cJSON_AddNumberToObject(res, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    return res;
}

static cJSON *handle_primitive_info(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    (void)params;
    (void)error_code;
    (void)error_msg;
    (void)error_msg_len;

    cJSON *res = cJSON_CreateObject();
    if (!res) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Out of memory");
        return NULL;
    }

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    cJSON_AddStringToObject(res, "target", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(res, "cores", chip_info.cores);
    cJSON_AddNumberToObject(res, "revision", chip_info.revision);
    cJSON_AddNumberToObject(res, "flash_size_mb", (double)(flash_size / (1024 * 1024)));
    cJSON_AddNumberToObject(res, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(res, "min_free_heap", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(res, "claimed_pins", (double)ledger_get_claimed_count(LEDGER_OWNER_NONE));
    cJSON_AddNumberToObject(res, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(res, "rpc_stack_total", (double)RPC_TASK_STACK_SIZE);
    cJSON_AddNumberToObject(res, "rpc_stack_headroom", (double)rpc_get_task_stack_headroom());

    cJSON *storage_obj = cJSON_AddObjectToObject(res, "storage");
    if (storage_obj) {
        bool mounted = storage_is_mounted();
        cJSON_AddBoolToObject(storage_obj, "mounted", mounted);
        cJSON_AddStringToObject(storage_obj, "base_path", STORAGE_BASE_PATH);
        if (mounted) {
            size_t total = 0, used = 0;
            if (storage_get_info(&total, &used) == ESP_OK) {
                cJSON_AddNumberToObject(storage_obj, "total_bytes", (double)total);
                cJSON_AddNumberToObject(storage_obj, "used_bytes", (double)used);
                cJSON_AddNumberToObject(storage_obj, "free_bytes", (double)(total >= used ? total - used : 0));
            }
        }
    }

    cJSON *sandbox_obj = cJSON_AddObjectToObject(res, "sandbox");
    if (sandbox_obj) {
        size_t used = 0, peak = 0, limit = 0;
        sandbox_get_mem_stats(&used, &peak, &limit);
        cJSON_AddStringToObject(sandbox_obj, "lua_version", LUA_VERSION);
        cJSON_AddNumberToObject(sandbox_obj, "mem_used", (double)used);
        cJSON_AddNumberToObject(sandbox_obj, "mem_peak", (double)peak);
        cJSON_AddNumberToObject(sandbox_obj, "mem_limit", (double)limit);
    }

    cJSON_AddStringToObject(res, "state", "ready");
    return res;
}

static cJSON *handle_primitive_break(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    (void)params;
    (void)error_code;
    (void)error_msg;
    (void)error_msg_len;

    if (s_break_cb) {
        s_break_cb();
    }
    uint32_t freed = ledger_teardown(LEDGER_OWNER_SCRIPT);

    cJSON *res = cJSON_CreateObject();
    if (!res) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Out of memory");
        return NULL;
    }
    cJSON_AddBoolToObject(res, "interrupted", true);
    cJSON_AddNumberToObject(res, "freed_pins", (double)freed);
    cJSON_AddStringToObject(res, "state", "ready");
    return res;
}

static cJSON *handle_primitive_exec(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    const char *code = rpc_param_get_string(params, 0, "code");
    if (code == NULL || code[0] == '\0') {
        *error_code = RPC_ERR_INVALID_PARAMS;
        snprintf(error_msg, error_msg_len, "Missing required string parameter 'code'");
        return NULL;
    }

    sandbox_result_t res;
    esp_err_t err = sandbox_eval(code, &res);
    if (err != ESP_OK) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Sandbox execution failure (0x%x)", err);
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to allocate JSON result");
        return NULL;
    }

    cJSON_AddBoolToObject(result, "ok", res.success);
    if (res.success) {
        cJSON_AddStringToObject(result, "value", res.output);
    } else {
        cJSON_AddStringToObject(result, "error", res.output);
    }
    cJSON_AddNumberToObject(result, "mem_used", (double)res.mem_used);
    cJSON_AddNumberToObject(result, "mem_peak", (double)res.mem_peak);
    cJSON_AddNumberToObject(result, "insn_count", (double)res.insn_count);

    return result;
}

static cJSON *handle_primitive_read(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    const char *path = rpc_param_get_string(params, 0, "path");
    if (path == NULL || path[0] == '\0') {
        *error_code = RPC_ERR_INVALID_PARAMS;
        snprintf(error_msg, error_msg_len, "Missing required parameter 'path'");
        return NULL;
    }

    char *buf = malloc(STORAGE_READ_MAX_BYTES);
    if (buf == NULL) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Memory allocation failure");
        return NULL;
    }

    size_t read_len = 0;
    esp_err_t err = storage_read_file(path, buf, STORAGE_READ_MAX_BYTES, &read_len);
    if (err != ESP_OK) {
        free(buf);
        *error_code = (err == ESP_ERR_NOT_FOUND) ? RPC_ERR_INVALID_PARAMS : RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to read file '%s' (%s)", path, esp_err_to_name(err));
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        free(buf);
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to allocate JSON result");
        return NULL;
    }

    cJSON_AddStringToObject(result, "path", path);
    cJSON_AddNumberToObject(result, "size", (double)read_len);
    cJSON_AddStringToObject(result, "content", buf);
    free(buf);
    return result;
}

static cJSON *handle_primitive_write(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    const char *path = rpc_param_get_string(params, 0, "path");
    const char *content = rpc_param_get_string(params, 1, "content");

    if (path == NULL || content == NULL) {
        *error_code = RPC_ERR_INVALID_PARAMS;
        snprintf(error_msg, error_msg_len, "Missing required parameters 'path' and 'content'");
        return NULL;
    }

    size_t len = strlen(content);
    esp_err_t err = storage_write_file(path, content, len);
    if (err != ESP_OK) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to write file '%s' (%s)", path, esp_err_to_name(err));
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to allocate JSON result");
        return NULL;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "path", path);
    cJSON_AddNumberToObject(result, "bytes_written", (double)len);
    return result;
}

static cJSON *handle_primitive_delete(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    const char *path = rpc_param_get_string(params, 0, "path");
    if (path == NULL || path[0] == '\0') {
        *error_code = RPC_ERR_INVALID_PARAMS;
        snprintf(error_msg, error_msg_len, "Missing required parameter 'path'");
        return NULL;
    }

    esp_err_t err = storage_delete_file(path);
    if (err != ESP_OK) {
        *error_code = (err == ESP_ERR_NOT_FOUND) ? RPC_ERR_INVALID_PARAMS : RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to delete file '%s' (%s)", path, esp_err_to_name(err));
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to allocate JSON result");
        return NULL;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "path", path);
    return result;
}

static void rpc_list_file_cb(const char *name, size_t size, void *user_data)
{
    cJSON *files_arr = (cJSON *)user_data;
    cJSON *file_obj = cJSON_CreateObject();
    if (file_obj != NULL) {
        cJSON_AddStringToObject(file_obj, "name", name);
        cJSON_AddNumberToObject(file_obj, "size", (double)size);
        cJSON_AddItemToArray(files_arr, file_obj);
    }
}

static cJSON *handle_primitive_list(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    (void)params;
    if (!storage_is_mounted()) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Storage filesystem is not mounted");
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to allocate JSON result");
        return NULL;
    }

    cJSON_AddStringToObject(result, "base_path", STORAGE_BASE_PATH);
    cJSON *files_arr = cJSON_AddArrayToObject(result, "files");
    if (files_arr == NULL) {
        cJSON_Delete(result);
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to allocate JSON array");
        return NULL;
    }

    esp_err_t err = storage_list_files(rpc_list_file_cb, files_arr);
    if (err != ESP_OK) {
        cJSON_Delete(result);
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to enumerate storage directory (%s)", esp_err_to_name(err));
        return NULL;
    }

    return result;
}

typedef struct {
    const char *name;
    rpc_method_handler_t handler;
} rpc_primitive_entry_t;

static const rpc_primitive_entry_t S_PRIMITIVE_LIST[] = {
    {"list",   handle_primitive_list},
    {"ping",   handle_primitive_ping},
    {"read",   handle_primitive_read},
    {"break",  handle_primitive_break},
    {"exec",   handle_primitive_exec},
    {"delete", handle_primitive_delete},
    {"write",  handle_primitive_write},
    {"info",   handle_primitive_info},
};

/*
 * Compile-time static dispatch table (size 16).
 * Hash index: ((unsigned char)method[0] + strlen(method)) & 0x0F
 *
 * Perfect hash slots:
 *   [0]  "list"   (108 + 4 = 112 -> 0)
 *   [4]  "ping"   (112 + 4 = 116 -> 4)
 *   [6]  "read"   (114 + 4 = 118 -> 6)
 *   [7]  "break"  (98  + 5 = 103 -> 7)
 *   [9]  "exec"   (101 + 4 = 105 -> 9)
 *   [10] "delete" (100 + 6 = 106 -> 10)
 *   [12] "write"  (119 + 5 = 124 -> 12)
 *   [13] "info"   (105 + 4 = 109 -> 13)
 */
static const rpc_primitive_entry_t * const S_PRIMITIVE_TABLE[16] = {
    [0]  = &S_PRIMITIVE_LIST[0],
    [4]  = &S_PRIMITIVE_LIST[1],
    [6]  = &S_PRIMITIVE_LIST[2],
    [7]  = &S_PRIMITIVE_LIST[3],
    [9]  = &S_PRIMITIVE_LIST[4],
    [10] = &S_PRIMITIVE_LIST[5],
    [12] = &S_PRIMITIVE_LIST[6],
    [13] = &S_PRIMITIVE_LIST[7],
};

static rpc_method_handler_t rpc_find_primitive_handler(const char *method)
{
    if (method == NULL || method[0] == '\0') {
        return NULL;
    }

    /* Transparent alias support for legacy system.* method names */
    if (strncmp(method, "system.", 7) == 0) {
        method += 7;
    }

    size_t len = strlen(method);
    uint8_t slot = ((uint8_t)method[0] + len) & 0x0F;
    const rpc_primitive_entry_t *entry = S_PRIMITIVE_TABLE[slot];
    if (entry != NULL && strcmp(entry->name, method) == 0) {
        return entry->handler;
    }
    return NULL;
}

static void rpc_dispatch_line(const char *line)
{
    cJSON *req = cJSON_Parse(line);
    if (!req) {
        rpc_send_error_raw(NULL, RPC_ERR_PARSE_ERROR, "Parse error: invalid JSON");
        return;
    }

    cJSON *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");
    if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0) {
        rpc_send_error_raw(NULL, RPC_ERR_INVALID_REQUEST, "Invalid Request: missing or bad jsonrpc version");
        cJSON_Delete(req);
        return;
    }

    cJSON *method = cJSON_GetObjectItem(req, "method");
    if (!cJSON_IsString(method) || !method->valuestring) {
        rpc_send_error_raw(NULL, RPC_ERR_INVALID_REQUEST, "Invalid Request: method must be string");
        cJSON_Delete(req);
        return;
    }

    cJSON *id = cJSON_GetObjectItem(req, "id");
    bool is_notification = (id == NULL);

    rpc_method_handler_t target_handler = rpc_find_primitive_handler(method->valuestring);

    if (!target_handler) {
        if (!is_notification) {
            rpc_send_error_raw(id, RPC_ERR_METHOD_NOT_FOUND, "Method not found");
        }
        cJSON_Delete(req);
        return;
    }

    cJSON *params = cJSON_GetObjectItem(req, "params");
    int error_code = 0;
    char error_msg[128] = {0};

    cJSON *result = target_handler(params, &error_code, error_msg, sizeof(error_msg));

    if (!is_notification) {
        if (error_code != 0) {
            rpc_send_error_raw(id, error_code, error_msg[0] ? error_msg : "Execution error");
        } else {
            cJSON *resp = cJSON_CreateObject();
            if (resp) {
                cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
                cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
                if (result) {
                    cJSON_AddItemToObject(resp, "result", result);
                } else {
                    cJSON_AddNullToObject(resp, "result");
                }
                char *rendered = cJSON_PrintUnformatted(resp);
                if (rendered) {
                    rpc_send_raw(rendered);
                    free(rendered);
                }
                cJSON_Delete(resp);
            }
        }
    } else {
        if (result) {
            cJSON_Delete(result);
        }
    }

    cJSON_Delete(req);
}

static void rpc_task(void *pvParameters)
{
    (void)pvParameters;
    static char line_buf[RPC_LINE_BUF_SIZE];
    size_t line_len = 0;
    uint8_t chunk[64];

    ESP_LOGI(TAG, "RPC listening task started on Core %d (stack %d B)", xPortGetCoreID(), RPC_TASK_STACK_SIZE);

    while (1) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }

        for (int i = 0; i < n; i++) {
            uint8_t ch = chunk[i];

            if (ch == 0x03) {
                ESP_LOGI(TAG, "Ctrl-C (0x03) received, executing break");
                if (s_break_cb) {
                    s_break_cb();
                }
                uint32_t freed = ledger_teardown(LEDGER_OWNER_SCRIPT);
                cJSON *evt_params = cJSON_CreateObject();
                if (evt_params) {
                    cJSON_AddStringToObject(evt_params, "reason", "ctrl_c");
                    cJSON_AddNumberToObject(evt_params, "freed_pins", (double)freed);
                    rpc_notify("event.break", evt_params);
                }
                line_len = 0;
                continue;
            }

            if (ch == '\n' || ch == '\r') {
                if (line_len > 0) {
                    line_buf[line_len] = '\0';
                    rpc_dispatch_line(line_buf);
                    line_len = 0;
                }
            } else {
                if (line_len < sizeof(line_buf) - 1) {
                    line_buf[line_len++] = (char)ch;
                } else {
                    line_len = 0;
                    rpc_send_error_raw(NULL, RPC_ERR_INVALID_REQUEST, "Line buffer overflow (>2048 bytes)");
                }
            }
        }
    }
}

esp_err_t rpc_notify(const char *event_name, cJSON *params)
{
    if (!event_name) {
        if (params) {
            cJSON_Delete(params);
        }
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *notify = cJSON_CreateObject();
    if (!notify) {
        if (params) {
            cJSON_Delete(params);
        }
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(notify, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notify, "method", event_name);
    if (params) {
        cJSON_AddItemToObject(notify, "params", params);
    }

    char *rendered = cJSON_PrintUnformatted(notify);
    if (rendered) {
        rpc_send_raw(rendered);
        free(rendered);
    }
    cJSON_Delete(notify);
    return ESP_OK;
}

void rpc_set_break_callback(rpc_break_callback_t cb)
{
    s_break_cb = cb;
}

esp_err_t rpc_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = RPC_LINE_BUF_SIZE;
    cfg.tx_buffer_size = RPC_LINE_BUF_SIZE;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB-Serial/JTAG driver: %s", esp_err_to_name(err));
        return err;
    }
    usb_serial_jtag_vfs_use_driver();

    s_initialized = true;

    BaseType_t res = xTaskCreatePinnedToCore(
        rpc_task,
        "rpc_engine",
        RPC_TASK_STACK_SIZE,
        NULL,
        RPC_TASK_PRIORITY,
        &s_rpc_task_handle,
        0
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn rpc task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "JSON-RPC 2.0 engine online with 7 Pi-style table primitives (O(1))");
    return ESP_OK;
}
