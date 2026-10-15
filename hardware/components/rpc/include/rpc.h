#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Standard JSON-RPC 2.0 Error Codes
#define RPC_ERR_PARSE_ERROR      (-32700)
#define RPC_ERR_INVALID_REQUEST  (-32600)
#define RPC_ERR_METHOD_NOT_FOUND (-32601)
#define RPC_ERR_INVALID_PARAMS   (-32602)
#define RPC_ERR_INTERNAL_ERROR   (-32603)

// Custom Bot Error Codes
#define RPC_ERR_HARDWARE_BUSY    (-32001)
#define RPC_ERR_PIN_CONFLICT     (-32002)
#define RPC_ERR_TIMEOUT          (-32003)

/**
 * @brief Handler signature for JSON-RPC methods.
 * @param params The "params" cJSON object from request (may be NULL).
 * @param error_code Pointer to set integer error code (0 on success).
 * @param error_msg Buffer to write error message if error_code != 0.
 * @param error_msg_len Size of error_msg buffer.
 * @return cJSON* result object on success (ownership transferred to caller), or NULL on error.
 */
typedef cJSON *(*rpc_method_handler_t)(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len);

/**
 * @brief Break callback signature.
 */
typedef void (*rpc_break_callback_t)(void);

/**
 * @brief Initialize the JSON-RPC engine, install USB-Serial/JTAG driver, and start background task.
 * @return ESP_OK on success.
 */
esp_err_t rpc_init(void);

/**
 * @brief Register a JSON-RPC method handler.
 * @param method Method name (e.g. "system.ping", "system.info").
 * @param handler Function pointer to method handler.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if registry full, ESP_ERR_INVALID_ARG if invalid params.
 */
esp_err_t rpc_register_method(const char *method, rpc_method_handler_t handler);

/**
 * @brief Send a JSON-RPC notification (no id).
 * @param event_name Event method name (e.g. "event.push").
 * @param params cJSON object with parameters (ownership transferred to rpc_notify, caller must not delete).
 * @return ESP_OK on success.
 */
esp_err_t rpc_notify(const char *event_name, cJSON *params);

/**
 * @brief Set the break callback (triggered when Ctrl-C or system.break is received).
 */
void rpc_set_break_callback(rpc_break_callback_t cb);

#ifdef __cplusplus
}
#endif
