#ifndef SANDBOX_H
#define SANDBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SANDBOX_DEFAULT_MEM_LIMIT (64 * 1024)   /* 64 KB default memory budget */
#define SANDBOX_DEFAULT_INSN_QUOTA 100000       /* 100k instruction hook quota */

/**
 * @brief Initialize the global Lua sandbox subsystem.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t sandbox_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SANDBOX_H */
