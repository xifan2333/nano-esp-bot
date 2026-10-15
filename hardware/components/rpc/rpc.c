#include "rpc.h"
#include "ledger.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "rpc";

#define RPC_LINE_BUF_SIZE    2048
#define RPC_MAX_METHODS      32
/*
 * The control plane task must be sized for the deepest handler it can run.
 *
 * RPC method handlers execute *inline* on this task, and `sandbox.eval` runs the
 * full Lua pipeline here: the expression pre-pass buffer, the recursive-descent
 * parser (LexState/FuncState/MBuffer live on the C stack) and the VM call path.
 * A 4096 byte stack overflowed during on-hardware verification and silently
 * corrupted the adjacent heap, which then faulted later inside
 * usb_serial_jtag_write_bytes() (assert: pxQueue->uxItemSize == 0).
 *
 * Keep this comfortably generous; stack high-water telemetry is exposed through
 * system.info -> rpc_stack_headroom so the margin can be verified on real
 * hardware instead of guessed.
 */
#define RPC_TASK_STACK_SIZE  16384
#define RPC_TASK_PRIORITY    10

typedef struct {
    char method[32];
    rpc_method_handler_t handler;
} rpc_method_entry_t;

static rpc_method_entry_t s_methods[RPC_MAX_METHODS];
static size_t s_method_count = 0;
static SemaphoreHandle_t s_rpc_mutex = NULL;
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
 * Exposed through system.info so the real margin can be checked on hardware.
 */
static uint32_t rpc_get_task_stack_headroom(void)
{
    if (s_rpc_task_handle == NULL) {
        return 0;
    }
    return (uint32_t)uxTaskGetStackHighWaterMark(s_rpc_task_handle);
}

static cJSON *handle_system_ping(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
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

static cJSON *handle_system_info(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
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
    cJSON_AddStringToObject(res, "state", "ready");
    return res;
}

static cJSON *handle_system_break(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
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

    rpc_method_handler_t target_handler = NULL;
    if (xSemaphoreTake(s_rpc_mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < s_method_count; i++) {
            if (strcmp(s_methods[i].method, method->valuestring) == 0) {
                target_handler = s_methods[i].handler;
                break;
            }
        }
        xSemaphoreGive(s_rpc_mutex);
    }

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

            // MicroPython style in-band Ctrl-C break (0x03)
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

esp_err_t rpc_register_method(const char *method, rpc_method_handler_t handler)
{
    if (!s_initialized || !method || !handler) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_rpc_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_method_count >= RPC_MAX_METHODS) {
        xSemaphoreGive(s_rpc_mutex);
        ESP_LOGE(TAG, "Method registry full (max %d)", RPC_MAX_METHODS);
        return ESP_ERR_NO_MEM;
    }

    strncpy(s_methods[s_method_count].method, method, sizeof(s_methods[s_method_count].method) - 1);
    s_methods[s_method_count].method[sizeof(s_methods[s_method_count].method) - 1] = '\0';
    s_methods[s_method_count].handler = handler;
    s_method_count++;

    xSemaphoreGive(s_rpc_mutex);
    ESP_LOGD(TAG, "Registered method: %s", method);
    return ESP_OK;
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

    s_rpc_mutex = xSemaphoreCreateMutex();
    if (!s_rpc_mutex) {
        ESP_LOGE(TAG, "Failed to create rpc mutex");
        return ESP_ERR_NO_MEM;
    }

    // Configure USB Serial JTAG Driver
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

    // Register built-in system methods
    rpc_register_method("system.ping", handle_system_ping);
    rpc_register_method("system.info", handle_system_info);
    rpc_register_method("system.break", handle_system_break);

    // Spawn background RPC task (Priority 10, Core 0)
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

    ESP_LOGI(TAG, "JSON-RPC 2.0 engine initialized successfully");
    return ESP_OK;
}
