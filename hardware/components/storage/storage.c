#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rpc.h"
#include "cJSON.h"

static const char *TAG = "storage";

#define STORAGE_READ_MAX_BYTES (16 * 1024)

static SemaphoreHandle_t s_storage_mutex = NULL;
static bool s_mounted = false;

static esp_err_t resolve_path(const char *rel_path, char *out_full, size_t out_size)
{
    if (rel_path == NULL || out_full == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Prevent directory traversal attacks */
    if (strstr(rel_path, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Strip leading slash if present */
    while (*rel_path == '/') {
        rel_path++;
    }

    if (strlen(rel_path) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out_full, out_size, "%s/%s", STORAGE_BASE_PATH, rel_path);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t storage_init(void)
{
    if (s_storage_mutex == NULL) {
        s_storage_mutex = xSemaphoreCreateMutex();
        if (s_storage_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    if (s_mounted) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_BASE_PATH,
        .partition_label = STORAGE_PARTITION_LABEL,
        .max_files = STORAGE_MAX_OPEN_FILES,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        xSemaphoreGive(s_storage_mutex);
        return ret;
    }

    s_mounted = true;
    size_t total = 0, used = 0;
    esp_spiffs_info(STORAGE_PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted at %s (total: %u KB, used: %u KB)",
             STORAGE_BASE_PATH, (unsigned int)(total / 1024), (unsigned int)(used / 1024));

    xSemaphoreGive(s_storage_mutex);
    return ESP_OK;
}

esp_err_t storage_deinit(void)
{
    if (s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    if (!s_mounted) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_spiffs_unregister(STORAGE_PARTITION_LABEL);
    if (ret == ESP_OK) {
        s_mounted = false;
        ESP_LOGI(TAG, "SPIFFS unmounted successfully");
    }

    xSemaphoreGive(s_storage_mutex);
    return ret;
}

bool storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t storage_get_info(size_t *out_total, size_t *out_used)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_spiffs_info(STORAGE_PARTITION_LABEL, out_total, out_used);
}

bool storage_file_exists(const char *rel_path)
{
    char full_path[STORAGE_PATH_MAX_LEN];
    if (resolve_path(rel_path, full_path, sizeof(full_path)) != ESP_OK) {
        return false;
    }

    struct stat st;
    return (stat(full_path, &st) == 0 && S_ISREG(st.st_mode));
}

esp_err_t storage_read_file(const char *rel_path, char *out_buf, size_t buf_size, size_t *out_len)
{
    if (rel_path == NULL || out_buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[STORAGE_PATH_MAX_LEN];
    esp_err_t err = resolve_path(rel_path, full_path, sizeof(full_path));
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    FILE *f = fopen(full_path, "r");
    if (f == NULL) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    size_t read_bytes = fread(out_buf, 1, buf_size - 1, f);
    out_buf[read_bytes] = '\0';
    if (out_len != NULL) {
        *out_len = read_bytes;
    }

    fclose(f);
    xSemaphoreGive(s_storage_mutex);
    return ESP_OK;
}

esp_err_t storage_write_file(const char *rel_path, const char *data, size_t data_len)
{
    if (rel_path == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[STORAGE_PATH_MAX_LEN];
    esp_err_t err = resolve_path(rel_path, full_path, sizeof(full_path));
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    FILE *f = fopen(full_path, "w");
    if (f == NULL) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);
    xSemaphoreGive(s_storage_mutex);

    if (written != data_len) {
        ESP_LOGE(TAG, "Write incomplete: %zu of %zu bytes written", written, data_len);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t storage_delete_file(const char *rel_path)
{
    char full_path[STORAGE_PATH_MAX_LEN];
    esp_err_t err = resolve_path(rel_path, full_path, sizeof(full_path));
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int ret = unlink(full_path);
    xSemaphoreGive(s_storage_mutex);

    return (ret == 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static cJSON *storage_rpc_info_handler(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    (void)params;
    size_t total = 0, used = 0;
    esp_err_t err = storage_get_info(&total, &used);
    if (err != ESP_OK) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Storage not available (0x%x)", err);
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to allocate JSON result");
        return NULL;
    }

    cJSON_AddBoolToObject(result, "mounted", true);
    cJSON_AddStringToObject(result, "base_path", STORAGE_BASE_PATH);
    cJSON_AddNumberToObject(result, "total_bytes", (double)total);
    cJSON_AddNumberToObject(result, "used_bytes", (double)used);
    cJSON_AddNumberToObject(result, "free_bytes", (double)(total >= used ? total - used : 0));

    return result;
}

static cJSON *storage_rpc_list_handler(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    (void)params;
    if (!s_mounted) {
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Storage filesystem is not mounted");
        return NULL;
    }

    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        *error_code = RPC_ERR_TIMEOUT;
        snprintf(error_msg, error_msg_len, "Storage mutex timeout");
        return NULL;
    }

    DIR *d = opendir(STORAGE_BASE_PATH);
    if (d == NULL) {
        xSemaphoreGive(s_storage_mutex);
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to open storage directory");
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        closedir(d);
        xSemaphoreGive(s_storage_mutex);
        *error_code = RPC_ERR_INTERNAL_ERROR;
        snprintf(error_msg, error_msg_len, "Failed to allocate JSON result");
        return NULL;
    }

    cJSON_AddStringToObject(result, "base_path", STORAGE_BASE_PATH);
    cJSON *files_arr = cJSON_AddArrayToObject(result, "files");

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
            char full_path[STORAGE_PATH_MAX_LEN];
            snprintf(full_path, sizeof(full_path), "%s/%s", STORAGE_BASE_PATH, entry->d_name);
            struct stat st;
            size_t file_size = 0;
            if (stat(full_path, &st) == 0) {
                file_size = (size_t)st.st_size;
            }

            cJSON *file_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(file_obj, "name", entry->d_name);
            cJSON_AddNumberToObject(file_obj, "size", (double)file_size);
            cJSON_AddItemToArray(files_arr, file_obj);
        }
    }

    closedir(d);
    xSemaphoreGive(s_storage_mutex);
    return result;
}

static cJSON *storage_rpc_read_handler(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    const char *path = NULL;
    if (params != NULL) {
        if (cJSON_IsString(params)) {
            path = params->valuestring;
        } else if (cJSON_IsObject(params)) {
            const cJSON *item = cJSON_GetObjectItem(params, "path");
            if (item && cJSON_IsString(item)) {
                path = item->valuestring;
            }
        }
    }

    if (path == NULL || strlen(path) == 0) {
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

static cJSON *storage_rpc_write_handler(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    const char *path = NULL;
    const char *content = NULL;

    if (params != NULL && cJSON_IsObject(params)) {
        const cJSON *p_item = cJSON_GetObjectItem(params, "path");
        const cJSON *c_item = cJSON_GetObjectItem(params, "content");
        if (p_item && cJSON_IsString(p_item)) {
            path = p_item->valuestring;
        }
        if (c_item && cJSON_IsString(c_item)) {
            content = c_item->valuestring;
        }
    }

    if (path == NULL || content == NULL) {
        *error_code = RPC_ERR_INVALID_PARAMS;
        snprintf(error_msg, error_msg_len, "Missing required string parameters 'path' and 'content'");
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

static cJSON *storage_rpc_delete_handler(const cJSON *params, int *error_code, char *error_msg, size_t error_msg_len)
{
    const char *path = NULL;
    if (params != NULL) {
        if (cJSON_IsString(params)) {
            path = params->valuestring;
        } else if (cJSON_IsObject(params)) {
            const cJSON *item = cJSON_GetObjectItem(params, "path");
            if (item && cJSON_IsString(item)) {
                path = item->valuestring;
            }
        }
    }

    if (path == NULL) {
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

esp_err_t storage_register_rpc(void)
{
    esp_err_t err = rpc_register_method("storage.info", storage_rpc_info_handler);
    if (err != ESP_OK) return err;

    err = rpc_register_method("storage.list", storage_rpc_list_handler);
    if (err != ESP_OK) return err;

    err = rpc_register_method("storage.read", storage_rpc_read_handler);
    if (err != ESP_OK) return err;

    err = rpc_register_method("storage.write", storage_rpc_write_handler);
    if (err != ESP_OK) return err;

    err = rpc_register_method("storage.delete", storage_rpc_delete_handler);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Registered RPC methods: storage.info, storage.list, storage.read, storage.write, storage.delete");
    return ESP_OK;
}
