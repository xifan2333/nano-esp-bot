#ifndef SANDBOX_H
#define SANDBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SANDBOX_DEFAULT_MEM_LIMIT   (64 * 1024)   /* 64 KB memory limit */
#define SANDBOX_DEFAULT_INSN_QUOTA  100000        /* 100,000 instruction hook quota */
#define SANDBOX_RESULT_MAX_LEN      512

typedef struct {
    bool success;
    char output[SANDBOX_RESULT_MAX_LEN];
    size_t mem_used;
    size_t mem_peak;
    uint32_t insn_count;
} sandbox_result_t;

/**
 * @brief Initialize the global Lua sandbox subsystem.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t sandbox_init(void);

/**
 * @brief Reset the Lua VM sandbox to an initial clean state.
 *
 * Tears down any existing Lua state, clears memory tracking, and initializes
 * a fresh sandbox environment.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t sandbox_reset(void);

/**
 * @brief Evaluate a Lua code snippet inside the sandbox.
 *
 * Supports expressions (e.g. "1 + 1") and statements (e.g. "x = 10\nreturn x * 2").
 *
 * @param code Null-terminated Lua source code string.
 * @param out_result Output struct containing success status, result string, memory & instruction metrics.
 * @return esp_err_t ESP_OK if evaluation finished (check out_result->success for Lua runtime status).
 */
esp_err_t sandbox_eval(const char *code, sandbox_result_t *out_result);

/**
 * @brief Request an immediate break/abort of any currently running Lua execution.
 *
 * Sets the abort flag and forces a hook interrupt on the next instruction.
 * Also cleans up any script-allocated ledger pins via ledger_teardown(LEDGER_OWNER_SCRIPT).
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t sandbox_break(void);

/**
 * @brief Query current sandbox memory metrics.
 *
 * @param out_used Pointer to receive currently allocated bytes (optional, may be NULL).
 * @param out_peak Pointer to receive peak allocated bytes (optional, may be NULL).
 * @param out_limit Pointer to receive maximum allowed bytes (optional, may be NULL).
 */
void sandbox_get_mem_stats(size_t *out_used, size_t *out_peak, size_t *out_limit);

/**
 * @brief Register sandbox JSON-RPC 2.0 methods (sandbox.eval, sandbox.reset, sandbox.info).
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t sandbox_register_rpc(void);

#ifdef __cplusplus
}
#endif

#endif /* SANDBOX_H */
