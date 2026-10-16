#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*chal_nimble_sync_cb_t)(void);

esp_err_t chal_nimble_ensure_inited(void);
esp_err_t chal_nimble_start_host(void);
esp_err_t chal_nimble_deinit(void);

bool chal_nimble_is_inited(void);
bool chal_nimble_is_synced(void);

esp_err_t chal_nimble_register_sync_cb(chal_nimble_sync_cb_t cb);

#ifdef __cplusplus
}
#endif
