#include "chal_http.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "storage.h"
#include "lauxlib.h"

static const char *TAG = "chal_http";

#define HTTP_MAX_IN_MEMORY_BODY (32 * 1024)
#define HTTP_STREAM_CHUNK_SIZE  1024

static SemaphoreHandle_t s_http_mutex = NULL;
static esp_http_client_handle_t s_active_client = NULL;
static volatile bool s_abort_requested = false;

static esp_err_t resolve_storage_path(const char *rel_path, char *out_full, size_t out_size)
{
    if (!rel_path || !out_full || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strstr(rel_path, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(rel_path, STORAGE_BASE_PATH, strlen(STORAGE_BASE_PATH)) == 0) {
        snprintf(out_full, out_size, "%s", rel_path);
        return ESP_OK;
    }

    while (*rel_path == '/') {
        rel_path++;
    }

    int written = snprintf(out_full, out_size, "%s/%s", STORAGE_BASE_PATH, rel_path);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t chal_http_request(const char *url, const char *method,
                            const char *req_body, size_t req_body_len,
                            const char **headers, size_t header_count,
                            int timeout_ms, int *out_status,
                            char **out_resp_body, size_t *out_resp_len)
{
    if (!url || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_http_mutex) {
        s_http_mutex = xSemaphoreCreateMutex();
        if (!s_http_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_http_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_abort_requested = false;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };

    if (method) {
        if (strcasecmp(method, "POST") == 0) {
            config.method = HTTP_METHOD_POST;
        } else if (strcasecmp(method, "PUT") == 0) {
            config.method = HTTP_METHOD_PUT;
        } else if (strcasecmp(method, "DELETE") == 0) {
            config.method = HTTP_METHOD_DELETE;
        } else if (strcasecmp(method, "HEAD") == 0) {
            config.method = HTTP_METHOD_HEAD;
        } else {
            config.method = HTTP_METHOD_GET;
        }
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        xSemaphoreGive(s_http_mutex);
        return ESP_FAIL;
    }

    s_active_client = client;

    if (headers && header_count > 0) {
        for (size_t i = 0; i < header_count; i += 2) {
            if (headers[i] && headers[i + 1]) {
                esp_http_client_set_header(client, headers[i], headers[i + 1]);
            }
        }
    }

    if (req_body && req_body_len > 0) {
        esp_http_client_set_post_field(client, req_body, (int)req_body_len);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        *out_status = esp_http_client_get_status_code(client);
        int content_len = esp_http_client_get_content_length(client);

        if (out_resp_body && out_resp_len) {
            if (content_len < 0) {
                content_len = 0;
            }
            if (content_len > HTTP_MAX_IN_MEMORY_BODY) {
                ESP_LOGW(TAG, "Content length (%d) exceeds max in-memory limit", content_len);
                content_len = HTTP_MAX_IN_MEMORY_BODY;
            }

            char *buf = malloc(content_len + 1);
            if (buf) {
                int read_len = esp_http_client_read_response(client, buf, content_len);
                if (read_len < 0) {
                    read_len = 0;
                }
                buf[read_len] = '\0';
                *out_resp_body = buf;
                *out_resp_len = (size_t)read_len;
            } else {
                *out_resp_body = NULL;
                *out_resp_len = 0;
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        *out_status = 0;
        if (out_resp_body) *out_resp_body = NULL;
        if (out_resp_len) *out_resp_len = 0;
    }

    s_active_client = NULL;
    esp_http_client_cleanup(client);
    xSemaphoreGive(s_http_mutex);
    return err;
}

esp_err_t chal_http_download(const char *url, const char *rel_filepath,
                             int timeout_ms, int *out_status,
                             size_t *out_downloaded_bytes)
{
    if (!url || !rel_filepath || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[STORAGE_PATH_MAX_LEN];
    esp_err_t err = resolve_storage_path(rel_filepath, full_path, sizeof(full_path));
    if (err != ESP_OK) {
        return err;
    }

    if (!s_http_mutex) {
        s_http_mutex = xSemaphoreCreateMutex();
        if (!s_http_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_http_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_abort_requested = false;

    FILE *fp = fopen(full_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open output file: %s", full_path);
        xSemaphoreGive(s_http_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .buffer_size = HTTP_STREAM_CHUNK_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(fp);
        xSemaphoreGive(s_http_mutex);
        return ESP_FAIL;
    }

    s_active_client = client;

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        fclose(fp);
        s_active_client = NULL;
        esp_http_client_cleanup(client);
        xSemaphoreGive(s_http_mutex);
        return err;
    }

    esp_http_client_fetch_headers(client);
    *out_status = esp_http_client_get_status_code(client);

    size_t total_bytes = 0;
    char chunk[HTTP_STREAM_CHUNK_SIZE];

    while (!s_abort_requested) {
        int read_len = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read_len < 0) {
            ESP_LOGE(TAG, "Error reading HTTP stream");
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            // End of stream
            err = ESP_OK;
            break;
        }

        size_t written = fwrite(chunk, 1, read_len, fp);
        if (written != (size_t)read_len) {
            ESP_LOGE(TAG, "Flash write failed (%zu/%d bytes)", written, read_len);
            err = ESP_ERR_NO_MEM;
            break;
        }
        total_bytes += written;
    }

    fclose(fp);
    esp_http_client_close(client);
    s_active_client = NULL;
    esp_http_client_cleanup(client);

    if (out_downloaded_bytes) {
        *out_downloaded_bytes = total_bytes;
    }

    xSemaphoreGive(s_http_mutex);
    return err;
}

void chal_http_teardown(void)
{
    s_abort_requested = true;
    if (s_active_client) {
        esp_http_client_close(s_active_client);
    }
}

int luaopen_http(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
