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

static const char *TAG = "storage";

#define STORAGE_READ_MAX_BYTES (16 * 1024)

static SemaphoreHandle_t s_storage_mutex = NULL;
static bool s_mounted = false;

static esp_err_t resolve_path(const char *rel_path, char *out_full, size_t out_size)
{
    if (rel_path == NULL || out_full == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strstr(rel_path, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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

esp_err_t storage_list_files(storage_list_cb_t cb, void *user_data)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    DIR *d = opendir(STORAGE_BASE_PATH);
    if (d == NULL) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_FAIL;
    }

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
            cb(entry->d_name, file_size, user_data);
        }
    }

    closedir(d);
    xSemaphoreGive(s_storage_mutex);
    return ESP_OK;
}
