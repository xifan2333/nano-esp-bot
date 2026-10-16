#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_http(lua_State *L);

esp_err_t chal_http_request(const char *url, const char *method,
                            const char *req_body, size_t req_body_len,
                            const char **headers, size_t header_count,
                            int timeout_ms, int *out_status,
                            char **out_resp_body, size_t *out_resp_len);

esp_err_t chal_http_download(const char *url, const char *rel_filepath,
                             int timeout_ms, int *out_status,
                             size_t *out_downloaded_bytes);

void chal_http_teardown(void);

#ifdef __cplusplus
}
#endif
